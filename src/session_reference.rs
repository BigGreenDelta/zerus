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
    let (reference, provider) = caller(config)?
        .ok_or_else(|| Error::new(1, "hgs self: no live Zerus session owns this process"))?;
    print(&reference, as_json, json!({"provider": provider}))
}

/// The reference and provider of the live agent whose process runs this CLI,
/// under its current name; `None` when no agent's ancestry owns it.
fn caller(config: &Config) -> cli::Result<Option<(Reference, String)>> {
    let Some(record) = state::session_graph::caller_record() else {
        return Ok(None);
    };
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
    Ok(Some((reference, text("agent"))))
}

/// The text a guarded send of the calling agent delivers: `text` inside one
/// envelope naming the sending session, so the recipient can tell another
/// agent declared it and inspect or answer that session. The same text and
/// unchanged sender metadata give the same bytes. A rename or resumed run
/// changes that metadata: an old request ID then fails the ordinary content
/// guard, rather than gaining permission to resend uncertain input.
pub(crate) fn sender_envelope(text: &str) -> Option<String> {
    // Leave an oversized body to the unchanged input validator; do not expand
    // a rejected multi-megabyte JSON text into XML first.
    if text.len() > 64 * 1024 {
        return None;
    }
    let config = Config::load().ok()?;
    let (reference, _) = caller(&config).ok()??;
    let uri = reference.uri();
    Reference::parse(&uri).ok()?;
    Some(envelope(&reference.name, &reference.host, &uri, text))
}

fn envelope(name: &str, host: &str, uri: &str, text: &str) -> String {
    let body = |value: &str| value.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;");
    let attribute = |value: &str| body(value).replace('"', "&quot;");
    format!(
        "<zerus-message from=\"{}\" host=\"{}\" reference=\"{}\">\n{}\n</zerus-message>",
        attribute(name),
        attribute(host),
        attribute(uri),
        body(text)
    )
}

/// Decode only the complete, bounded XML subset hgs emits. The metadata is a
/// declaration, not authentication: anyone can type this frame. Never use it
/// as authority to route input or bypass the ordinary target/delivery guards.
pub(crate) fn sender_message(text: &str) -> Option<(String, String, String)> {
    if text.len() > 64 * 1024 {
        return None;
    }
    fn attribute(rest: &str) -> Option<(String, &str)> {
        let (value, rest) = rest.split_once('"')?;
        Some((xml_text(value)?, rest))
    }
    let rest = text.strip_prefix("<zerus-message from=\"")?;
    let (name, rest) = attribute(rest)?;
    let (host, rest) = attribute(rest.strip_prefix(" host=\"")?)?;
    let (uri, rest) = attribute(rest.strip_prefix(" reference=\"")?)?;
    let body = rest.strip_prefix(">\n")?.strip_suffix("\n</zerus-message>")?;
    let reference = Reference::parse(&uri).ok()?;
    if reference.name != name || reference.host != host {
        return None;
    }
    Some((name, uri, xml_text(body)?))
}

/// Named entities only, decoded once. No DTD, numeric references, nested
/// markup, bare ampersands or invalid XML characters can enter the text.
fn xml_text(value: &str) -> Option<String> {
    let mut decoded = String::with_capacity(value.len());
    let mut rest = value;
    while let Some(at) = rest.find('&') {
        let plain = &rest[..at];
        if !xml_plain(plain) {
            return None;
        }
        decoded.push_str(plain);
        let entity = &rest[at..];
        let (encoded, character) = [("&amp;", '&'), ("&lt;", '<'), ("&gt;", '>'), ("&quot;", '"')]
            .into_iter()
            .find(|(encoded, _)| entity.starts_with(*encoded))?;
        decoded.push(character);
        rest = &entity[encoded.len()..];
    }
    if !xml_plain(rest) {
        return None;
    }
    decoded.push_str(rest);
    Some(decoded)
}

