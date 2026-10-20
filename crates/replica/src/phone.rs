//! The phone's task exchange through the relay
//! (docs/superpowers/specs/2026-10-18-task-replication-loro-design.md, "Phone"):
//! the wire form of a request and an answer, the computer's answer, which hgs
//! serves, and the phone's side, which walks its inventory in ranges that fit
//! a request, puts large documents together from parts and sends what the
//! computer lacks.
use crate::store::{Damaged, ProjectStore};
use crate::sync::{Document, Inventory, Update, Version};
use crate::{ids, Error, Result};
use base64::{engine::general_purpose::STANDARD, Engine};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};

/// The update bytes one answer carries; base64 and JSON keep it under the relay's 1 MiB.
pub const PAGE: usize = 512 * 1024;
/// The most updates one request carries; a phone sends the rest in its next.
pub const MAX_UPDATES: usize = 256;
/// The largest document a phone puts together from parts: twice the 4 MiB a
/// task document may reach.
const MAX_ASSEMBLY: usize = 2 * crate::MAX_DOCUMENT;

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

/// What the phone's own part of a request may take, so that the whole request
/// with its envelope stays within the relay's 1 MiB and 50,000 JSON values.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Budget {
    /// The inventory of one range, as JSON.
    pub inventory_bytes: usize,
    /// The inventory's JSON values, as the relay counts them.
    pub inventory_values: usize,
    /// The phone's updates in one request, before base64.
    pub push_bytes: usize,
    /// The requests of one exchange before it gives up.
    pub requests: usize,
}

impl Default for Budget {
    fn default() -> Self {
        Self {
            inventory_bytes: 256 * 1024,
            inventory_values: 20_000,
            push_bytes: 512 * 1024,
            requests: 512,
        }
    }
}

/// What one answer did on the phone.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Progress {
    /// Documents the computer's updates changed here.
    pub changed: usize,
    /// The computer's updates this replica refused.
    pub refused_here: Vec<Damaged>,
    /// This replica's updates the computer refused; they are not sent again.
    pub refused_there: Vec<Damaged>,
    /// This replica's documents too large for a request ("project" or a task UUID).
    pub unsent: Vec<String>,
    /// The exchange has nothing left to do.
    pub finished: bool,
    /// The computer's live digest after this answer.
    pub digest: String,
}

/// A document that arrives in parts.
struct Assembly {
    task: Option<String>,
    size: usize,
    sha256: String,
    bytes: Vec<u8>,
}

/// The phone's exchange of one project's tasks with one computer. Each
/// request covers a range of task UUIDs whose inventory fits the budget; a
/// range is done once the computer has nothing more for the phone and its
/// inventory shows nothing the phone still has to send. Each call opens
/// nothing itself: the caller passes the store, under its lock.
pub struct Exchange {
    budget: Budget,
    /// The ranges still to settle; the first is the current one.
    ranges: Vec<Range>,
    /// The computer's inventory of the current range, from its last answer.
    computer: Option<Inventory>,
    /// The updates the next request carries, found when the last answer came.
    queued: Option<Vec<WireUpdate>>,
    part: Option<Assembly>,
    /// Documents not sent again: the computer refused them, or they are too large.
    skip: BTreeSet<String>,
    unsent: BTreeSet<String>,
    refused_here: BTreeSet<String>,
    /// Tasks the phone asks for although it does not hold them, such as a
    /// Done task the person opens; they arrive whole and are then held.
    wanted: BTreeSet<String>,
    requests: usize,
}

impl Default for Exchange {
    fn default() -> Self {
        Self::new()
    }
}

/// How a document is named in refusals and in `unsent`.
fn key(document: &Document) -> String {
    match document {
        Document::Project => "project".to_owned(),
        Document::Task(uuid) => uuid.clone(),
    }
}

impl Exchange {
    pub fn new() -> Self {
        Self::with_budget(Budget::default())
    }

    pub fn with_budget(budget: Budget) -> Self {
        Self {
            budget,
            ranges: vec![Range::default()],
            computer: None,
            queued: None,
            part: None,
            skip: BTreeSet::new(),
            unsent: BTreeSet::new(),
            refused_here: BTreeSet::new(),
            wanted: BTreeSet::new(),
            requests: 0,
        }
    }

