//! The phone's task exchange through the relay
//! (docs/superpowers/specs/2026-10-18-task-replication-loro-design.md, "Phone"):
//! the wire form of a request and an answer, and the computer's answer, which
//! hgs serves.
use crate::store::{Damaged, ProjectStore};
use crate::sync::{Document, Inventory, Update, Version};
use crate::{ids, Error, Result};
use base64::{engine::general_purpose::STANDARD, Engine};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

/// The update bytes one answer carries; base64 and JSON keep it under the relay's 1 MiB.
pub const PAGE: usize = 512 * 1024;
/// The most updates one request carries; a phone sends the rest in its next.
pub const MAX_UPDATES: usize = 256;

/// One document's updates on the wire: `task` is its UUID, or none for the
/// project document; `bytes` are base64.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct WireUpdate {
    pub task: Option<String>,
    pub bytes: String,
}

impl WireUpdate {
    fn encode(update: &Update) -> Self {
        Self {
            task: name(&update.document),
            bytes: STANDARD.encode(&update.bytes),
        }
    }

    fn decode(self, invalid: &str) -> Result<Update> {
        Ok(Update {
            document: document(self.task),
            bytes: STANDARD
                .decode(self.bytes)
                .map_err(|_| Error::new(invalid))?,
        })
    }
}

/// The tasks whose UUIDs lie after `after` and up to `through`, in text
/// order, so a phone whose inventory is too large for one request sends it in
/// parts. The project document belongs to the range without `after`.
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Range {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub after: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub through: Option<String>,
}

impl Range {
    pub fn check(&self) -> Result<()> {
        for bound in self.after.iter().chain(&self.through) {
            if !ids::is_uuid(bound) {
                return Err(Error::new(format!(
                    "the range bound {bound:?} is not a task UUID"
                )));
            }
        }
        if let (Some(after), Some(through)) = (&self.after, &self.through) {
            if after >= through {
                return Err(Error::new("an empty range"));
            }
        }
        Ok(())
    }

    pub fn contains(&self, uuid: &str) -> bool {
        self.after.as_deref().is_none_or(|after| uuid > after)
            && self
                .through
                .as_deref()
                .is_none_or(|through| uuid <= through)
    }

    /// `inventory` narrowed to this range.
    pub fn narrow(&self, mut inventory: Inventory) -> Inventory {
        inventory.tasks.retain(|uuid, _| self.contains(uuid));
        if self.after.is_some() {
            inventory.project = Version::new();
        }
        inventory
    }
}

/// The next part of an update too large for one answer.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Resume {
    pub task: Option<String>,
    pub sha256: String,
    pub offset: usize,
}

/// What a phone sends: its inventory of the project within `range`, its
/// updates the computer lacks, and the next part it asks for.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Request {
    pub schema: u32,
    pub project: String,
    pub inventory: Inventory,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub range: Option<Range>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub updates: Vec<WireUpdate>,
    #[serde(default, rename = "continue", skip_serializing_if = "Option::is_none")]
    pub resume: Option<Resume>,
}

impl Request {
    /// The rules a request meets before any project is opened.
    pub fn check(&self) -> Result<()> {
        if self.schema != 1 {
            return Err(Error::new(format!(
                "unknown exchange schema {}",
                self.schema
            )));
        }
        let range = self.range.clone().unwrap_or_default();
        range.check()?;
        if let Some(outside) = self
            .inventory
            .tasks
            .keys()
            .find(|uuid| !range.contains(uuid))
        {
            return Err(Error::new(format!(
                "the inventory's task {outside:?} lies outside its range"
            )));
        }
        if self.updates.len() > MAX_UPDATES {
            return Err(Error::new(format!(
                "an exchange carries at most {MAX_UPDATES} updates"
            )));
        }
        Ok(())
    }
}

/// One part of an update larger than a page.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Part {
    pub task: Option<String>,
    pub size: usize,
    pub sha256: String,
    pub offset: usize,
    pub bytes: String,
}

