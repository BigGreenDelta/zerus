//! An append-only, multi-value register per shared field. Causal parents, rather
//! than wall clocks, decide which edits supersede others. Deletions are retained.
use anyhow::{bail, ensure, Result};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use uuid::Uuid;

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq, PartialOrd, Ord)]
#[serde(tag = "kind", deny_unknown_fields, rename_all = "snake_case")]
pub enum Key {
    Project { id: String, field: String },
    Folder { id: String },
    Session { machine: String, session: String },
    Machine { id: String, field: String },
    Preference { field: String },
}

impl Key {
    pub fn token(&self) -> String {
        serde_json::to_string(self).expect("field key")
    }
    pub fn project(id: &str, field: &str) -> Self {
        Self::Project {
            id: id.into(),
            field: field.into(),
        }
    }
    pub fn machine(id: &str, field: &str) -> Self {
        Self::Machine {
            id: id.into(),
            field: field.into(),
        }
    }
    pub fn preference(field: &str) -> Self {
        Self::Preference {
            field: field.into(),
        }
    }
    /// The serde tag. Exchanges use it to omit kinds an older member rejects.
    pub fn kind(&self) -> &'static str {
        match self {
            Self::Project { .. } => "project",
            Self::Folder { .. } => "folder",
            Self::Session { .. } => "session",
            Self::Machine { .. } => "machine",
            Self::Preference { .. } => "preference",
        }
    }
    fn validate(&self, value: &Value) -> Result<()> {
        let label = |s: &str, max: usize| {
            !s.is_empty() && s.len() <= max && !s.chars().any(char::is_control)
        };
        let string = |v: &Value, max| v.as_str().is_some_and(|s| label(s, max));
        match self {
            Self::Project { id, field } => {
                ensure!(label(id, 128), "invalid project ID");
                ensure!(
                    match field.as_str() {
                        "alive" => value.is_boolean(),
                        "name" => string(value, 320),
                        "color" => value.as_str().is_some_and(|s| s.len() == 7
                            && s.starts_with('#')
                            && s[1..].chars().all(|c| c.is_ascii_hexdigit())),
                        _ => false,
                    },
                    "invalid shared project field"
                );
            }
            Self::Folder { id } => {
                ensure!(label(id, 128), "invalid folder ID");
                if !value.is_null() {
                    let o = value
                        .as_object()
                        .ok_or_else(|| anyhow::anyhow!("invalid folder"))?;
                    ensure!(o.len() == 4 && string(&value["project"], 128) && uuid(value["machine"].as_str().unwrap_or_default())
                        && value["path"].as_str().is_some_and(|p| p.starts_with('/') && p.len() <= 8192 && !p.chars().any(char::is_control))
                        && value["name"].as_str().is_some_and(|s| s.len() <= 320 && !s.chars().any(char::is_control)), "invalid shared folder fields");
                }
            }
            Self::Session { machine, session } => {
                ensure!(
                    uuid(machine)
                        && !session.is_empty()
                        && session.len() <= 2048
                        && !session.contains('\0'),
                    "invalid session identity"
                );
                ensure!(
                    value.is_null() || string(value, 128),
                    "invalid session project"
                );
            }
            Self::Machine { id, field } => {
                ensure!(uuid(id), "invalid machine identity");
                ensure!(
                    match field.as_str() {
                        "name" => string(value, 320),
                        "redirect" => value.as_str().is_some_and(|s| uuid(s) && s != id),
                        _ => false,
                    },
                    "invalid shared machine field"
                );
            }
            Self::Preference { field } => {
                ensure!(
                    match field.as_str() {
                        "mobile_delivery" =>
                            value.as_str().is_some_and(|s| MOBILE_DELIVERY.contains(&s)),
                        _ => false,
                    },
                    "invalid shared preference"
                );
            }
        }
        Ok(())
    }
}

