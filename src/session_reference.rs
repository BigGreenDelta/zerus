//! Portable, read-only conversation references. Aliases route; machine IDs verify.
use crate::{
    cli::{self, Error},
    config::Config,
    state, swarm,
};
use serde_json::{json, Value};
use std::collections::BTreeMap;

#[derive(Debug)]
pub(crate) struct Reference {
    pub machine: String,
    pub host: String,
    pub name: String,
    pub conversation: String,
    pub run: String,
    pub archive: String,
    pub agent: String,
}
fn encode(value: &str) -> String {
    value
        .bytes()
        .map(|b| {
            if b.is_ascii_alphanumeric() || b"-._~".contains(&b) {
                (b as char).to_string()
            } else {
                format!("%{b:02X}")
            }
        })
        .collect()
}
fn decode(value: &str) -> cli::Result<String> {
    let mut bytes = Vec::new();
    let mut input = value.bytes();
    while let Some(b) = input.next() {
        if b == b'%' {
            let a = input.next().and_then(|v| (v as char).to_digit(16));
            let c = input.next().and_then(|v| (v as char).to_digit(16));
            let (a, c) = a
                .zip(c)
                .ok_or_else(|| Error::new(1, "Invalid session reference escape"))?;
            bytes.push((a * 16 + c) as u8);
        } else if b.is_ascii_alphanumeric() || b"-._~".contains(&b) {
            bytes.push(b);
        } else {
            return Err(Error::new(1, "Invalid session reference encoding"));
        }
    }
    String::from_utf8(bytes).map_err(|_| Error::new(1, "Invalid session reference text"))
}
impl Reference {
    pub fn parse(uri: &str) -> cli::Result<Self> {
        let query = uri
            .strip_prefix("zerus://session?")
            .filter(|_| uri.len() <= 8192)
            .ok_or_else(|| Error::new(1, "Invalid Zerus session reference"))?;
        let mut fields = BTreeMap::new();
        for pair in query.split('&') {
            let (key, value) = pair
                .split_once('=')
                .ok_or_else(|| Error::new(1, "Invalid session reference field"))?;
            let key = decode(key)?;
            if ![
                "machine",
                "host",
                "name",
                "conversation",
                "run",
                "archive",
                "agent",
            ]
            .contains(&key.as_str())
                || fields.insert(key, decode(value)?).is_some()
            {
                return Err(Error::new(
                    1,
                    "Unknown or duplicate session reference field",
                ));
            }
        }
        let take = |key: &str| fields.get(key).cloned().unwrap_or_default();
        let reference = Self {
            machine: take("machine"),
            host: take("host"),
            name: take("name"),
            conversation: take("conversation"),
            run: take("run"),
            archive: take("archive"),
            agent: take("agent"),
        };
        if !uuid::Uuid::parse_str(&reference.machine)
            .is_ok_and(|id| !id.is_nil() && id.to_string() == reference.machine)
            || reference.host.is_empty()
            || reference.host.len() > 80
            || reference.host.starts_with('-')
            || !reference
                .host
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b"_.-".contains(&b))
            || reference.name.is_empty()
            || reference.name.len() > 512
            || !reference.name.contains('/')
            || (reference.conversation.is_empty() && reference.run.is_empty())
            || [
                &reference.name,
                &reference.conversation,
                &reference.run,
                &reference.archive,
                &reference.agent,
            ]
            .iter()
            .any(|v| v.len() > 512 || v.chars().any(char::is_control))
        {
            return Err(Error::new(1, "Invalid session reference identity"));
        }
        Ok(reference)
    }
    fn uri(&self) -> String {
        let fields = [
            ("machine", &self.machine),
            ("host", &self.host),
            ("name", &self.name),
            ("conversation", &self.conversation),
            ("run", &self.run),
            ("archive", &self.archive),
            ("agent", &self.agent),
        ];
        format!(
            "zerus://session?{}",
            fields
                .iter()
                .filter(|(_, v)| !v.is_empty())
                .map(|(k, v)| format!("{k}={}", encode(v)))
                .collect::<Vec<_>>()
                .join("&")
        )
    }
}
pub(crate) fn create(config: &Config, args: &[String]) -> cli::Result<i32> {
    let name = args.first().ok_or_else(|| {
        Error::new(
            1,
            "usage: hgs reference <session> --json [--archive ID] [--agent ID]",
        )
    })?;
    let mut archive = String::new();
    let mut agent = String::new();
    let mut expected_run = None;
    let mut expected_conversation = None;
    let mut as_json = false;
    let mut index = 1;
    while index < args.len() {
        if args[index] == "--json" && !as_json {
            as_json = true;
            index += 1;
            continue;
        }
        let value = args
            .get(index + 1)
            .ok_or_else(|| Error::new(1, "Missing reference option value"))?;
        match args[index].as_str() {
            "--archive" if archive.is_empty() => archive = value.clone(),
            "--agent" if agent.is_empty() => agent = value.clone(),
            "--run" if expected_run.is_none() => expected_run = Some(value.as_str()),
            "--conversation" if expected_conversation.is_none() => {
                expected_conversation = Some(value.as_str())
            }
            _ => return Err(Error::new(1, "Invalid reference option")),
        }
        index += 2;
    }
    let details = state::reference_details(name, &archive, "").map_err(|e| Error::new(1, e))?;
    let text = |key: &str| details[key].as_str().unwrap_or_default().to_owned();
    if expected_run.is_some_and(|id| id != text("run_id"))
        || expected_conversation.is_some_and(|id| id != text("conversation_id"))
    {
        return Err(Error::new(
            1,
            "Session changed before copying its reference; refresh it",
        ));
    }
    if !agent.is_empty() {
        let child =
            state::reference_details(name, &archive, &agent).map_err(|e| Error::new(1, e))?;
        if child["parent_conversation_id"] != details["conversation_id"] {
            return Err(Error::new(
                1,
                "Parent conversation changed before copying its child reference",
            ));
        }
    }
    let reference = Reference {
        machine: swarm::local_node_id(config).map_err(|e| Error::new(1, e.to_string()))?,
        host: config.host().into(),
        name: name.clone(),
        conversation: text("conversation_id"),
        run: text("run_id"),
        archive,
        agent,
    };
    print(&reference, as_json, json!({}))
}