fn xml_plain(value: &str) -> bool {
    !value.contains('<') && value.chars().all(|character| {
        matches!(character, '\t' | '\n' | '\r' | '\u{20}'..='\u{D7FF}' | '\u{E000}'..='\u{FFFD}' | '\u{10000}'..='\u{10FFFF}')
    })
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

    #[test]
    fn the_sender_envelope_names_an_exact_copyable_reference() {
        let reference = Reference {
            machine: "11111111-1111-4111-8111-111111111111".into(),
            host: "arch".into(),
            name: "claude/p/\"<review>\" & co".into(),
            conversation: "conversation".into(),
            run: "run".into(),
            archive: String::new(),
            agent: String::new(),
        };
        let uri = reference.uri();
        let text = envelope(&reference.name, &reference.host, &uri, "Line one.\n\nLine two.");
        let encoded_uri = uri.replace('&', "&amp;");
        assert_eq!(text, format!("<zerus-message from=\"claude/p/&quot;&lt;review&gt;&quot; &amp; co\" \
            host=\"arch\" reference=\"{encoded_uri}\">\nLine one.\n\nLine two.\n</zerus-message>"));
        let (sender, copied, body) = sender_message(&text).unwrap();
        assert_eq!(Reference::parse(&copied).unwrap().name, reference.name);
        assert_eq!((sender, copied, body), (reference.name.clone(), uri.clone(), "Line one.\n\nLine two.".into()));
        assert_eq!(envelope(&reference.name, &reference.host, &uri, ""), envelope(&reference.name, "arch", &uri, ""));
    }

    #[test]
    fn sender_xml_preserves_literal_text_without_expanding_it_twice() {
        let uri = "zerus://session?machine=11111111-1111-4111-8111-111111111111&host=arch&name=claude%2Fp%2Freview&conversation=c&run=r";
        let body = " \r\n雪 & <tag> \"quoted\" </zerus-message> &amp; &lt; &#65;\n ";
        let frame = envelope("claude/p/review", "arch", uri, body);
        assert!(frame.contains("&lt;/zerus-message&gt;"));
        assert!(frame.contains("&amp;amp; &amp;lt; &amp;#65;"));
        assert_eq!(sender_message(&frame), Some(("claude/p/review".into(), uri.into(), body.into())));
        assert_eq!(sender_message(&envelope("claude/p/review", "arch", uri, "")),
            Some(("claude/p/review".into(), uri.into(), "".into())));
        assert_eq!(frame, envelope("claude/p/review", "arch", uri, body));
    }

    #[test]
    fn sender_xml_refuses_malformed_framing_entities_or_declared_routes() {
        let uri = "zerus://session?machine=11111111-1111-4111-8111-111111111111&host=arch&name=claude%2Fp%2Freview&conversation=c&run=r";
        let frame = envelope("claude/p/review", "arch", uri, "BODY");
        for body in ["raw & value", "<nested/>", "</zerus-message>", "<!DOCTYPE x>", "&#65;", "&#x41;", "&unknown;", "&apos;", "\u{0}", "\u{FFFF}"] {
            assert!(sender_message(&frame.replace("BODY", body)).is_none(), "{body:?}");
        }
        for invalid in [
            frame.replace(" from=", " extra=\"x\" from="),
            frame.replace(" host=\"arch\"", " host=\"other\""),
            frame.replace("from=\"claude/p/review\"", "from=\"claude/p/other\""),
            frame.replace(" reference=", " reference=\"zerus://invalid\" ignored="),
            frame.replace("&amp;host=", "&host="),
            frame.replace("&amp;run=r", "&amp;run=r&amp;run=duplicate"),
            frame.replace("\">\nBODY", "\">BODY"),
            frame.replace("\n</zerus-message>", "</zerus-message>"),
            frame.replace("\n</zerus-message>", "…"),
            format!("{frame}\ntrailing"),
            format!("{frame}{frame}"),
            format!("<!DOCTYPE x>{frame}"),
            envelope("claude/p/review", "arch", "https://example.invalid", "BODY"),
            envelope("claude/p/review", "arch", "zerus://not-a-session", "BODY"),
        ] {
            assert!(sender_message(&invalid).is_none(), "{invalid}");
        }
        assert!(sender_message(&envelope("claude/p/review", "arch", uri, &"x".repeat(64 * 1024))).is_none());
    }
}
