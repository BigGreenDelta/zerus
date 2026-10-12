//! Reaching other machines for blobs and deliveries, over the swarm channel.
use super::blobs;
use super::commands::{self, Ctx, MAX_ATTACHMENT, MAX_ATTACHMENTS, MAX_ATTACHMENTS_TOTAL};
use super::deliver;
use super::fold::{Attachment, Task};
use super::model::{Actor, ActorKind, Refusal};
use super::store::Store;
use crate::config::Config;
use anyhow::{anyhow, bail, ensure, Result};
use base64::Engine;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};

pub const RANGE: usize = 512 * 1024;
const MAX_WANT: usize = 64;

fn unavailable(attachment: &Attachment, last: &str) -> anyhow::Error {
    let detail = if last.is_empty() { String::new() } else { format!(" (last error: {last})") };
    Refusal::field("attachment_unavailable", "attachments", "missing", format!("no reachable machine holds the image {}{detail}", attachment.name)).into()
}

/// Serves one range of a stored blob, reading only that range. A blob this
/// machine lacks is an answer, not a failed call, so the caller still asks this
/// machine for other images.
pub fn serve_blob(store: &Store, request: &Value) -> Result<Value> {
    let sha256 = request["sha256"].as_str().unwrap_or_default();
    let offset = request["offset"].as_u64().unwrap_or(0);
    let length = request["length"].as_u64().unwrap_or(RANGE as u64).min(RANGE as u64);
    let Ok(mut file) = File::open(blobs::path(store.dir(), sha256)?) else {
        return Ok(json!({"missing": true}));
    };
    let size = file.metadata()?.len();
    ensure!(offset <= size, "invalid range");
    // Blobs never change, so the range is read exactly.
    let mut bytes = vec![0; length.min(size - offset) as usize];
    file.seek(SeekFrom::Start(offset))?;
    file.read_exact(&mut bytes)?;
    Ok(json!({"size": size, "data_base64": base64::engine::general_purpose::STANDARD.encode(&bytes)}))
}

/// Decodes base64 that may hold at most `max` bytes, refusing a longer text
/// before decoding it.
fn decode_at_most(text: &str, max: usize, too_long: &str) -> Result<Vec<u8>> {
    ensure!(text.len() <= max.div_ceil(3) * 4, "{too_long}");
    let bytes = base64::engine::general_purpose::STANDARD.decode(text)?;
    ensure!(bytes.len() <= max, "{too_long}");
    Ok(bytes)
}