pub fn uuid(value: &str) -> bool {
    Uuid::parse_str(value).is_ok()
}
pub fn new_id() -> String {
    Uuid::new_v4().to_string()
}
/// When the phone notifies: at once, or after the owner is away from every
/// Zerus desktop for the given number of minutes.
pub const MOBILE_DELIVERY: [&str; 6] = [
    "immediate",
    "away",
    "away_5",
    "away_10",
    "away_15",
    "away_30",
];
pub const DEFAULT_MOBILE_DELIVERY: &str = "away";
pub fn default_preferences() -> Value {
    json!({"mobile_delivery": DEFAULT_MOBILE_DELIVERY})
}
/// Key kinds every member parses. Members from before shared preferences reject
/// any other kind, so exchanges with them omit newer kinds.
pub const LEGACY_KINDS: [&str; 4] = ["project", "folder", "session", "machine"];
pub fn kinds() -> BTreeSet<String> {
    ["project", "folder", "session", "machine", "preference"]
        .into_iter()
        .map(str::to_owned)
        .collect()
}
/// Whether a member advertising `kinds` (or nothing, if older) accepts `key`.
pub fn accepted(kinds: Option<&BTreeSet<String>>, key: &Key) -> bool {
    kinds.map_or(LEGACY_KINDS.contains(&key.kind()), |kinds| {
        kinds.contains(key.kind())
    })
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct Operation {
    pub id: String,
    pub actor: String,
    pub clock: u64,
    pub key: Key,
    pub parents: Vec<String>,
    pub value: Value,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Catalog {
    pub schema: u32,
    pub swarm_id: String,
    pub operations: BTreeMap<String, Operation>,
}

impl Default for Catalog {
    fn default() -> Self {
        Self {
            schema: 1,
            swarm_id: new_id(),
            operations: BTreeMap::new(),
        }
    }
}

impl Catalog {
    pub fn validate(&self) -> Result<()> {
        ensure!(
            self.schema == 1 && uuid(&self.swarm_id),
            "unsupported swarm catalog"
        );
        ensure!(
            self.operations.len() <= 100_000,
            "swarm catalog is too large"
        );
        let identities = self.identities()?;
        for (id, op) in &self.operations {
            ensure!(
                id == &op.id && uuid(id) && uuid(&op.actor) && op.clock > 0 && op.clock < u64::MAX,
                "invalid operation identity"
            );
            op.key.validate(&op.value)?;
            ensure!(op.parents.len() <= 1024, "too many causal parents");
            for parent in &op.parents {
                let p = self
                    .operations
                    .get(parent)
                    .ok_or_else(|| anyhow::anyhow!("missing causal parent"))?;
                ensure!(
                    p.clock < op.clock
                        && Self::key_with_identities(&p.key, &identities)
                            == Self::key_with_identities(&op.key, &identities),
                    "invalid causal parent"
                );
            }
        }
        Ok(())
    }

    pub fn identities(&self) -> Result<BTreeMap<String, String>> {
        let hidden: BTreeSet<_> = self
            .operations
            .values()
            .flat_map(|o| o.parents.iter())
            .collect();
        let mut selected: BTreeMap<String, &Operation> = BTreeMap::new();
        for op in self
            .operations
            .values()
            .filter(|op| !hidden.contains(&op.id))
        {
            if let Key::Machine { id, field } = &op.key {
                if field == "redirect"
                    && selected.get(id).is_none_or(|old| {
                        (old.clock, &old.actor, &old.id) < (op.clock, &op.actor, &op.id)
                    })
                {
                    selected.insert(id.clone(), op);
                }
            }
        }
        let mut result = BTreeMap::new();
        for id in selected.keys() {
            let mut current = id.as_str();
            let mut seen = BTreeSet::new();
            while let Some(next) = selected.get(current) {
                ensure!(
                    seen.insert(current) && seen.len() <= 64,
                    "machine identity redirect cycle"
                );
                current = next
                    .value
                    .as_str()
                    .ok_or_else(|| anyhow::anyhow!("invalid machine redirect"))?;
            }
            result.insert(id.clone(), current.to_owned());
        }
        Ok(result)
    }
    fn key_with_identities(key: &Key, ids: &BTreeMap<String, String>) -> Key {
        match key {
            Key::Session { machine, session } => Key::Session {
                machine: ids.get(machine).unwrap_or(machine).clone(),
                session: session.clone(),
            },
            _ => key.clone(),
        }
    }

    fn raw_heads(&self, key: &Key) -> Vec<&Operation> {
        let candidates: Vec<_> = self.operations.values().filter(|o| &o.key == key).collect();
        let hidden: BTreeSet<_> = candidates.iter().flat_map(|o| o.parents.iter()).collect();
        let mut heads: Vec<_> = candidates
            .into_iter()
            .filter(|o| !hidden.contains(&o.id))
            .collect();
        heads.sort_by(|a, b| (a.clock, &a.actor, &a.id).cmp(&(b.clock, &b.actor, &b.id)));
        heads
    }

    fn resolve_machine_checked(&self, id: &str) -> Result<String> {
        let mut current = id.to_owned();
        let mut seen = BTreeSet::new();
        loop {
            ensure!(
                seen.insert(current.clone()) && seen.len() <= 64,
                "machine identity redirect cycle"
            );
            let heads = self.raw_heads(&Key::machine(&current, "redirect"));
            let Some(next) = heads.last().and_then(|o| o.value.as_str()) else {
                return Ok(current);
            };
            current = next.to_owned();
        }
    }

    pub fn machine(&self, id: &str) -> String {
        self.resolve_machine_checked(id)
            .unwrap_or_else(|_| id.to_owned())
    }
    pub fn canonical_key(&self, key: &Key) -> Key {
        match key {
            Key::Session { machine, session } => Key::Session {
                machine: self.machine(machine),
                session: session.clone(),
            },
            _ => key.clone(),
        }
    }
    pub fn heads(&self) -> BTreeMap<Key, Vec<&Operation>> {
        let hidden: BTreeSet<_> = self
            .operations
            .values()
            .flat_map(|o| o.parents.iter())
            .collect();
        let identities = self.identities().unwrap_or_default();
        let mut result: BTreeMap<Key, Vec<&Operation>> = BTreeMap::new();
        for op in self.operations.values().filter(|o| !hidden.contains(&o.id)) {
            result
                .entry(Self::key_with_identities(&op.key, &identities))
                .or_default()
                .push(op);
        }
        for heads in result.values_mut() {
            heads.sort_by(|a, b| (a.clock, &a.actor, &a.id).cmp(&(b.clock, &b.actor, &b.id)));
        }
        result
    }
    pub fn selected<'a>(key: &Key, heads: &'a [&Operation]) -> Option<&'a Operation> {
        // Concurrent deletion wins. Its operation stays forever, including when
        // older replicas bring back fields/folders that existed before removal.
        let deleted = heads.iter().find(|o| match key {
            Key::Project { field, .. } if field == "alive" => o.value == false,
            Key::Folder { .. } | Key::Session { .. } => o.value.is_null(),
            _ => false,
        });
        deleted.copied().or_else(|| heads.last().copied())
    }
    pub fn values(&self) -> BTreeMap<Key, Value> {
        self.heads()
            .into_iter()
            .filter_map(|(k, h)| Self::selected(&k, &h).map(|o| (k, o.value.clone())))
            .collect()
    }
    /// Shared preferences with defaults. Concurrent edits resolve to the newest
    /// head on every replica; the next choice on any device supersedes them all.
    /// An unvalidated (hand-edited) value reports the default.
    pub fn preferences(&self) -> Value {
        let key = Key::preference("mobile_delivery");
        let heads = self.raw_heads(&key);
        let value = Self::selected(&key, &heads)
            .and_then(|op| op.value.as_str())
            .filter(|s| MOBILE_DELIVERY.contains(s))
            .unwrap_or(DEFAULT_MOBILE_DELIVERY);
        json!({"mobile_delivery": value})
    }
    pub fn versions(&self) -> BTreeMap<String, Vec<String>> {
        self.heads()
            .into_iter()
            .map(|(k, h)| (k.token(), h.iter().map(|o| o.id.clone()).collect()))
            .collect()
    }
    pub fn write(
        &mut self,
        actor: &str,
        key: Key,
        value: Value,
        parents: Vec<String>,
    ) -> Result<()> {
        key.validate(&value)?;
        for id in &parents {
            ensure!(
                self.operations
                    .get(id)
                    .is_some_and(|o| self.canonical_key(&o.key) == self.canonical_key(&key)),
                "unknown field version; reload the catalog"
            );
        }
        let op = Operation {
            id: new_id(),
            actor: actor.into(),
            clock: self.operations.values().map(|o| o.clock).max().unwrap_or(0) + 1,
            key,
            value,
            parents,
        };
        self.operations.insert(op.id.clone(), op);
        Ok(())
    }
    pub fn set(&mut self, actor: &str, key: Key, value: Value) -> Result<()> {
        let heads = self.heads();
        let current = heads
            .get(&self.canonical_key(&key))
            .cloned()
            .unwrap_or_default();
        if !current.is_empty() && current.iter().all(|o| o.value == value) {
            return Ok(());
        }
        let parents = current.iter().map(|o| o.id.clone()).collect();
        self.write(actor, key, value, parents)
    }
    pub fn merge(&mut self, swarm_id: &str, operations: Vec<Operation>) -> Result<()> {
        ensure!(
            self.swarm_id == swarm_id,
            "different swarm; join explicitly first"
        );
        let mut merged = self.clone();
        for op in operations {
            if let Some(existing) = merged.operations.get(&op.id) {
                if existing != &op {
                    bail!("conflicting operation identity");
                }
            } else {
                merged.operations.insert(op.id.clone(), op);
            }
        }
        merged.validate()?;
        *self = merged;
        Ok(())
    }
    pub fn digest(&self) -> String {
        let mut hash = Sha256::new();
        hash.update(self.swarm_id.as_bytes());
        for op in self.operations.values() {
            hash.update(serde_json::to_vec(op).expect("operation"));
        }
        format!("{:x}", hash.finalize())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    fn merge(a: &mut Catalog, b: &Catalog) {
        a.merge(&b.swarm_id, b.operations.values().cloned().collect())
            .unwrap();
    }
    #[test]
    fn three_replicas_merge_offline_edits_and_resolve_conflict() {
        let (a, b, c) = (new_id(), new_id(), new_id());
        let name = Key::project("p", "name");
        let mut first = Catalog::default();
        first.set(&a, name.clone(), json!("Original")).unwrap();
        let mut second = first.clone();
        let mut third = first.clone();
        first.set(&a, name.clone(), json!("On A")).unwrap();
        second.set(&b, name.clone(), json!("On B")).unwrap();
        third
            .set(&c, Key::project("p", "color"), json!("#123456"))
            .unwrap();
        merge(&mut second, &first);
        merge(&mut third, &second);
        merge(&mut first, &third);
        assert_eq!(first.heads()[&name].len(), 2);
        assert_eq!(first.digest(), third.digest());
        third.set(&c, name.clone(), json!("Resolved")).unwrap();
        merge(&mut first, &third);
        merge(&mut second, &first);
        assert_eq!(second.heads()[&name].len(), 1);
        assert_eq!(second.values()[&name], "Resolved");
        assert_eq!(first.digest(), second.digest());
    }
    #[test]
    fn offline_edits_and_repeated_exchange_never_resurrect_deleted_projects() {
        let actor = new_id();
        let alive = Key::project("p", "alive");
        let mut a = Catalog::default();
        a.set(&actor, alive.clone(), json!(true)).unwrap();
        let mut b = a.clone();
        a.set(&actor, alive.clone(), json!(false)).unwrap();
        b.set(&new_id(), Key::project("p", "name"), json!("Late edit"))
            .unwrap();
        merge(&mut b, &a);
        merge(&mut a, &b);
        let digest = a.digest();
        merge(&mut a, &b);
        assert_eq!(a.values()[&alive], false);
        assert_eq!(a.digest(), digest);
    }
    #[test]
    fn invalid_exchange_is_atomic_and_forbids_connection_fields() {
        let mut a = Catalog::default();
        let original = a.digest();
        let op = Operation {
            id: new_id(),
            actor: new_id(),
            clock: 1,
            key: Key::machine(&new_id(), "ssh_key"),
            parents: vec![],
            value: json!("private"),
        };
        let swarm = a.swarm_id.clone();
        assert!(a.merge(&swarm, vec![op]).is_err());
        assert_eq!(a.digest(), original);
        assert!(serde_json::from_value::<Catalog>(
            json!({"schema":1,"swarm_id":swarm,"operations":{},"credentials":"no"})
        )
        .is_err());
    }
    #[test]
    fn delivery_preference_validates_merges_and_defaults() {
        let (a, b) = (new_id(), new_id());
        let key = Key::preference("mobile_delivery");
        let mut first = Catalog::default();
        assert_eq!(first.preferences(), json!({"mobile_delivery": "away"}));
        assert!(first.set(&a, key.clone(), json!("later")).is_err());
        assert!(first.set(&a, key.clone(), json!(5)).is_err());
        assert!(first
            .set(&a, Key::preference("theme"), json!("away"))
            .is_err());
        let mut second = first.clone();
        first.set(&a, key.clone(), json!("away_5")).unwrap();
        second.set(&b, key.clone(), json!("immediate")).unwrap();
        merge(&mut first, &second);
        merge(&mut second, &first);
        assert_eq!(first.heads()[&key].len(), 2);
        assert_eq!(first.preferences(), second.preferences());
        first.set(&a, key.clone(), json!("away_30")).unwrap();
        merge(&mut second, &first);
        assert_eq!(second.heads()[&key].len(), 1);
        assert_eq!(second.preferences(), json!({"mobile_delivery": "away_30"}));
    }
    #[test]
    fn preference_keys_use_their_wire_tag_as_kind() {
        let key = Key::preference("mobile_delivery");
        assert_eq!(
            serde_json::to_value(&key).unwrap(),
            json!({"kind": "preference", "field": "mobile_delivery"})
        );
        for key in [
            Key::project("p", "name"),
            Key::Folder { id: "f".into() },
            Key::Session {
                machine: new_id(),
                session: "s".into(),
            },
            Key::machine(&new_id(), "name"),
            key,
        ] {
            assert_eq!(serde_json::to_value(&key).unwrap()["kind"], key.kind());
        }
    }
    #[test]
    fn legacy_members_receive_only_kinds_they_parse() {
        let preference = Key::preference("mobile_delivery");
        assert!(accepted(None, &Key::project("p", "name")));
        assert!(accepted(None, &Key::Folder { id: "f".into() }));
        assert!(!accepted(None, &preference));
        assert!(accepted(Some(&kinds()), &preference));
        let older: BTreeSet<String> = LEGACY_KINDS.iter().map(|k| (*k).to_owned()).collect();
        assert!(!accepted(Some(&older), &preference));
    }
    #[test]
    fn preferences_report_the_default_for_an_unvalidated_invalid_choice() {
        // The lock-free listing reads the store without validating it.
        let mut catalog = Catalog::default();
        let op = Operation {
            id: new_id(),
            actor: new_id(),
            clock: 1,
            key: Key::preference("mobile_delivery"),
            parents: vec![],
            value: json!("later"),
        };
        catalog.operations.insert(op.id.clone(), op);
        assert!(catalog.validate().is_err());
        assert_eq!(catalog.preferences(), json!({"mobile_delivery": "away"}));
    }
}
