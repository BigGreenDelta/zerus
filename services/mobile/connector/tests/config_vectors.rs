#![cfg(unix)]
//! `config.jsonl`, `identities.json`, `request_hash.jsonl` and the operation
//! sets of `operations.json`, against the library.
mod common;

use common::*;
use serde_json::Value;
use zerus_mobile_connector::capabilities::{self, Class};
use zerus_mobile_connector::config::{self, Environment};
use zerus_mobile_connector::error::{
    INVALID_INTERVAL, INVALID_JSON, INVALID_SERVER_URL, INVALID_STATE, NON_ASCII_HOST,
    OTHER_USER_HOME, STATE_UNAVAILABLE,
};
use zerus_mobile_connector::journal::{self, Journal};
use zerus_mobile_connector::{pyjson, request, url};

/// Rows where Python and Rust deliberately differ (README divergences), and
/// what Rust does instead. Any other row marked `divergence` fails the test
/// until it is reviewed here.
fn rust_refusal(id: &str) -> Option<&'static str> {
    Some(match id {
        "poll-true" => INVALID_INTERVAL,
        "state-zero" | "state-false" => INVALID_STATE,
        "state-tilde-user" => OTHER_USER_HOME,
        _ => return None,
    })
}

fn pyjson_of(value: &Value) -> pyjson::Value {
    pyjson::parse(&serde_json::to_string(value).unwrap()).unwrap()
}

/// `Connector(config)`: validation, then the journal at the vectors' clock.
fn connector(
    value: &pyjson::Value,
    allow: bool,
    environment: &Environment,
) -> Result<(config::Config, Option<String>), (String, &'static str)> {
    let config =
        config::validate(value, allow, environment).map_err(|error| (error.0, "ConnectorError"))?;
    let journal = Journal::open(&config.state_dir, &config.binding, EPOCH).map_err(|error| {
        let kind = match error {
            journal::JournalError::Refused(_) => "ConnectorError",
            journal::JournalError::Io(_) => "OSError",
            journal::JournalError::Storage(_) => "sqlite3.Error",
        };
        (error.message().to_owned(), kind)
    })?;
    journal.close();
    let stored = read_only(&config.state_dir.join(journal::JOURNAL))
        .query_row(
            "SELECT value FROM metadata WHERE key='relay_binding'",
            [],
            |row| row.get(0),
        )
        .ok();
    Ok((config, stored))
}

#[test]
fn configuration_outcomes_match_the_python_connector() {
    umask();
    let mut checked = 0;
    for record in records("config.jsonl")
        .into_iter()
        .filter(|record| record.get("argv").is_none())
    {
        let id = record["id"].as_str().unwrap();
        let scratch = Scratch::new(&format!("config-{id}"));
        for name in ["home", "cwd"] {
            setup(
                &scratch.root,
                &serde_json::json!([{"action": "mkdir", "path": name, "mode": "0700"}]),
            );
        }
        if let Some(actions) = record.get("setup") {
            setup(&scratch.root, actions);
        }
        let before = tree(&scratch.root);
        let environment = Environment {
            home: Some(scratch.path("home")),
            cwd: scratch.path("cwd"),
        };
        let parsed = match record.get("config_text") {
            Some(text) => {
                config::parse(&scratch.substitute(text.as_str().unwrap())).map_err(|error| error.0)
            }
            None => {
                let text = scratch.substitute(&serde_json::to_string(&record["config"]).unwrap());
                Ok(pyjson::parse(&text).unwrap())
            }
        };
        let allow = record["allow_insecure_localhost"] == true;
        let outcome = parsed
            .map_err(|error| (error, "parse"))
            .and_then(|value| connector(&value, allow, &environment));
        let expect = &record["expect"];
        let created: serde_json::Map<String, Value> = tree(&scratch.root)
            .as_object()
            .unwrap()
            .iter()
            .filter(|(path, _)| before.get(path.as_str()).is_none())
            .map(|(path, info)| (scratch.portable(&scratch.root.join(path)), info.clone()))
            .collect();
        match (rust_refusal(id), record.get("divergence")) {
            (Some(refusal), Some(_)) => {
                assert_eq!(outcome.as_ref().map(|_| ()).unwrap_err().0, refusal, "{id}");
                assert!(
                    created.is_empty(),
                    "{id}: a refused start created {created:?}"
                );
                checked += 1;
                continue;
            }
            (None, Some(divergence)) => panic!("{id}: unreviewed divergence {divergence}"),
            (Some(_), None) => panic!("{id}: no longer marked as a divergence"),
            (None, None) => {}
        }
        if expect["accepted"] == true {
            let (config, stored) =
                outcome.unwrap_or_else(|error| panic!("{id}: refused with {error:?}"));
            assert_eq!(
                config.server_url,
                expect["server_url"].as_str().unwrap(),
                "{id}"
            );
            assert_eq!(
                config.identity_url,
                expect["identity_url"].as_str().unwrap(),
                "{id}"
            );
            assert_eq!(
                config.hgs_path,
                expect["hgs_path"].as_str().unwrap(),
                "{id}"
            );
            assert_eq!(
                Some(config.poll_interval),
                expect["poll_interval"].as_f64(),
                "{id}"
            );
            assert_eq!(config.binding, expect["binding"].as_str().unwrap(), "{id}");
            assert_eq!(stored.as_deref(), expect["binding"].as_str(), "{id}");
            assert_eq!(
                scratch.portable(&config.state_dir),
                expect["state_dir"].as_str().unwrap(),
                "{id}"
            );
        } else {
            let (message, _) = outcome.as_ref().map(|_| ()).unwrap_err();
            match expect["raised"].as_str().unwrap() {
                "ConnectorError" => assert_eq!(message, expect["error"].as_str().unwrap(), "{id}"),
                "JSONDecodeError" => assert_eq!(message, INVALID_JSON, "{id}"),
                "OSError" => assert_eq!(message, STATE_UNAVAILABLE, "{id}"),
                other => panic!("{id}: unexpected Python exception {other}"),
            }
            // main() logs only the fixed startup line for every one of these.
            assert_eq!(expect["startup_line"], true, "{id}");
        }
        assert_same(&Value::Object(created), &expect["created"], id);
        checked += 1;
    }
    assert_eq!(checked, 50);
}