/// Stores pushed blobs after checking their digests; answers which wanted ones are missing.
pub fn serve_blobs(store: &Store, request: &Value) -> Result<Value> {
    let puts = request["put"].as_array().map(Vec::as_slice).unwrap_or_default();
    ensure!(puts.len() <= MAX_ATTACHMENTS, "too many pushed images in one call");
    ensure!(request["want"].as_array().map_or(0, Vec::len) <= MAX_WANT, "too many wanted images in one call");
    // Everything is decoded and checked before the first write.
    let mut decoded = Vec::new();
    let mut total = 0usize;
    for item in puts {
        let text = item["data_base64"].as_str().unwrap_or_default();
        let bytes = decode_at_most(text, MAX_ATTACHMENT, "a pushed image exceeds the per-image limit")?;
        total += bytes.len();
        ensure!(total <= MAX_ATTACHMENTS_TOTAL, "the pushed images exceed the total limit");
        ensure!(format!("{:x}", Sha256::digest(&bytes)) == item["sha256"].as_str().unwrap_or_default(), "a pushed image does not match its digest");
        decoded.push(bytes);
    }
    for bytes in decoded {
        blobs::put(store.dir(), &bytes)?;
    }
    let missing: Vec<&str> = request["want"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(Value::as_str)
        .filter(|sha| blobs::path(store.dir(), sha).map_or(true, |path| !path.exists()))
        .collect();
    Ok(json!({"missing": missing}))
}

fn present(store: &Store, attachment: &Attachment) -> bool {
    blobs::path(store.dir(), &attachment.sha256).is_ok_and(|path| path.exists())
}

/// Appends one served range, refusing sizes the task does not announce.
fn accept_chunk(bytes: &mut Vec<u8>, chunk: &[u8], size: Option<u64>, expected: u64) -> Result<()> {
    ensure!(size == Some(expected), "the peer holds an image of another size");
    ensure!(!chunk.is_empty(), "the peer returned an empty range");
    ensure!(chunk.len() <= RANGE, "the peer returned more than the requested range");
    ensure!((bytes.len() + chunk.len()) as u64 <= expected, "the peer returned more bytes than the image has");
    bytes.extend_from_slice(chunk);
    Ok(())
}

/// Fetches one image through `call` (one `overmind-blob` request per range).
fn fetch(store: &Store, attachment: &Attachment, call: &mut dyn FnMut(&Value) -> Result<Value>) -> Result<()> {
    ensure!(attachment.bytes <= MAX_ATTACHMENT as u64, "the image is larger than the per-image limit");
    let mut bytes = Vec::new();
    while (bytes.len() as u64) < attachment.bytes {
        let part = call(&json!({"sha256": attachment.sha256, "offset": bytes.len(), "length": RANGE}))?;
        ensure!(part["missing"] != true, "the peer does not hold the image");
        let text = part["data_base64"].as_str().unwrap_or_default();
        let chunk = decode_at_most(text, RANGE, "the peer returned more than the requested range")?;
        accept_chunk(&mut bytes, &chunk, part["size"].as_u64(), attachment.bytes)?;
    }
    ensure!(format!("{:x}", Sha256::digest(&bytes)) == attachment.sha256, "a fetched image does not match its digest");
    blobs::put(store.dir(), &bytes)?;
    Ok(())
}

/// Fetches every image of `task` that this machine lacks, trying the
/// creator's machine first and then every other peer.
pub fn ensure_blobs(store: &Store, task: &Task) -> Result<()> {
    let missing: Vec<&Attachment> = task.attachments.iter().filter(|a| !present(store, a)).collect();
    if missing.is_empty() {
        return Ok(());
    }
    let config = Config::load().map_err(|e| anyhow!(e.message))?;
    let mut aliases: Vec<String> = Vec::new();
    if let Some(alias) = crate::swarm::alias_for(&config, &task.created_by.machine) {
        aliases.push(alias);
    }
    for peer in store.peers()? {
        if !peer.alias.is_empty() && !aliases.contains(&peer.alias) {
            aliases.push(peer.alias);
        }
    }
    fetch_all(store, &missing, &aliases, &mut |alias, request| crate::swarm::call(&config, alias, "overmind-blob", Some(request)))
}

/// Asks `aliases` in order for each image. A peer whose call failed is not asked
/// again for the remaining images; a peer without one image, or with a wrong
/// copy, is still asked for the others.
fn fetch_all(
    store: &Store,
    missing: &[&Attachment],
    aliases: &[String],
    call: &mut dyn FnMut(&str, &Value) -> Result<Value>,
) -> Result<()> {
    let mut failed: Vec<&str> = Vec::new();
    for attachment in missing {
        let mut last = String::new();
        let mut found = false;
        for alias in aliases {
            if failed.contains(&alias.as_str()) {
                continue;
            }
            let mut down = false;
            let result = fetch(store, attachment, &mut |request| call(alias, request).inspect_err(|_| down = true));
            match result {
                Ok(()) => {
                    found = true;
                    break;
                }
                Err(error) => {
                    last = format!("{alias}: {error:#}");
                    if down {
                        failed.push(alias);
                    }
                }
            }
        }
        if !found {
            return Err(unavailable(attachment, &last));
        }
    }
    Ok(())
}

/// Gives the machine behind `alias` the task's images it lacks, fetching them
/// here first when this machine lacks them too.
pub fn push_blobs(config: &Config, alias: &str, store: &Store, task: &Task) -> Result<()> {
    let want: Vec<&str> = task.attachments.iter().map(|a| a.sha256.as_str()).collect();
    if want.is_empty() {
        return Ok(());
    }
    let answer = crate::swarm::call(config, alias, "overmind-blobs", Some(&json!({"want": want})))?;
    let mut lacking = Vec::new();
    for sha in answer["missing"].as_array().into_iter().flatten().filter_map(Value::as_str) {
        let attachment = task.attachments.iter().find(|a| a.sha256 == sha).ok_or_else(|| anyhow!("unexpected image request"))?;
        lacking.push(attachment.clone());
    }
    if lacking.is_empty() {
        return Ok(());
    }
    ensure_blobs(store, &Task { attachments: lacking.clone(), ..task.clone() })?;
    let mut put = Vec::new();
    for attachment in &lacking {
        let bytes = std::fs::read(blobs::path(store.dir(), &attachment.sha256)?).map_err(|_| unavailable(attachment, ""))?;
        put.push(json!({"sha256": attachment.sha256, "data_base64": base64::engine::general_purpose::STANDARD.encode(bytes)}));
    }
    crate::swarm::call(config, alias, "overmind-blobs", Some(&json!({"put": put})))?;
    Ok(())
}

/// Runs a sending command on the host of a session on another machine (spec
/// Part 5): brings the host up to date, gives it the task's images, runs the
/// command there as a person and pulls the claim and its outcome back.
pub fn forward(store: &mut Store, command: &str, request: &Value, task: &str, machine: &str) -> Result<Value> {
    let config = Config::load().map_err(|e| anyhow!(e.message))?;
    // The request names the receiving session itself, or (rework, answer) only the task.
    let field = if request.get("session").is_some() { "session" } else { "task" };
    let unreachable = |message: String| -> anyhow::Error { Refusal::field("host_unreachable", field, "invalid", message).into() };
    let alias = crate::swarm::alias_for(&config, machine).ok_or_else(|| {
        unreachable("this machine has no verified route to the session's machine; connect it in the swarm".into())
    })?;
    let hello = crate::swarm::call(&config, &alias, "hello", None)
        .map_err(|error| unreachable(format!("the session's machine cannot be reached: {error:#}")))?;
    if hello["node_id"] != machine {
        let message = format!("the machine behind {alias} is no longer the session's machine; nothing was attempted on it");
        return Err(Refusal::field("host_changed", field, "invalid", message).into());
    }
    if !hello["features"].as_array().is_some_and(|features| features.iter().any(|f| f == super::sync::FEATURE)) {
        return Err(Refusal::field("host_outdated", field, "invalid", "update HGS on the session's machine").into());
    }
    // Everything up to the delivery call leaves the host untouched.
    let not_attempted = |what: &str, error: anyhow::Error| -> anyhow::Error {
        match error.downcast::<Refusal>() {
            Ok(refusal) => refusal.into(),
            Err(error) => {
                let message = format!("{what} {alias} failed ({error:#}); nothing was attempted on the session's machine");
                Refusal::new("host_sync_failed", message).into()
            }
        }
    };
    let mut exchange = |request: &Value| crate::swarm::call(&config, &alias, "overmind-exchange", Some(request));
    super::sync::exchange(store, machine, &alias, &mut exchange)
        .map_err(|error| not_attempted("bringing the task up to date on", error))?;
    let loaded = store.read(|tx| commands::load(tx, task))?;
    push_blobs(&config, &alias, store, &loaded).map_err(|error| not_attempted("giving the task's images to", error))?;
    // The host may have acted before the call failed, so the outcome is unknown.
    let answer = crate::swarm::call(&config, &alias, "overmind-deliver", Some(&json!({"command": command, "request": request})))
        .map_err(|error| -> anyhow::Error {
            let text = format!("{error:#}");
            // An old host rejected the call before running anything.
            if outdated(&text) {
                return Refusal::field("host_outdated", field, "invalid", "update HGS on the session's machine").into();
            }
            let message = format!("the session's machine {alias} did not report the delivery ({text}); check the task before sending it again");
            Refusal::new("delivery_unknown", message).into()
        })?;
    // A failure here only delays the board: the worker exchanges again soon.
    let pulled = super::sync::exchange(store, machine, &alias, &mut exchange).is_ok();
    let mut result = host_answer(answer, field, &alias)?;
    if pulled && result.get("task").is_some() {
        // The host's view carries the host's reference; this machine's is the one the caller can use.
        let local = store.read(|tx| Ok(commands::view(&commands::load(tx, task)?, &commands::reference(tx, task)?, true)));
        if let Ok(view) = local {
            result["task"] = view;
        }
    }
    Ok(result)
}

/// Whether a failed call's text is a host's answer to a request or delivery
/// that its version does not know.
fn outdated(text: &str) -> bool {
    text.contains(super::UNKNOWN)
}

/// The host's result, or its refusal as the same refusal here. Only an
/// explicit `ok: true` is a result. `field` is what the caller calls the
/// receiving session; the host's refusals about it use that name. An answer
/// that is neither leaves the outcome unknown: the host may have acted.
fn host_answer(answer: Value, field: &str, alias: &str) -> Result<Value> {
    if answer["ok"] == true {
        return Ok(answer);
    }
    let refusal = (answer["ok"] == false)
        .then(|| serde_json::from_value::<Refusal>(answer["error"].clone()).ok())
        .flatten();
    let Some(mut refusal) = refusal else {
        let message = format!(
            "the session's machine {alias} sent an answer that is neither a result nor a refusal; check the task before sending it again"
        );
        return Err(Refusal::new("delivery_unknown", message).into());
    };
    if refusal.field == "session" {
        refusal.field = field.into();
    }
    Err(refusal.into())
}

/// Runs a forwarded sending command as a person on this machine. The session
/// must be here: the command is never forwarded again. A refusal is the
/// answer, not a failed call, so the caller can show its fields.
pub fn serve_deliver(store: &mut Store, request: &Value) -> Result<Value> {
    let machine = store.machine().to_owned();
    let inner = request["request"].clone();
    let mut ctx = Ctx { store, actor: Actor::human(&machine), cwd: None };
    let result = match request["command"].as_str().unwrap_or_default() {
        "send" => deliver::send_on(&mut ctx, inner, true),
        "review-send" => deliver::review_send_on(&mut ctx, inner, true),
        "rework-send" => deliver::rework_send_on(&mut ctx, inner, true),
        "answer" => deliver::answer_on(&mut ctx, inner, true),
        command => bail!("{} delivery {command}; update HGS on it", super::UNKNOWN),
    };
    result.or_else(|error| match error.downcast::<Refusal>() {
        Ok(refusal) => Ok(json!({"ok": false, "error": refusal})),
        Err(error) => Err(error),
    })
}

#[derive(serde::Deserialize)]
#[serde(deny_unknown_fields)]
struct AttachmentRequest {
    task: String,
    sha256: String,
}

pub fn attachment(ctx: &mut Ctx, request: Value) -> Result<Value> {
    let req: AttachmentRequest = commands::parse(&request)?;
    let actor = ctx.actor.clone();
    let task = ctx.store.read(|tx| commands::load(tx, &commands::resolve(tx, &req.task)?))?;
    if actor.kind != ActorKind::Human {
        let me = actor.session_ref();
        if me.is_none() || (me != task.assignee && me != task.reviewer) {
            return commands::refuse("not_assigned", "this task is not assigned to your session");
        }
    }
    let attachment = task
        .attachments
        .iter()
        .find(|a| a.sha256 == req.sha256)
        .ok_or_else(|| Refusal::field("invalid_field", "sha256", "invalid", "this task has no such image"))?;
    let only = Task { attachments: vec![attachment.clone()], ..task.clone() };
    ensure_blobs(ctx.store, &only)?;
    Ok(json!({"ok": true, "path": blobs::path(ctx.store.dir(), &attachment.sha256)?, "name": attachment.name,
              "mime": attachment.mime, "bytes": attachment.bytes}))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn push(count: usize, size: usize) -> Value {
        let items: Vec<Value> = (0..count)
            .map(|n| {
                let mut bytes = vec![0u8; size];
                bytes[0] = n as u8;
                json!({"sha256": format!("{:x}", Sha256::digest(&bytes)), "data_base64": base64::engine::general_purpose::STANDARD.encode(&bytes)})
            })
            .collect();
        json!({"put": items})
    }

    fn store() -> (tempfile::TempDir, Store) {
        let dir = tempfile::tempdir().unwrap();
        let store = Store::open(dir.path(), "11111111-1111-4111-8111-111111111111").unwrap();
        (dir, store)
    }

    #[test]
    fn pushed_blobs_are_bounded_before_anything_is_written() {
        let (_dir, store) = store();
        assert!(serve_blobs(&store, &push(9, 10)).is_err());
        assert!(serve_blobs(&store, &push(1, MAX_ATTACHMENT + 1)).is_err());
        assert!(serve_blobs(&store, &push(3, MAX_ATTACHMENT)).is_err());
        let mut request = push(2, 10);
        request["want"] = json!(vec!["a"; MAX_WANT + 1]);
        assert!(serve_blobs(&store, &request).is_err());
        let first = request["put"][0]["sha256"].as_str().unwrap().to_owned();
        assert!(!blobs::path(store.dir(), &first).unwrap().exists());
        assert!(serve_blobs(&store, &push(2, 10)).is_ok());
        assert!(blobs::path(store.dir(), &first).unwrap().exists());
    }

    fn image(fill: u8) -> (Vec<u8>, Attachment) {
        let bytes = vec![fill; 100];
        let attachment = Attachment {
            name: format!("{fill}.png"), mime: "image/png".into(), bytes: 100, sha256: format!("{:x}", Sha256::digest(&bytes)),
        };
        (bytes, attachment)
    }

    #[test]
    fn a_peer_without_one_image_is_still_asked_for_the_others() {
        let (_dir, store) = store();
        let (first, one) = image(1);
        let (second, two) = image(2);
        let aliases: Vec<String> = ["down", "half", "full"].iter().map(|a| a.to_string()).collect();
        let mut asked = Vec::new();
        // `down` fails its call; `half` holds only the second image; `full` holds both.
        let result = fetch_all(&store, &[&one, &two], &aliases, &mut |alias, request| {
            asked.push(alias.to_owned());
            let wanted = if request["sha256"] == one.sha256.as_str() { &first } else { &second };
            match alias {
                "down" => anyhow::bail!("connection to down timed out"),
                "half" if wanted == &first => Ok(json!({"missing": true})),
                _ => Ok(json!({"size": 100, "data_base64": base64::engine::general_purpose::STANDARD.encode(wanted)})),
            }
        });
        result.unwrap();
        assert_eq!(asked, ["down", "half", "full", "half"]);
        assert!(present(&store, &one) && present(&store, &two));
    }

    #[test]
    fn an_image_nobody_holds_names_the_last_error() {
        let (_dir, store) = store();
        let (_, one) = image(1);
        let aliases = vec!["half".to_string()];
        let error = fetch_all(&store, &[&one], &aliases, &mut |_, _| Ok(json!({"missing": true}))).unwrap_err();
        let refusal = error.downcast::<Refusal>().unwrap();
        assert_eq!(refusal.code, "attachment_unavailable");
        assert!(refusal.message.contains("half: the peer does not hold the image"), "{}", refusal.message);
    }

    #[test]
    fn a_missing_blob_is_an_answer() {
        let (_dir, store) = store();
        let (bytes, one) = image(1);
        assert_eq!(serve_blob(&store, &json!({"sha256": one.sha256})).unwrap(), json!({"missing": true}));
        blobs::put(store.dir(), &bytes).unwrap();
        assert_eq!(serve_blob(&store, &json!({"sha256": one.sha256})).unwrap()["size"], 100);
    }

    #[test]
    fn a_blob_is_served_by_ranges() {
        let (_dir, store) = store();
        let bytes: Vec<u8> = (0..=255).cycle().take(RANGE + 100).collect();
        let sha = blobs::put(store.dir(), &bytes).unwrap();
        let range = |offset: usize, length: Option<usize>| {
            let mut request = json!({"sha256": sha, "offset": offset});
            if let Some(length) = length {
                request["length"] = json!(length);
            }
            let answer = serve_blob(&store, &request)?;
            assert_eq!(answer["size"], bytes.len());
            Ok::<_, anyhow::Error>(base64::engine::general_purpose::STANDARD.decode(answer["data_base64"].as_str().unwrap()).unwrap())
        };
        assert_eq!(range(10, Some(20)).unwrap(), &bytes[10..30]);
        assert_eq!(range(0, None).unwrap(), &bytes[..RANGE], "at most one range");
        assert_eq!(range(RANGE, Some(RANGE * 4)).unwrap(), &bytes[RANGE..]);
        assert!(range(bytes.len(), Some(10)).unwrap().is_empty());
        assert!(range(bytes.len() + 1, Some(10)).is_err());
    }

    #[test]
    fn oversized_base64_is_refused_before_it_is_decoded() {
        let (_dir, store) = store();
        // Not even base64, but too long to hold an allowed image.
        let pushed = "!".repeat(MAX_ATTACHMENT.div_ceil(3) * 4 + 4);
        let error = serve_blobs(&store, &json!({"put": [{"sha256": "a", "data_base64": pushed}]})).unwrap_err();
        assert!(format!("{error:#}").contains("per-image limit"), "{error:#}");
        let (_, one) = image(1);
        let served = "!".repeat(RANGE.div_ceil(3) * 4 + 4);
        let error = fetch(&store, &one, &mut |_| Ok(json!({"size": 100, "data_base64": served}))).unwrap_err();
        assert!(format!("{error:#}").contains("more than the requested range"), "{error:#}");
    }

    #[test]
    fn a_request_the_host_does_not_know_reads_as_an_outdated_host() {
        let dir = tempfile::tempdir().unwrap();
        let config = Config {
            home: dir.path().to_str().unwrap().into(),
            dir: dir.path().join("config"),
            state: dir.path().join("state"),
            selves: vec![],
            peers: vec![],
            base_peers: vec![],
            peers_from_environment: false,
            machines: Default::default(),
            tab: false,
            tab_colors: String::new(),
        };
        let unknown = super::super::serve(&config, "overmind-from-the-future", json!({})).unwrap_err();
        // The caller sees the host's error inside the swarm call's own.
        assert!(outdated(&format!("swarm call failed: hgs: {unknown:#}")), "{unknown:#}");
        let (_dir, mut store) = store();
        let delivery = serve_deliver(&mut store, &json!({"command": "edit", "request": {}})).unwrap_err();
        assert!(outdated(&format!("{delivery:#}")), "{delivery:#}");
        assert!(!outdated("the session's machine is unreachable: connection refused"));
        assert!(!dir.path().join("state").exists(), "an unknown request opens no store");
    }

    #[test]
    fn a_host_refusal_becomes_the_same_refusal() {
        let sent = json!({"ok": true, "delivery": "sent"});
        assert_eq!(host_answer(sent.clone(), "session", "b").unwrap(), sent);
        let refused = json!({"ok": false, "error": {"code": "agent_busy", "message": "busy", "field": "confirm", "state": "missing"}});
        let refusal = host_answer(refused, "session", "b").unwrap_err().downcast::<Refusal>().unwrap();
        assert_eq!(refusal, Refusal::field("agent_busy", "confirm", "missing", "busy"));
        // A request without a session names the receiving session by its task.
        let about_session = json!({"ok": false, "error": {"code": "unknown_session", "message": "m", "field": "session", "state": "invalid"}});
        assert_eq!(host_answer(about_session.clone(), "task", "b").unwrap_err().downcast::<Refusal>().unwrap().field, "task");
        assert_eq!(host_answer(about_session, "session", "b").unwrap_err().downcast::<Refusal>().unwrap().field, "session");
        // Only an explicit success is one. Anything else that is no refusal leaves
        // the outcome unknown: the host may have acted.
        for answer in [json!({"ok": false, "error": "garbage"}), json!({"delivery": "sent"}), json!({"ok": "yes"}), json!(null)] {
            let refusal = host_answer(answer.clone(), "session", "host-b").unwrap_err().downcast::<Refusal>().unwrap();
            assert_eq!(refusal.code, "delivery_unknown", "{answer}");
            assert!(refusal.message.contains("host-b") && refusal.message.contains("check the task"), "{}", refusal.message);
        }
    }

    #[test]
    fn a_host_refuses_sessions_on_other_machines() {
        use crate::overmind::workflow::Action;
        const MACHINE: &str = "11111111-1111-4111-8111-111111111111";
        const OTHER: &str = "22222222-2222-4222-8222-222222222222";
        let (_dir, mut store) = store();
        let mut ctx = Ctx { store: &mut store, actor: Actor::human(MACHINE), cwd: None };
        let created = commands::create(&mut ctx, json!({"project": "p", "prompt": "Do it", "stage": "Queue"})).unwrap();
        let uuid = created["task"]["uuid"].as_str().unwrap().to_owned();
        let elsewhere = super::super::model::SessionRef { machine: OTHER.into(), name: "w1".into() };
        store
            .write(|tx| {
                let task = commands::load(tx, &uuid)?;
                let fields = std::collections::BTreeMap::new();
                commands::transition(tx, &Actor::human(MACHINE), &task, Action::Claim, "Design", fields, Some(elsewhere.clone()), vec![], "")
            })
            .unwrap();
        let session = json!({"machine": OTHER, "name": "w2"});
        let before = store.cursor().unwrap();
        for (command, request) in [
            ("send", json!({"task": uuid, "session": session})),
            ("review-send", json!({"task": uuid, "session": session})),
            ("rework-send", json!({"task": uuid})),
            ("answer", json!({"task": uuid, "answer": "yes"})),
        ] {
            let answer = serve_deliver(&mut store, &json!({"command": command, "request": request})).unwrap();
            assert_eq!((answer["ok"].clone(), answer["error"]["code"].clone()), (json!(false), json!("remote_session")), "{command}");
        }
        assert!(serve_deliver(&mut store, &json!({"command": "edit", "request": {}})).is_err());
        assert_eq!(store.cursor().unwrap(), before, "nothing was written");
    }

    #[test]
    fn fetched_chunks_are_checked_against_the_announced_size() {
        let mut bytes = Vec::new();
        assert!(accept_chunk(&mut bytes, &[1; 10], Some(11), 10).is_err());
        assert!(accept_chunk(&mut bytes, &[], Some(10), 10).is_err());
        assert!(accept_chunk(&mut bytes, &[1; 11], Some(10), 10).is_err());
        assert!(accept_chunk(&mut bytes, &vec![1; RANGE + 1], Some(RANGE as u64 + 1), RANGE as u64 + 1).is_err());
        assert!(accept_chunk(&mut bytes, &[1; 6], Some(10), 10).is_ok());
        assert!(accept_chunk(&mut bytes, &[1; 5], Some(10), 10).is_err());
        assert!(accept_chunk(&mut bytes, &[1; 4], Some(10), 10).is_ok());
        assert_eq!(bytes.len(), 10);
    }

    #[test]
    fn decoding_accepts_exactly_the_limit_and_refuses_one_byte_more() {
        let encode = |len: usize| base64::engine::general_purpose::STANDARD.encode(vec![7u8; len]);
        assert_eq!(decode_at_most(&encode(RANGE), RANGE, "too long").unwrap().len(), RANGE);
        assert!(decode_at_most(&encode(RANGE + 1), RANGE, "too long").is_err());
    }

    #[test]
    fn a_full_range_chunk_is_fetched() {
        let (_dir, store) = store();
        let bytes = vec![9u8; RANGE];
        let attachment = Attachment {
            name: "full.png".into(), mime: "image/png".into(), bytes: RANGE as u64, sha256: format!("{:x}", Sha256::digest(&bytes)),
        };
        let encoded = base64::engine::general_purpose::STANDARD.encode(&bytes);
        fetch(&store, &attachment, &mut |_| Ok(json!({"size": RANGE, "data_base64": encoded}))).unwrap();
        assert!(present(&store, &attachment));
    }
}