    /// Asks for these tasks too, whatever their stage: each comes whole and
    /// stays with the phone, so later exchanges keep it.
    pub fn fetch(&mut self, uuids: impl IntoIterator<Item = String>) {
        self.wanted
            .extend(uuids.into_iter().filter(|uuid| ids::is_uuid(uuid)));
    }

    pub fn finished(&self) -> bool {
        self.ranges.is_empty()
    }

    /// The next request, or none once every range is settled.
    pub fn next_request(&mut self, store: &ProjectStore) -> Result<Option<Request>> {
        if self.ranges.is_empty() {
            return Ok(None);
        }
        if self.requests >= self.budget.requests {
            self.ranges.clear();
            return Err(Error::failed(format!(
                "the exchange did not settle within {} requests",
                self.budget.requests
            )));
        }
        let (held, _) = store.inventory()?;
        let inventory = self.fit(held);
        let updates = match (self.queued.take(), self.computer.take()) {
            (Some(queued), computer) => {
                self.computer = computer;
                queued
            }
            (None, Some(theirs)) => {
                let updates = self.push(store, &inventory, &theirs)?;
                self.computer = Some(theirs);
                updates
            }
            (None, None) => Vec::new(),
        };
        self.requests += 1;
        let range = self.ranges[0].clone();
        // A task asked for and not held goes with an empty version: the
        // computer sends all of it.
        let mut asked = inventory;
        for uuid in self.wanted.iter().filter(|uuid| range.contains(uuid)) {
            asked.tasks.entry(uuid.clone()).or_default();
        }
        Ok(Some(Request {
            schema: 1,
            project: store.project_id().to_owned(),
            inventory: asked,
            range: (range != Range::default()).then_some(range),
            updates,
            resume: self.part.as_ref().map(|part| Resume {
                task: part.task.clone(),
                sha256: part.sha256.clone(),
                offset: part.bytes.len(),
            }),
        }))
    }

    /// Takes the computer's answer to the last request.
    pub fn accept(&mut self, store: &ProjectStore, answer: Answer) -> Result<Progress> {
        if self.ranges.is_empty() {
            return Err(Error::new("the exchange has ended"));
        }
        if answer.schema != 1 {
            return Err(Error::new(format!(
                "unknown exchange schema {}",
                answer.schema
            )));
        }
        let mut updates = answer
            .updates
            .into_iter()
            .map(|update| update.decode("invalid base64 in an answer"))
            .collect::<Result<Vec<_>>>()?;
        let with_part = answer.part.is_some();
        match answer.part {
            Some(part) => updates.extend(self.assemble(part)?),
            // The request asked for the next part and none came: the computer no
            // longer offers that document, so the phone stops waiting for it.
            None => self.part = None,
        }
        let brought = updates.len();
        // More promised and nothing brought would come back the same.
        if answer.more && brought == 0 && !with_part {
            self.ranges.clear();
            return Err(Error::failed(
                "the computer's answer brought nothing but said more remains",
            ));
        }
        let imported = store.import_updates(updates)?;
        self.skip
            .extend(answer.refused.iter().map(|damaged| damaged.uuid.clone()));
        // An answer that changes nothing and only brings what this replica
        // already refused would come back the same until the budget runs out.
        if brought > 0
            && imported.changed == 0
            && !imported.refused.is_empty()
            && imported
                .refused
                .iter()
                .all(|damaged| self.refused_here.contains(&damaged.uuid))
        {
            self.ranges.clear();
            return Err(Error::failed(format!(
                "the computer's updates do not apply here: {}",
                imported.refused[0].error
            )));
        }
        self.refused_here
            .extend(imported.refused.iter().map(|damaged| damaged.uuid.clone()));
        self.computer = answer.inventory.map(|theirs| self.ranges[0].narrow(theirs));
        self.queued = None;
        let mut settled = false;
        if self.part.is_none() && !answer.more {
            if let Some(theirs) = self.computer.take() {
                let (held, _) = store.inventory()?;
                let ours = self.ranges[0].narrow(held);
                let pending = self.push(store, &ours, &theirs)?;
                self.computer = Some(theirs);
                settled = pending.is_empty();
                self.queued = Some(pending);
            }
        }
        if settled {
            self.ranges.remove(0);
            self.computer = None;
            self.queued = None;
        }
        Ok(Progress {
            changed: imported.changed,
            refused_here: imported.refused,
            refused_there: answer.refused,
            unsent: self.unsent.iter().cloned().collect(),
            finished: self.ranges.is_empty(),
            digest: answer.digest,
        })
    }