#[test]
fn relay_urls_and_bindings_match_the_python_connector() {
    umask();
    let identities = document("identities.json");
    for row in identities["checked_url"].as_array().unwrap() {
        let allow = row["allow_insecure_localhost"] == true;
        let outcome = match row["value"].as_str() {
            Some(value) => url::checked_url(value, allow).map_err(|error| error.0),
            // A missing or non-string server_url fails before checked_url's own checks.
            None => Err(INVALID_SERVER_URL.to_owned()),
        };
        match (row.get("result"), row.get("error")) {
            (Some(result), None) => {
                assert_eq!(outcome.as_deref(), Ok(result.as_str().unwrap()), "{row}");
                let transport = url::Transport::new(&url::transport(result.as_str().unwrap()));
                if row.get("divergence").is_some() {
                    assert_eq!(transport.unwrap_err().0, NON_ASCII_HOST, "{row}");
                } else {
                    transport.unwrap_or_else(|error| panic!("{row}: {error}"));
                }
            }
            (None, Some(error)) => {
                assert_eq!(outcome.unwrap_err(), error.as_str().unwrap(), "{row}")
            }
            _ => panic!("{row}"),
        }
    }
    for row in identities["managed_origin"].as_array().unwrap() {
        let value = row["value"].as_str().unwrap();
        assert_eq!(
            url::managed_origin(value),
            row["managed_origin"].as_str(),
            "{row}"
        );
        assert_eq!(
            url::transport(value),
            row["transport"].as_str().unwrap(),
            "{row}"
        );
    }
    for row in identities["identity_url"].as_array().unwrap() {
        let outcome = url::identity_url(
            row["server_url"].as_str().unwrap(),
            row["identity_url"].as_str(),
        );
        match row.get("result") {
            Some(result) => assert_eq!(outcome.unwrap(), result.as_str().unwrap(), "{row}"),
            None => assert_eq!(
                outcome.unwrap_err().0,
                row["error"].as_str().unwrap(),
                "{row}"
            ),
        }
    }
    for row in identities["bindings"].as_array().unwrap() {
        let scratch = Scratch::new("binding");
        let mut config = serde_json::json!({"server_url": row["server_url"], "node_token": row["node_token"],
                                            "state_dir": scratch.path("state").display().to_string()});
        if !row["identity_url"].is_null() {
            config["identity_url"] = row["identity_url"].clone();
        }
        let environment = Environment {
            home: None,
            cwd: scratch.root.clone(),
        };
        let (config, stored) = connector(&pyjson_of(&config), false, &environment).unwrap();
        assert_eq!(
            config.server_url,
            row["transport"].as_str().unwrap(),
            "{row}"
        );
        assert_eq!(
            config.identity_url,
            row["binding_input"].as_str().unwrap(),
            "{row}"
        );
        assert_eq!(config.binding, row["binding"].as_str().unwrap(), "{row}");
        assert_eq!(stored.as_deref(), row["binding"].as_str(), "{row}");
    }
}

