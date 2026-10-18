//! Opt-in, computer-owned analytics. Reads do not collect, create or enable it.
pub(crate) mod adapters;
pub(crate) mod model;
pub(crate) mod native;
mod store;
#[cfg(test)]
mod tests;

use crate::{
    cli::{Error, Result},
    config::Config,
};
use chrono::{Duration as Days, NaiveDate, Utc};
use serde_json::Value;
use std::{
    fs::File,
    io::{self, Read},
    path::Path,
};

fn bad(error: impl ToString) -> Error {
    Error::new(1, error.to_string())
}
pub(crate) fn validate_request(value: &Value) -> Result<()> {
    let object = value
        .as_object()
        .ok_or_else(|| bad("Analytics request must be an object"))?;
    if value["schema"] != 1 {
        return Err(bad("Unsupported analytics schema"));
    }
    let allowed: &[&str] = match value["action"].as_str() {
        Some("summary") => &["schema", "action", "from", "through"],
        Some("configure") => &[
            "schema",
            "action",
            "expected_epoch",
            "enabled",
            "retention_days",
        ],
        Some("purge") => &["schema", "action", "expected_epoch", "confirm"],
        _ => return Err(bad("Unknown analytics action")),
    };
    if object.keys().any(|key| !allowed.contains(&key.as_str())) {
        return Err(bad("Unknown analytics field"));
    }
    if value["action"] == "summary" {
        let parse = |key: &str| -> Result<NaiveDate> {
            let s = value[key]
                .as_str()
                .ok_or_else(|| bad("Analytics needs a UTC date range"))?;
            let date =
                NaiveDate::parse_from_str(s, "%Y-%m-%d").map_err(|_| bad("Invalid UTC date"))?;
            if date.format("%Y-%m-%d").to_string() != s || s.len() != 10 || s.starts_with("0000") {
                return Err(bad("Invalid UTC date"));
            }
            Ok(date)
        };
        let count = (parse("through")? - parse("from")?).num_days();
        if !(0..90).contains(&count) {
            return Err(bad("Analytics range must contain 1 to 90 UTC days"));
        }
    } else {
        let epoch = value["expected_epoch"]
            .as_str()
            .ok_or_else(|| bad("Refresh analytics before changing collection"))?;
        if uuid::Uuid::parse_str(epoch)
            .ok()
            .map(|v| v.to_string())
            .as_deref()
            != Some(epoch)
        {
            return Err(bad("Invalid analytics epoch"));
        }
        if value["action"] == "configure" {
            if !value["enabled"].is_boolean()
                || !value["retention_days"]
                    .as_u64()
                    .is_some_and(|n| (1..=90).contains(&n))
            {
                return Err(bad("Invalid analytics settings"));
            }
        } else if value["confirm"] != true {
            return Err(bad("Purging analytics needs explicit confirmation"));
        }
    }
    Ok(())
}

/// Read the existing computer identity, without invoking Swarm's initializing
/// writer. An unconfigured computer remains explicitly unavailable.
fn machine_id(config: &Config) -> Result<String> {
    let file = File::open(config.dir.join("swarm/catalog.json"))
        .map_err(|_| bad("Computer identity unavailable"))?;
    let mut bytes = Vec::new();
    file.take(16 * 1024 * 1024 + 1).read_to_end(&mut bytes)?;
    if bytes.len() > 16 * 1024 * 1024 {
        return Err(bad("Computer identity catalog exceeds limit"));
    }
    let value: Value =
        serde_json::from_slice(&bytes).map_err(|_| bad("Computer identity unavailable"))?;
    let id = value["node_id"]
        .as_str()
        .ok_or_else(|| bad("Computer identity unavailable"))?;
    let parsed = uuid::Uuid::parse_str(id).map_err(|_| bad("Invalid computer identity"))?;
    if parsed.is_nil() || parsed.to_string() != id {
        return Err(bad("Invalid computer identity"));
    }
    Ok(id.into())
}

pub(crate) fn handle(root: &Path, machine: &str, value: &Value) -> Result<Value> {
    validate_request(value)?;
    let now = Utc::now().timestamp_millis();
    let today = Utc::now().date_naive();
    let default_from = (today - Days::days(29)).to_string();
    let default_through = today.to_string();
    let from = value["from"].as_str().unwrap_or(&default_from);
    let through = value["through"].as_str().unwrap_or(&default_through);
    let action = value["action"].as_str().unwrap();
    let path = root.join("analytics");
    let mut db =
        store::Store::open(&path, action == "configure", action != "summary").map_err(bad)?;
    match action {
        "configure" => db
            .as_mut()
            .unwrap()
            .configure(
                value["expected_epoch"].as_str().unwrap(),
                value["enabled"].as_bool().unwrap(),
                value["retention_days"].as_u64().unwrap(),
                now,
            )
            .map_err(bad)?,
        "purge" => {
            if let Some(db) = db.as_mut() {
                db.purge(value["expected_epoch"].as_str().unwrap(), now)
                    .map_err(bad)?;
            } else if value["expected_epoch"] != store::NIL_EPOCH {
                return Err(bad("Analytics history changed; refresh first"));
            }
        }
        _ => {}
    }
    let result = match db {
        Some(db) => db.summary(machine, from, through, now).map_err(bad)?,
        None => store::empty(machine, from, through, now),
    };
    if result.to_string().len() >= 1024 * 1024 {
        return Err(bad(
            "Analytics summary exceeds 1 MiB; choose a shorter date range",
        ));
    }
    Ok(result)
}

pub(crate) fn dispatch(config: &Config, args: &[String], dry: bool) -> Result<i32> {
    let read_only = args == ["--json", "--read-only"];
    if dry || (args != ["--json"] && !read_only) {
        return Err(bad("usage: hgs [@host] analytics --json"));
    }
    let mut bytes = Vec::new();
    io::stdin().take(4097).read_to_end(&mut bytes)?;
    if bytes.len() > 4096 {
        return Err(bad("Analytics request exceeds 4 KiB"));
    }
    let value: Value = serde_json::from_slice(&bytes).map_err(|_| bad("Invalid analytics JSON"))?;
    validate_request(&value)?;
    if read_only && value["action"] != "summary" {
        return Err(bad("Remote analytics only permits summary reads"));
    }
    let machine = machine_id(config)?;
    println!("{}", handle(&config.state, &machine, &value)?);
    Ok(0)
}

/// Called only from native event/receipt producers. A dashboard never calls it.
/// Analytics failure must not alter native input or lifecycle outcomes.
pub(crate) fn observe(root: &Path, event: &model::Event) {
    if let Ok(Some(mut db)) = store::Store::open_observer(&root.join("analytics")) {
        let _ = db.record(event, Utc::now().timestamp_millis());
    }
}
pub(crate) fn gap(root: &Path, gap: model::Gap) {
    if let Ok(Some(mut db)) = store::Store::open_observer(&root.join("analytics")) {
        let _ = db.note_gap(gap, Utc::now().timestamp_millis());
    }
}

/// The existing recovery worker calls this bounded maintenance step even when
/// automatic recovery is disabled. Summary reads never perform housekeeping.
pub(crate) fn maintenance(root: &Path) {
    if let Ok(Some(mut db)) = store::Store::open_observer(&root.join("analytics")) {
        let _ = db.maintain(Utc::now().timestamp_millis());
    }
}