    /// The phone's inventory of the current range. A range that has grown
    /// past the budget ends at the last task that fits, and the rest becomes
    /// the next range; at least one task always stays.
    fn fit(&mut self, held: Inventory) -> Inventory {
        let inventory = self.ranges[0].narrow(held);
        let project_only = Inventory {
            project: inventory.project.clone(),
            tasks: BTreeMap::new(),
        };
        let mut bytes = serde_json::to_vec(&project_only).map_or(0, |b| b.len());
        // As the relay counts: the inventory, its keys, the project and its peers.
        let mut values = 5 + 2 * inventory.project.len();
        let mut cut = None;
        for (count, (uuid, version)) in inventory.tasks.iter().enumerate() {
            bytes += uuid.len() + 4 + serde_json::to_vec(version).map_or(0, |b| b.len());
            values += 2 + 2 * version.len();
            if count > 0
                && (bytes > self.budget.inventory_bytes || values > self.budget.inventory_values)
            {
                cut = Some(uuid.clone());
                break;
            }
        }
        let Some(cut) = cut else {
            return inventory;
        };
        let mut tasks = inventory.tasks;
        tasks.split_off(&cut);
        let last = tasks.keys().next_back().expect("one task stays").clone();
        let through = self.ranges[0].through.replace(last.clone());
        self.ranges.insert(
            1,
            Range {
                after: Some(last),
                through,
            },
        );
        if let Some(theirs) = self.computer.take() {
            self.computer = Some(self.ranges[0].narrow(theirs));
        }
        self.queued = None;
        Inventory {
            project: inventory.project,
            tasks,
        }
    }

    /// The phone's updates the computer lacks of `ours`, within the budget;
    /// a document too large for any request is named and never sent.
    fn push(
        &mut self,
        store: &ProjectStore,
        ours: &Inventory,
        theirs: &Inventory,
    ) -> Result<Vec<WireUpdate>> {
        let mut ours = ours.clone();
        loop {
            for name in &self.skip {
                if name == "project" {
                    ours.project = Version::new();
                } else {
                    ours.tasks.remove(name);
                }
            }
            let (mut page, _) = store.updates_for(&ours, theirs, self.budget.push_bytes)?;
            if let [update] = page.as_slice() {
                if update.bytes.len() > self.budget.push_bytes {
                    let name = key(&update.document);
                    self.unsent.insert(name.clone());
                    self.skip.insert(name);
                    continue;
                }
            }
            page.truncate(MAX_UPDATES);
            return Ok(page.iter().map(WireUpdate::encode).collect());
        }
    }

    /// Adds one part; the whole document once its last part arrived and its
    /// SHA-256 matches. A part that does not continue what arrived drops the
    /// document, and the next request asks for it afresh.
    fn assemble(&mut self, part: Part) -> Result<Option<Update>> {
        if part.size > MAX_ASSEMBLY {
            return Err(Error::new("an update in parts is too large"));
        }
        let chunk = STANDARD
            .decode(&part.bytes)
            .map_err(|_| Error::new("invalid base64 in an answer"))?;
        let continues = self.part.as_ref().is_some_and(|whole| {
            whole.task == part.task
                && whole.sha256 == part.sha256
                && whole.size == part.size
                && whole.bytes.len() == part.offset
        });
        if part.offset == 0 {
            self.part = Some(Assembly {
                task: part.task,
                size: part.size,
                sha256: part.sha256,
                bytes: chunk,
            });
        } else if continues {
            self.part.as_mut().expect("continues").bytes.extend(chunk);
        } else {
            self.part = None;
            return Ok(None);
        }
        if self
            .part
            .as_ref()
            .is_some_and(|whole| whole.bytes.len() < whole.size)
        {
            return Ok(None);
        }
        let whole = self.part.take().expect("complete");
        if whole.bytes.len() != whole.size || sha256(&whole.bytes) != whole.sha256 {
            return Ok(None);
        }
        Ok(Some(Update {
            document: document(whole.task),
            bytes: whole.bytes,
        }))
    }
}