#[test]
fn request_hashes_match_python_byte_for_byte() {
    let mut hashed = 0;
    for record in records("request_hash.jsonl") {
        let id = record["id"].as_str().unwrap();
        let raw: Vec<u8> = match record.get("raw_parts") {
            Some(parts) => parts
                .as_array()
                .unwrap()
                .iter()
                .flat_map(|part| {
                    part[0]
                        .as_str()
                        .unwrap()
                        .repeat(part[1].as_u64().unwrap() as usize)
                        .into_bytes()
                })
                .collect(),
            None => record["raw"].as_str().unwrap().as_bytes().to_vec(),
        };
        let expect = &record["expect"];
        // A request inside its claim batch: at least two levels and 64 values more than the relay admits.
        let parsed = request::parse(&raw, 34, 50_064);
        let note = record["note"].as_str().unwrap_or_default();
        if let Some(hash) = expect["request_hash"].as_str() {
            if note.contains("the relay parser cannot produce it") {
                assert!(
                    parsed.is_err(),
                    "{id}: lone surrogates never reach the connector"
                );
                continue;
            }
            let value = parsed.unwrap_or_else(|error| panic!("{id}: {error}"));
            let canonical = zerus_relay::json::canonical(&value).unwrap();
            if let Some(expected) = expect["canonical"].as_str() {
                assert_eq!(canonical, expected, "{id}");
            }
            assert_eq!(
                canonical.len() as u64,
                expect["canonical_bytes"].as_u64().unwrap(),
                "{id}"
            );
            assert_eq!(request::request_hash(&value).unwrap(), hash, "{id}");
            hashed += 1;
            continue;
        }
        match (
            expect["rejected"].as_str().unwrap(),
            expect["error"].as_str().unwrap(),
        ) {
            // Python's json refuses the poll body itself.
            ("parse", _) => assert!(parsed.is_err(), "{id}"),
            // Python reads NaN, Infinity and 1e400 but cannot hash them. The relay
            // never stores NaN or Infinity; an overflowing exponent reaches the hash.
            (_, "invalid native request JSON") => {
                if let Ok(value) = parsed {
                    assert!(request::request_hash(&value).is_err(), "{id}");
                }
            }
            // Envelope validation comes with the request pipeline.
            _ => {}
        }
    }
    assert_eq!(hashed, 56);
}

#[test]
fn journal_operation_sets_match_the_python_connector() {
    let operations = document("operations.json");
    let set = |name: &str| -> Vec<String> {
        let mut items: Vec<String> = operations["sets"][name]
            .as_array()
            .unwrap()
            .iter()
            .map(|item| item.as_str().unwrap().to_owned())
            .collect();
        items.sort();
        items
    };
    let sorted = |items: &[&str]| -> Vec<String> {
        let mut items: Vec<String> = items.iter().map(|item| (*item).to_owned()).collect();
        items.sort();
        items
    };
    let python_reads: Vec<&str> = zerus_relay::protocol::READS
        .iter()
        .copied()
        .filter(|operation| !capabilities::connector_only(operation))
        .collect();
    assert_eq!(sorted(&python_reads), set("READ_OPERATIONS"));
    // The relay's set adds the operations only this connector executes, each
    // journaled by its class: a mutation recovers as uncertain.
    let only = capabilities::CONNECTOR_ONLY;
    let python: Vec<&str> = zerus_relay::protocol::OPERATIONS
        .iter()
        .copied()
        .filter(|operation| !capabilities::connector_only(operation))
        .collect();
    assert_eq!(
        python.len() + only.len(),
        zerus_relay::protocol::OPERATIONS.len()
    );
    assert_eq!(sorted(&python), set("OPERATIONS"));
    let mutations: Vec<&str> = python
        .iter()
        .copied()
        .filter(|operation| journal::is_mutation(operation))
        .collect();
    assert_eq!(sorted(&mutations), set("MUTATIONS"));
    for row in only {
        let mutation = row.class == Class::Mutation;
        assert_eq!(journal::is_mutation(row.operation), mutation, "{row:?}");
        assert_eq!(
            zerus_relay::protocol::READS.contains(&row.operation),
            !mutation,
            "{row:?}"
        );
        if mutation {
            assert!(!journal::READ_CLASS_SQL.contains(row.operation), "{row:?}");
        }
    }
    let sql = &operations["sql"];
    assert_eq!(
        journal::READ_CLASS_SQL,
        sql["READ_CLASS_SQL"].as_str().unwrap()
    );
    assert_eq!(journal::READ_SQL, sql["READ_SQL"].as_str().unwrap());
    assert_eq!(
        journal::SPEECH_READ_SQL,
        sql["speech.READ_SQL"].as_str().unwrap()
    );
    assert_eq!(
        journal::SPEECH_CONFIG_READ_SQL,
        sql["speech_config.READ_SQL"].as_str().unwrap()
    );
    assert_eq!(
        journal::VOICE_READ_SQL,
        sql["voice.READ_SQL"].as_str().unwrap()
    );
    // READ_CLASS_SQL lists exactly the read operations.
    let listed: Vec<String> = journal::READ_CLASS_SQL
        .trim_matches(['(', ')'])
        .split(',')
        .map(|item| item.trim_matches('\'').to_owned())
        .collect();
    assert_eq!(listed, set("READ_OPERATIONS"));
}
