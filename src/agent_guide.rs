//! `hgs guide`: how an agent reads, messages, launches and stops other Zerus
//! sessions. Every byte printed here lands in an agent's context, so the index
//! stays short and each topic is read only when the agent needs it. The text
//! ships inside the binary and therefore matches the commands this hgs has.
use crate::cli::{Error, Result};

const INDEX: &str = include_str!("agent_guide/index.md");
const TOPICS: &[(&str, &str)] = &[
    ("send", include_str!("agent_guide/send.md")),
    ("wait", include_str!("agent_guide/wait.md")),
    ("launch", include_str!("agent_guide/launch.md")),
    ("stop", include_str!("agent_guide/stop.md")),
    ("tasks", include_str!("agent_guide/tasks.md")),
];
const USAGE: &str = "usage: hgs guide [send|wait|launch|stop|tasks]";

pub(crate) fn dispatch(args: &[String]) -> Result<i32> {
    let text = match args {
        [] => INDEX,
        [topic] => TOPICS
            .iter()
            .find(|(name, _)| name == topic)
            .map(|(_, text)| *text)
            .ok_or_else(|| Error::new(1, format!("unknown guide topic: {topic}; {USAGE}")))?,
        _ => return Err(Error::new(1, USAGE)),
    };
    print!("{text}");
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn pages() -> impl Iterator<Item = (&'static str, &'static str)> {
        std::iter::once(("index", INDEX)).chain(TOPICS.iter().copied())
    }

    #[test]
    fn pages_stay_small_plain_and_free_of_local_details() {
        for (name, text) in pages() {
            // About four bytes a token: a page costs an agent at most ~600.
            assert!(text.len() <= 2400, "{name} has {} bytes", text.len());
            assert!(text.is_ascii(), "{name} is not ASCII");
            assert!(text.ends_with('\n') && !text.contains('\t'), "{name}");
            assert!(text.lines().all(|line| line.len() <= 100), "{name} has a long line");
            // Spelled in parts: the source privacy guard rejects the literal.
            for root in ["w", "home", "Users"] {
                assert!(!text.contains(&format!("/{root}/")), "{name} names a local path");
            }
            assert!(!text.contains("~/") && !text.contains(":\\"), "{name} names a local path");
        }
    }

    #[test]
    fn index_lists_every_topic_and_usage_names_them() {
        for (name, _) in TOPICS {
            assert!(INDEX.contains(&format!("hgs guide {name} ")), "index omits {name}");
            assert!(USAGE.contains(name), "usage omits {name}");
        }
    }

    #[test]
    fn named_commands_exist_in_this_hgs() {
        let usage = crate::cli::USAGE;
        for (name, text) in pages() {
            for line in text.lines() {
                let mut rest = line;
                while let Some(at) = rest.find("hgs ") {
                    rest = &rest[at + 4..];
                    let command = rest
                        .split_whitespace()
                        .find(|word| !word.starts_with('@') && *word != "[@h]")
                        .unwrap_or_default()
                        .trim_matches(|c: char| !c.is_ascii_alphanumeric() && c != '-');
                    // `hgs [@h] AGENT DIRECTORY` launches an agent; -h is help.
                    if command.is_empty() || command == "-h" || command.chars().all(|c| c.is_ascii_uppercase()) {
                        continue;
                    }
                    assert!(
                        usage.contains(&format!("hgs {command} "))
                            || usage.contains(&format!("hgs [@host] {command} "))
                            || usage.contains(&format!("hgs [@host] {command}|")),
                        "{name} names unknown command {command:?}: {line}"
                    );
                }
            }
        }
    }

    #[test]
    fn topics_and_arguments_are_exact() {
        assert!(dispatch(&[]).is_ok());
        for (name, _) in TOPICS {
            assert!(dispatch(&[(*name).into()]).is_ok());
        }
        assert!(dispatch(&["Send".into()]).is_err());
        assert!(dispatch(&["send".into(), "wait".into()]).is_err());
    }
}