fn print(reference: &Reference, as_json: bool, extra: Value) -> cli::Result<i32> {
    let uri = reference.uri();
    Reference::parse(&uri)?;
    if as_json {
        let mut output = json!({"schema":1,"uri":uri,"name":reference.name,"machine_id":reference.machine,
        "host":reference.host,"run_id":reference.run,"conversation_id":reference.conversation,"archive_id":reference.archive,"agent_id":reference.agent});
        for (key, value) in extra.as_object().into_iter().flatten() {
            output[key] = value.clone();
        }
        println!("{output}");
    } else {
        println!("{uri}");
    }
    Ok(0)
}

/// `hgs self`: the reference of the agent whose process runs this CLI, under
/// its current name. Only verified native ancestry identifies the caller.
pub(crate) fn current(config: &Config, args: &[String]) -> cli::Result<i32> {
    let as_json = match args {
        [] => false,
        [flag] if flag == "--json" => true,
        _ => return Err(Error::new(1, "usage: hgs self [--json]")),
    };
    let record = state::session_graph::caller_record().ok_or_else(|| {
        Error::new(1, "hgs self: no live Zerus session owns this process")
    })?;
    let text = |key: &str| record[key].as_str().unwrap_or_default().to_owned();
    let reference = Reference {
        machine: swarm::local_node_id(config).map_err(|e| Error::new(1, e.to_string()))?,
        host: config.host().into(),
        name: text("name"),
        conversation: text("conversation_id"),
        run: text("run_id"),
        archive: String::new(),
        agent: String::new(),
    };
    print(&reference, as_json, json!({"provider": text("agent")}))
}
pub(crate) fn inspect(config: &Config, reference: &Reference) -> cli::Result<Value> {
    if swarm::local_node_id(config).map_err(|e| Error::new(1, e.to_string()))? != reference.machine
    {
        return Err(Error::new(
            1,
            "Session reference belongs to another computer; use a configured @host",
        ));
    }
    state::inspect_reference(reference).map_err(|e| Error::new(1, e))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn strict_uri_round_trip_and_ambiguous_fields() {
        let reference = Reference {
            machine: "11111111-1111-4111-8111-111111111111".into(),
            host: "fixture".into(),
            name: "claude/project/a & б".into(),
            conversation: "conversation".into(),
            run: "run".into(),
            archive: String::new(),
            agent: String::new(),
        };
        let uri = reference.uri();
        assert_eq!(Reference::parse(&uri).unwrap().name, reference.name);
        for tail in [
            "&machine=other",
            "&%6Eame=other",
            "&secret=x",
            "#fragment",
            "&agent=%00",
            "&agent=%ZZ",
            "&agent=%FF",
            "&",
        ] {
            assert!(Reference::parse(&(uri.clone() + tail)).is_err(), "{tail}");
        }
    }
}
