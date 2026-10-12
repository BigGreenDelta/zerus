//! Reaching other machines for blobs and deliveries, over the swarm channel.
use super::blobs;
use super::commands::{self, Ctx, MAX_ATTACHMENT, MAX_ATTACHMENTS, MAX_ATTACHMENTS_TOTAL};
use super::fold::{Attachment, Task};
use super::model::{ActorKind, Refusal};
use super::store::Store;
use crate::config::Config;
use anyhow::{anyhow, ensure, Result};
use base64::Engine;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};

pub const RANGE: usize = 512 * 1024;
const MAX_WANT: usize = 64;

fn unavailable(attachment: &Attachment, last: &str) -> anyhow::Error {
    let detail = if last.is_empty() { String::new() } else { format!(" (last error: {last})") };
    Refusal::field("attachment_unavailable", "attachments", "missing", format!("no reachable machine holds the image {}{detail}", attachment.name)).into()
}

/// Serves one range of a stored blob.
pub fn serve_blob(store: &Store, request: &Value) -> Result<Value> {
    let sha256 = request["sha256"].as_str().unwrap_or_default();
    let offset = request["offset"].as_u64().unwrap_or(0) as usize;
    let length = (request["length"].as_u64().unwrap_or(RANGE as u64) as usize).min(RANGE);
    let bytes = std::fs::read(blobs::path(store.dir(), sha256)?).map_err(|_| anyhow!("missing"))?;
    let end = offset.saturating_add(length).min(bytes.len());
    ensure!(offset <= end, "invalid range");
    Ok(json!({"size": bytes.len(), "data_base64": base64::engine::general_purpose::STANDARD.encode(&bytes[offset..end])}))
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
        let bytes = base64::engine::general_purpose::STANDARD.decode(item["data_base64"].as_str().unwrap_or_default())?;
        ensure!(bytes.len() <= MAX_ATTACHMENT, "a pushed image exceeds the per-image limit");
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

fn fetch(config: &Config, alias: &str, store: &Store, attachment: &Attachment) -> Result<()> {
    ensure!(attachment.bytes <= MAX_ATTACHMENT as u64, "the image is larger than the per-image limit");
    let mut bytes = Vec::new();
    while (bytes.len() as u64) < attachment.bytes {
        let part = crate::swarm::call(
            config,
            alias,
            "overmind-blob",
            Some(&json!({"sha256": attachment.sha256, "offset": bytes.len(), "length": RANGE})),
        )?;
        let chunk = base64::engine::general_purpose::STANDARD.decode(part["data_base64"].as_str().unwrap_or_default())?;
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
    // A peer that failed once is not asked again for the remaining images.
    let mut failed: Vec<String> = Vec::new();
    for attachment in missing {
        let mut last = String::new();
        let mut found = false;
        for alias in &aliases {
            if failed.contains(alias) {
                continue;
            }
            match fetch(&config, alias, store, attachment) {
                Ok(()) => {
                    found = true;
                    break;
                }
                Err(error) => {
                    last = format!("{error:#}");
                    failed.push(alias.clone());
                }
            }
        }
        if !found {
            return Err(unavailable(attachment, &last));
        }
    }
    Ok(())
}

/// Gives the machine behind `alias` the task's images it lacks.
// Delivery to another machine (Task 7) is its caller.
#[allow(dead_code)]
pub fn push_blobs(config: &Config, alias: &str, store: &Store, task: &Task) -> Result<()> {
    let want: Vec<&str> = task.attachments.iter().map(|a| a.sha256.as_str()).collect();
    if want.is_empty() {
        return Ok(());
    }
    let answer = crate::swarm::call(config, alias, "overmind-blobs", Some(&json!({"want": want})))?;
    let mut put = Vec::new();
    for sha in answer["missing"].as_array().into_iter().flatten().filter_map(Value::as_str) {
        let attachment = task.attachments.iter().find(|a| a.sha256 == sha).ok_or_else(|| anyhow!("unexpected image request"))?;
        let bytes = std::fs::read(blobs::path(store.dir(), sha)?).map_err(|_| unavailable(attachment, ""))?;
        put.push(json!({"sha256": sha, "data_base64": base64::engine::general_purpose::STANDARD.encode(bytes)}));
    }
    if !put.is_empty() {
        crate::swarm::call(config, alias, "overmind-blobs", Some(&json!({"put": put})))?;
    }
    Ok(())
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
}
