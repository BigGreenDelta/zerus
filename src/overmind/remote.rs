//! Reaching other machines for blobs and deliveries, over the swarm channel.
use super::blobs;
use super::commands::{self, Ctx};
use super::fold::{Attachment, Task};
use super::model::{ActorKind, Refusal};
use super::store::Store;
use crate::config::Config;
use anyhow::{anyhow, ensure, Result};
use base64::Engine;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};

pub const RANGE: usize = 512 * 1024;

fn unavailable(attachment: &Attachment) -> anyhow::Error {
    Refusal::field("attachment_unavailable", "attachments", "missing", format!("no reachable machine holds the image {}", attachment.name)).into()
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
    for item in request["put"].as_array().into_iter().flatten() {
        let bytes = base64::engine::general_purpose::STANDARD.decode(item["data_base64"].as_str().unwrap_or_default())?;
        ensure!(format!("{:x}", Sha256::digest(&bytes)) == item["sha256"].as_str().unwrap_or_default(), "a pushed image does not match its digest");
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

fn fetch(config: &Config, alias: &str, store: &Store, attachment: &Attachment) -> Result<()> {
    let mut bytes = Vec::new();
    while (bytes.len() as u64) < attachment.bytes {
        let part = crate::swarm::call(
            config,
            alias,
            "overmind-blob",
            Some(&json!({"sha256": attachment.sha256, "offset": bytes.len(), "length": RANGE})),
        )?;
        let chunk = base64::engine::general_purpose::STANDARD.decode(part["data_base64"].as_str().unwrap_or_default())?;
        ensure!(!chunk.is_empty(), "the peer returned an empty range");
        bytes.extend(chunk);
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
    for attachment in missing {
        let found = aliases.iter().any(|alias| fetch(&config, alias, store, attachment).is_ok());
        if !found {
            return Err(unavailable(attachment));
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
        let bytes = std::fs::read(blobs::path(store.dir(), sha)?).map_err(|_| unavailable(attachment))?;
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