/// What a computer answers: what the phone's updates changed and what did not
/// apply, a page of what the phone lacks (or one part of a larger update),
/// whether more remains, this computer's live digest, and, once nothing
/// remains for the phone, what this computer holds of its view.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Answer {
    pub schema: u32,
    pub imported: usize,
    pub refused: Vec<Damaged>,
    pub updates: Vec<WireUpdate>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub part: Option<Part>,
    pub more: bool,
    pub digest: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub inventory: Option<Inventory>,
}

fn document(task: Option<String>) -> Document {
    task.map_or(Document::Project, Document::Task)
}

fn name(document: &Document) -> Option<String> {
    match document {
        Document::Project => None,
        Document::Task(uuid) => Some(uuid.clone()),
    }
}

fn sha256(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}

/// `ours` narrowed to one document.
fn only(ours: &Inventory, document: &Document) -> Inventory {
    let mut one = Inventory::default();
    match document {
        Document::Project => one.project = ours.project.clone(),
        Document::Task(uuid) => {
            if let Some(version) = ours.tasks.get(uuid) {
                one.tasks.insert(uuid.clone(), version.clone());
            }
        }
    }
    one
}

/// The part of `update` from `offset`, at most one page.
fn part(update: &Update, offset: usize) -> Part {
    let end = (offset + PAGE).min(update.bytes.len());
    Part {
        task: name(&update.document),
        size: update.bytes.len(),
        sha256: sha256(&update.bytes),
        offset,
        bytes: STANDARD.encode(&update.bytes[offset..end]),
    }
}

/// The computer's answer to one request: it imports the phone's updates and
/// answers a page of what the phone lacks of the documents it holds and of the
/// live tasks, within the request's range; an update larger than a page goes
/// in parts. An answer with a part always has `more`: the phone asks again
/// once it holds the whole update. An answer with nothing for the phone names
/// what this computer holds of the phone's view, so the phone can send what
/// it lacks. Merges are idempotent, so a repeated request is safe.
pub fn answer(store: &ProjectStore, request: Request) -> Result<Answer> {
    request.check()?;
    if request.project != store.project_id() {
        return Err(Error::new(format!(
            "the request is for project {:?}",
            request.project
        )));
    }
    let updates = request
        .updates
        .into_iter()
        .map(|update| update.decode("invalid base64 in an update"))
        .collect::<Result<Vec<_>>>()?;
    let imported = store.import_updates(updates)?;
    let range = request.range.unwrap_or_default();
    let theirs = request.inventory;
    let ours = range.narrow(store.phone_inventory(&theirs)?);
    let mut answered = Answer {
        schema: 1,
        imported: imported.changed,
        refused: imported.refused,
        updates: Vec::new(),
        part: None,
        more: false,
        digest: String::new(),
        inventory: None,
    };
    match request.resume {
        Some(resume) => {
            // The whole update again: its parts continue while it is unchanged.
            let wanted = document(resume.task);
            let (whole, _) = store.updates_for(&only(&ours, &wanted), &theirs, usize::MAX)?;
            if let Some(update) = whole.into_iter().find(|u| u.document == wanted) {
                let same =
                    sha256(&update.bytes) == resume.sha256 && resume.offset < update.bytes.len();
                answered.part = Some(part(&update, if same { resume.offset } else { 0 }));
                answered.more = true;
            }
        }
        None => {
            let (page, more) = store.updates_for(&ours, &theirs, PAGE)?;
            match page.as_slice() {
                [update] if update.bytes.len() > PAGE => {
                    answered.part = Some(part(update, 0));
                    answered.more = true;
                }
                _ => {
                    answered.updates = page.iter().map(WireUpdate::encode).collect();
                    answered.more = more;
                }
            }
        }
    }
    if answered.part.is_none() && answered.updates.is_empty() {
        answered.inventory = Some(ours);
    }
    answered.digest = store.live_inventory()?.0.digest();
    Ok(answered)
}
