//! Relationship identities are independent of mutable display names and routes.
//! Only host-owned binding metadata enters this projection; no message text is
//! interpreted as an authenticated sender, a child, or a completed task.
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;

pub(super) fn id(kind: &str, parts: &Value) -> String {
    format!("{kind}:{:x}", Sha256::digest(parts.to_string().as_bytes()))
}

fn text<'a>(value: &'a Value, key: &str) -> &'a str {
    value[key].as_str().unwrap_or_default()
}
fn valid(value: &str, max: usize) -> bool {
    !value.is_empty() && value.len() <= max && !value.chars().any(char::is_control)
}
fn label(value: &Value, key: &str, max: usize) -> String {
    text(value, key)
        .chars()
        .filter(|c| !c.is_control())
        .take(max)
        .collect()
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(deny_unknown_fields)]
pub(super) struct Endpoint {
    pub machine: String,
    pub provider: String,
    pub namespace: String,
    pub conversation: String,
    pub provisional_run: String,
}

impl Endpoint {
    pub fn valid(&self) -> bool {
        uuid::Uuid::parse_str(&self.machine)
            .is_ok_and(|id| !id.is_nil() && id.to_string() == self.machine)
            && ["claude", "codex", "kimi", "dsh", "opencode"].contains(&self.provider.as_str())
            && self.namespace.len() == 72
            && self.namespace.starts_with("account:")
            && self.namespace[8..]
                .bytes()
                .all(|c| c.is_ascii_hexdigit() && !c.is_ascii_uppercase())
            && (valid(&self.conversation, 512) && self.provisional_run.is_empty()
                || self.conversation.is_empty() && valid(&self.provisional_run, 128))
    }
    pub fn from_record(machine: &str, record: &Value) -> Option<Self> {
        let provider = text(record, "agent");
        let conversation = text(record, "conversation_id");
        let run = text(record, "run_id");
        if !uuid::Uuid::parse_str(machine).is_ok_and(|id| !id.is_nil() && id.to_string() == machine)
            || !["claude", "codex", "kimi", "dsh", "opencode"].contains(&provider)
            || !valid(run, 128)
            || (!conversation.is_empty() && !valid(conversation, 512))
            || record["creation_pending"] == true
        {
            return None;
        }
        // Account IDs and native homes are namespaces, never display labels.
        // Keep the private home out of the projected metadata.
        let namespace = id(
            "account",
            &json!([
                text(record, "account_id"),
                text(record, "agent_home"),
                text(record, "native_data_home"),
                text(record, "native_config_file")
            ]),
        );
        Some(Self {
            machine: machine.into(),
            provider: provider.into(),
            namespace,
            conversation: conversation.into(),
            provisional_run: if conversation.is_empty() {
                run.into()
            } else {
                String::new()
            },
        })
    }
    pub fn key(&self) -> String {
        id("node", &json!(self))
    }
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub(super) struct Item {
    pub kind: String,
    pub value: Value,
}

pub(super) type Projection = BTreeMap<String, Item>;

fn insert(items: &mut Projection, key: String, kind: &str, value: Value) {
    items.insert(
        key,
        Item {
            kind: kind.into(),
            value,
        },
    );
}

/// One authoritative binding contributes facts. Multiple bindings can point at
/// one conversation, while action targets remain separate and exact.
pub(super) fn project(machine: &str, record: &Value) -> Option<Projection> {
    let endpoint = Endpoint::from_record(machine, record)?;
    let name = text(record, "name");
    if !valid(name, 512) || record["rename_shadow"] == true {
        return None;
    }
    let mut items = Projection::new();
    let node = endpoint.key();
    insert(
        &mut items,
        node.clone(),
        "node",
        json!({"endpoint":endpoint,"provisional":endpoint.conversation.is_empty()}),
    );
    let archive = text(record, "archive_id");
    let binding = if archive.is_empty() {
        id("binding", &json!([machine, text(record, "run_id")]))
    } else {
        id(
            "binding",
            &json!([machine, text(record, "run_id"), archive]),
        )
    };
    let children: serde_json::Map<String,Value>=record["subagents"].as_object().into_iter().flatten().take(256)
        .filter(|(id,c)|valid(id,160)&&!id.contains(['/', '\\'])&&c.is_object()&&c["group"]!=true)
        .map(|(id,c)|(id.clone(),json!({"name":label(c,"name",240),
            "state":if ["working","finished"].contains(&text(c,"state")){text(c,"state")}else{"unknown"},
            "started":c["started"].as_f64().filter(|n|n.is_finite()&&*n>0.0),
            "provider":if ["claude","codex","kimi","dsh","opencode"].contains(&text(c,"provider")){text(c,"provider")}else{endpoint.provider.as_str()}}))).collect();
    let summary = json!({"name":name,"cmd":endpoint.provider,"run_id":record["run_id"],"conversation_id":endpoint.conversation,
        "archive_id":archive,"archived_at":record["archived_at"].as_f64().filter(|n|n.is_finite()&&(0.0..253402300800.0).contains(n)).unwrap_or(0.0),"state":if archive.is_empty(){"stopped"}else{"archived"},
        "title":label(record,"label",240),"cwd":label(record,"cwd",1024),"tracked":true,"resumable":false,"subagents":children});
    insert(
        &mut items,
        binding.clone(),
        "binding",
        json!({
            "node":node,"name":name,"run_id":record["run_id"],
            "conversation_id":endpoint.conversation,"provider":endpoint.provider,
            "label":label(record,"label",240),"cwd":label(record,"cwd",1024),
            "project_id":label(record,"project_id",160),
            "archived":record["state"]=="archived", "archive_id":archive,
            "summary":summary,
            "source":"saved_binding"
        }),
    );
    // A known run upgrading to its confirmed conversation is the only alias
    // this collector can establish. Display-name matches never create aliases.
    if !endpoint.conversation.is_empty() {
        let mut provisional = endpoint.clone();
        provisional.conversation.clear();
        provisional.provisional_run = text(record, "run_id").into();
        insert(
            &mut items,
            id("alias", &json!([provisional.key(), node])),
            "alias",
            json!({"from":provisional.key(),"to":node,"binding":binding}),
        );
    }
    let parent = text(record, "fork_parent_id");
    if !endpoint.conversation.is_empty()
        && valid(parent, 512)
        && parent != endpoint.conversation
        && text(record, "expected_id").is_empty()
    {
        let mut origin = endpoint.clone();
        origin.conversation = parent.into();
        insert(
            &mut items,
            id("relation", &json!(["fork", origin.key(), node])),
            "relation",
            json!({"type":"fork","from":origin.key(),"to":node,
                "source_endpoint":origin,"target_endpoint":endpoint,
                "evidence":"native_fork_binding","binding":binding}),
        );
    }
    // The persisted roster is already populated by native hooks/adapters. A
    // profile aggregate is not a roster and must not invent individual agents.
    if let Some(children) = record["subagents"].as_object() {
        for (child_id, child) in children.iter().take(256) {
            if !valid(child_id, 160)
                || child_id.contains(['/', '\\'])
                || !child.is_object()
                || child["group"] == true
            {
                continue;
            }
            let generation = child
                .get("started")
                .filter(|v| v.as_f64().is_some_and(|n| n.is_finite() && n > 0.0));
            let generation = generation
                .cloned()
                .unwrap_or_else(|| json!(text(record, "run_id")));
            let key = id("child", &json!([node, child_id, generation]));
            insert(
                &mut items,
                key.clone(),
                "child",
                json!({
                    "parent":node,"agent_id":child_id,"generation":generation,
                    "label":label(child,"name",240),"provider":if child["external"]==true && ["claude","codex","kimi","dsh","opencode"].contains(&text(child,"provider")){text(child,"provider")}else{&endpoint.provider},
                    "binding":binding,"source":"native_roster"
                    ,"state":if ["working","finished"].contains(&text(child,"state")){text(child,"state")}else{"unknown"}
                }),
            );
            insert(
                &mut items,
                id("relation", &json!(["parent", node, key])),
                "relation",
                json!({"type":"parent","from":node,"to":key,"evidence":"native_roster","binding":binding}),
            );
        }
    }
    Some(items)
}

/// New exchanges have two independently scoped native identities. A readable
/// name, a quoted envelope or a request supplied source string is never proof.
pub(super) fn exchange(machine: &str, record: &Value) -> Option<Projection> {
    let mut items = Projection::new();
    if record["status"] != "submitted" {
        return Some(items);
    }
    let from: Endpoint = serde_json::from_value(record["source"]["endpoint"].clone()).ok()?;
    let to: Endpoint = serde_json::from_value(record["target"]["endpoint"].clone()).ok()?;
    let request = text(record, "request_id");
    let event = text(record, "id");
    if record["schema"] != 1
        || !from.valid()
        || !to.valid()
        || from.machine != machine
        || record["source"]["node"] != from.key()
        || record["target"]["node"] != to.key()
        || !uuid::Uuid::parse_str(request).is_ok_and(|id| !id.is_nil() && id.to_string() == request)
        || !record["at"]
            .as_f64()
            .is_some_and(|at| at.is_finite() && (0.0..253402300800.0).contains(&at))
        || event
            != id(
                "event",
                &json!([
                    record["source"]["node"],
                    request,
                    record["route"],
                    record["name"]
                ]),
            )
        || record["target"]["name"] != record["name"]
        || record["target"]["run_id"] != record["expected_run_id"]
        || to.conversation != text(record, "expected_conversation_id")
        || !valid(text(record, "expected_run_id"), 128)
        || !valid(text(&record["source"], "run_id"), 128)
        || !valid(text(record, "name"), 512)
        || !valid(text(&record["source"], "name"), 512)
    {
        return None;
    }
    insert(
        &mut items,
        event.into(),
        "event",
        json!({"type":"message","from":from.key(),"to":to.key(),
        "source_endpoint":from,"target_endpoint":to,"request_id":request,"at":record["at"],
        "submitted_at":record["submitted_at"].as_f64().filter(|n|n.is_finite()&&(0.0..253402300800.0).contains(n)),"delivery_state":"submitted",
        "evidence":"verified_zerus_delivery","source_name":label(&record["source"],"name",512),
        "target_name":label(record,"name",512),"source_run":record["source"]["run_id"],
        "target_run":record["expected_run_id"],"source_locator":event}),
    );
    Some(items)
}

#[cfg(test)]
mod tests {
    use super::*;
    const MACHINE: &str = "11111111-1111-4111-8111-111111111111";
    fn record() -> Value {
        json!({"name":"codex/example/review","agent":"codex","run_id":"run-one",
        "conversation_id":"native-one","account_id":"work","agent_home":"/fixture/account"})
    }
    fn endpoint(r: &Value) -> Endpoint {
        Endpoint::from_record(MACHINE, r).unwrap()
    }
    fn delivery() -> Value {
        let source = endpoint(&record());
        let mut target = source.clone();
        target.conversation = "recipient-native".into();
        let request = "11111111-1111-4111-8111-111111111112";
        let key = id(
            "event",
            &json!([source.key(), request, "mac", "claude/example/recipient"]),
        );
        json!({"schema":1,"id":key,"status":"submitted","source":{"endpoint":source,"node":source.key(),"name":"codex/example/review","run_id":"source-run"},
            "target":{"endpoint":target,"node":target.key(),"name":"claude/example/recipient","run_id":"recipient-run"},
            "request_id":request,"route":"mac","name":"claude/example/recipient","expected_run_id":"recipient-run","expected_conversation_id":"recipient-native","at":100.0})
    }
    #[test]
    fn delivery_projection_keeps_distinct_requests_without_private_bodies() {
        let mut event = delivery();
        event["text"] = json!("private prompt");
        event["attachments"] = json!(["private-file"]);
        let projected = exchange(MACHINE, &event).unwrap();
        assert_eq!(projected.len(), 1);
        let (_, item) = projected.first_key_value().unwrap();
        assert_eq!(item.kind, "event");
        assert_eq!(item.value["evidence"], "verified_zerus_delivery");
        let bytes = serde_json::to_string(&projected).unwrap();
        assert!(!bytes.contains("private prompt"));
        assert!(!bytes.contains("private-file"));
        event["request_id"] = json!("11111111-1111-4111-8111-111111111113");
        event["id"] = json!(id(
            "event",
            &json!([
                event["source"]["node"],
                event["request_id"],
                event["route"],
                event["name"]
            ])
        ));
        let next = exchange(MACHINE, &event).unwrap();
        assert_ne!(
            projected.first_key_value().unwrap().0,
            next.first_key_value().unwrap().0
        );
    }
    #[test]
    fn unconfirmed_or_mismatched_delivery_never_infers_an_arrow() {
        let event = delivery();
        for field in ["status", "target"] {
            let mut missing = event.clone();
            missing[field] = Value::Null;
            if field == "status" {
                assert!(exchange(MACHINE, &missing).unwrap().is_empty());
            } else {
                assert!(exchange(MACHINE, &missing).is_none());
            }
        }
        for (field, value) in [
            ("request_id", json!("human label")),
            ("id", json!("event:forged")),
            ("expected_run_id", json!("reused-name-run")),
            ("expected_conversation_id", json!("other-native")),
            ("at", json!(-1)),
        ] {
            let mut invalid = event.clone();
            invalid[field] = value;
            assert!(exchange(MACHINE, &invalid).is_none(), "{field}");
        }
        assert!(exchange("22222222-2222-4222-8222-222222222222", &event).is_none());
        let mut invalid = event.clone();
        invalid["target"]["endpoint"]["namespace"] = json!("account:wrong");
        assert!(exchange(MACHINE, &invalid).is_none());
    }
    #[test]
    fn rename_resume_and_labels_do_not_change_conversation_identity() {
        let a = record();
        let mut b = a.clone();
        b["name"] = json!("codex/other/renamed");
        b["label"] = json!("Same display name");
        b["run_id"] = json!("resumed");
        b["cwd"] = json!("/other/project");
        assert_eq!(endpoint(&a).key(), endpoint(&b).key());
        assert_ne!(project(MACHINE, &a), project(MACHINE, &b));
    }
    #[test]
    fn machine_provider_account_and_clear_are_separate() {
        let a = record();
        let original = endpoint(&a).key();
        for (field, value) in [
            ("agent", "claude"),
            ("account_id", "personal"),
            ("conversation_id", "cleared"),
            ("agent_home", "/other/account"),
        ] {
            let mut b = a.clone();
            b[field] = json!(value);
            assert_ne!(original, endpoint(&b).key(), "{field}");
        }
        assert_ne!(
            original,
            Endpoint::from_record("22222222-2222-4222-8222-222222222222", &a)
                .unwrap()
                .key()
        );
    }
    #[test]
    fn only_confirmed_run_upgrades_provisional_identity() {
        let mut a = record();
        a["conversation_id"] = json!("");
        let prior = endpoint(&a).key();
        let b = record();
        let projection = project(MACHINE, &b).unwrap();
        assert!(projection.values().any(|i| i.kind == "alias"
            && i.value["from"] == prior
            && i.value["to"] == endpoint(&b).key()));
        a["run_id"] = json!("another-run");
        assert_ne!(prior, endpoint(&a).key());
        a["creation_pending"] = json!(true);
        assert!(project(MACHINE, &a).is_none());
    }
    #[test]
    fn no_name_or_message_inference_and_no_private_body_projection() {
        let mut a = record();
        a["prompt"] = json!("Message from codex/example/other");
        a["detail"] = json!("private transcript text");
        a["subagent_groups"] = json!({"reviewer":{"count":8}});
        let p = project(MACHINE, &a).unwrap();
        assert!(!p.values().any(|i| i.kind == "relation"));
        let bytes = serde_json::to_string(&p).unwrap();
        assert!(!bytes.contains("private transcript"));
        assert!(!bytes.contains("/fixture/account"));
    }
    #[test]
    fn child_generations_and_parent_namespaces_do_not_collapse() {
        let mut a = record();
        a["subagents"] = json!({"worker":{"name":"Reviewer","started":1.0}});
        let child = |r: &Value| {
            project(MACHINE, r)
                .unwrap()
                .into_iter()
                .find(|(_, i)| i.kind == "child")
                .unwrap()
                .0
        };
        let original = child(&a);
        a["subagents"]["worker"]["started"] = json!(2.0);
        assert_ne!(original, child(&a));
        a["subagents"]["worker"]["started"] = json!(1.0);
        a["conversation_id"] = json!("other");
        assert_ne!(original, child(&a));
    }
    #[test]
    fn fork_preserves_distinct_identity_and_requires_confirmation() {
        let mut a = record();
        a["fork_parent_id"] = json!("native-parent");
        assert!(project(MACHINE, &a)
            .unwrap()
            .values()
            .any(|i| i.kind == "relation" && i.value["type"] == "fork"));
        a["expected_id"] = json!("unconfirmed");
        assert!(!project(MACHINE, &a)
            .unwrap()
            .values()
            .any(|i| i.kind == "relation"));
    }
}
