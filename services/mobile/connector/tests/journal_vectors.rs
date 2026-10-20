#![cfg(unix)]
//! `journal_traces.jsonl` and `journal_expect.json`: the Rust journal replays
//! every step the Python `Journal` was recorded taking, on fresh state and on
//! Python-written, legacy, foreign and capped fixtures.
mod common;

use common::*;
use serde_json::{json, Value};
use std::collections::HashMap;
use std::path::Path;
use zerus_mobile_connector::journal::{Journal, JournalError, JOURNAL};

struct Run<'a> {
    scratch: &'a Scratch,
    journals: HashMap<String, Journal>,
    now: f64,
}

impl Run<'_> {
    fn state(&self) -> std::path::PathBuf {
        self.scratch.path("state")
    }

    fn step(&mut self, step: &Value) -> Value {
        if let Some(at) = step["at"].as_f64() {
            self.now = at;
        }
        let instance = step["instance"].as_str().unwrap_or("a").to_owned();
        let now = self.now;
        let raised = |error: JournalError| match error {
            JournalError::Refused(error) => {
                json!({"error": error.message(), "raised": "ConnectorError"})
            }
            other => panic!("unexpected journal failure: {other:?}"),
        };
        let journal = self.journals.get(&instance);
        let request_id = step["request_id"].as_str().unwrap_or_default();
        match step["op"].as_str().unwrap() {
            "setup" => {
                setup(&self.scratch.root, &step["actions"]);
                json!({})
            }
            "open" => match Journal::open(&self.state(), step["binding"].as_str().unwrap(), now) {
                Ok(journal) => {
                    self.journals.insert(instance, journal);
                    json!({"ok": true})
                }
                Err(error) => raised(error),
            },
            "close" => {
                self.journals.remove(&instance).unwrap().close();
                json!({})
            }
            "claim" => match journal.unwrap().claim(
                request_id,
                step["operation"].as_str().unwrap(),
                step["request_hash"].as_str(),
                now,
            ) {
                Ok(claimed) => json!({"returns": claimed}),
                Err(error) => raised(error),
            },
            "finish" => {
                journal
                    .unwrap()
                    .finish(request_id, &step["response"], now)
                    .unwrap();
                json!({})
            }
            "replay" => {
                let text = journal.unwrap().replay(request_id).unwrap();
                json!({"returns": serde_json::from_str::<Value>(&text).unwrap()})
            }
            "pending" => {
                let mut pending = journal.unwrap().pending().unwrap();
                pending.sort();
                let listed: Vec<Value> = pending
                    .into_iter()
                    .map(|(id, text)| json!([id, serde_json::from_str::<Value>(&text).unwrap()]))
                    .collect();
                json!({"order": "unspecified", "returns": listed})
            }
            "delivered" => {
                journal.unwrap().delivered(request_id, now).unwrap();
                json!({})
            }
            "prune" => {
                journal.unwrap().prune(now).unwrap();
                json!({})
            }
            "rows" => json!({"returns": rows(&self.state().join(JOURNAL))}),
            "schema" => {
                let mut schema = schema(&self.state().join(JOURNAL));
                if let Some(journal) = journal {
                    let pragmas: serde_json::Map<String, Value> = [
                        "synchronous",
                        "secure_delete",
                        "busy_timeout",
                        "foreign_keys",
                    ]
                    .into_iter()
                    .map(|name| {
                        (
                            name.to_owned(),
                            json!(journal.connection_pragma(name).unwrap()),
                        )
                    })
                    .collect();
                    schema.insert("connection_pragmas".into(), Value::Object(pragmas));
                }
                json!({"returns": Value::Object(schema)})
            }
            "files" => json!({"returns": tree(&self.scratch.root)}),
            "sql" => {
                rusqlite::Connection::open(self.state().join(JOURNAL))
                    .unwrap()
                    .execute_batch(step["sql"].as_str().unwrap())
                    .unwrap();
                json!({})
            }
            other => panic!("unknown journal step {other}"),
        }
    }
}

fn replay(label: &str, steps: &Value, fixture: Option<&Path>) -> usize {
    umask();
    let scratch = Scratch::new(label);
    if let Some(fixture) = fixture {
        load_fixture(
            &scratch.path("state"),
            &std::fs::read_to_string(fixture).unwrap(),
        );
    }
    let mut run = Run {
        scratch: &scratch,
        journals: HashMap::new(),
        now: EPOCH,
    };
    let steps = steps.as_array().unwrap();
    for (index, step) in steps.iter().enumerate() {
        let actual = run.step(step);
        let expected = &step["expect"];
        let context = format!("{label} step {index} ({})", step["op"]);
        if step["op"] == "rows" {
            let actual = actual["returns"].as_array().unwrap();
            assert!(
                same_rows(actual, &expected["returns"]),
                "{context}\n actual: {actual:?}\nexpected: {}",
                expected["returns"]
            );
        } else {
            assert_same(&actual, expected, &context);
        }
    }
    run.journals.clear();
    steps.len()
}

#[test]
fn journal_traces_match_the_python_journal() {
    let mut steps = 0;
    let mut written = Vec::new();
    for record in records("journal_traces.jsonl") {
        steps += replay(record["id"].as_str().unwrap(), &record["steps"], None);
        if let Some(fixture) = record["writes_fixture"].as_str() {
            written.push(fixture.to_owned());
        }
    }
    assert_eq!(steps, 163);
    assert_eq!(
        written,
        [
            "journal/python-fresh.sql",
            "journal/python-running.sql",
            "journal/python-retention.sql"
        ]
    );
}

#[test]
fn python_written_and_legacy_journals_reopen_as_python_reopens_them() {
    let expect = document("journal_expect.json");
    let mut scenarios = 0;
    for scenario in expect["scenarios"].as_array().unwrap() {
        let fixture = vectors().join(scenario["fixture"].as_str().unwrap());
        replay(
            scenario["id"].as_str().unwrap(),
            &scenario["steps"],
            Some(&fixture),
        );
        scenarios += 1;
    }
    assert_eq!(scenarios, 7);
}

/// A trace's fixture is what the Rust journal writes for the same steps:
/// the rows and schema equal Python's dump, receipts by meaning and length.
#[test]
fn rust_written_journals_equal_the_python_written_fixtures() {
    for record in records("journal_traces.jsonl") {
        let Some(fixture) = record["writes_fixture"].as_str() else {
            continue;
        };
        let id = record["id"].as_str().unwrap();
        umask();
        let written = Scratch::new(&format!("written-{id}"));
        let mut run = Run {
            scratch: &written,
            journals: HashMap::new(),
            now: EPOCH,
        };
        for step in record["steps"].as_array().unwrap() {
            run.step(step);
        }
        run.journals.clear();
        let python = Scratch::new(&format!("python-{id}"));
        load_fixture(
            &python.path("state"),
            &std::fs::read_to_string(vectors().join(fixture)).unwrap(),
        );
        let ours = written.path("state").join(JOURNAL);
        let theirs = python.path("state").join(JOURNAL);
        assert_same(
            &Value::Object(schema(&ours)),
            &Value::Object(schema(&theirs)),
            id,
        );
        assert!(
            same_rows(&rows(&ours), &Value::Array(rows(&theirs))),
            "{id}"
        );
    }
}
