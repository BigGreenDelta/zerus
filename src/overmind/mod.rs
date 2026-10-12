//! Overmind: tasks whose content is a ready prompt, moved through a gated,
//! data-driven workflow. See docs/superpowers/specs/2026-10-11-overmind-core-design.md.
mod agent;
mod blobs;
mod cli;
mod commands;
mod deliver;
mod fold;
mod identity;
mod model;
mod rank;
mod refs;
mod remote;
mod sessions;
mod store;
mod sync;
mod templates;
mod workflow;

pub use cli::dispatch;
pub use sessions::{session_ended, session_renamed};

pub use sync::FEATURE;

fn open(config: &crate::config::Config) -> anyhow::Result<store::Store> {
    store::Store::open(&config.state.join("overmind"), &machine(config)?)
}

/// This machine's identity, read without the swarm catalog lock: commands and
/// polls never wait for a swarm sync. Only the first use creates and saves it.
fn machine(config: &crate::config::Config) -> anyhow::Result<String> {
    match crate::swarm::saved_node_id(config) {
        Some(machine) => Ok(machine),
        None => crate::swarm::local_node_id(config),
    }
}

/// How a machine answers an Overmind request or delivery it does not know. A
/// caller that meets it reports an outdated host, so the text never changes.
const UNKNOWN: &str = "this machine does not know the Overmind";

/// Serves an Overmind request that arrived over the swarm channel.
pub fn serve(config: &crate::config::Config, action: &str, request: serde_json::Value) -> anyhow::Result<serde_json::Value> {
    match action {
        "overmind-exchange" => sync::serve(&mut open(config)?, request),
        "overmind-blob" => remote::serve_blob(&open(config)?, &request),
        "overmind-blobs" => remote::serve_blobs(&open(config)?, &request),
        "overmind-deliver" => remote::serve_deliver(&mut open(config)?, &request),
        _ => anyhow::bail!("{UNKNOWN} request {action}; update HGS on it"),
    }
}

/// One exchange with a swarm peer; the outcome is kept for `hgs overmind status`.
pub fn replicate(
    config: &crate::config::Config,
    alias: &str,
    node: &str,
    call: &mut dyn FnMut(&serde_json::Value) -> anyhow::Result<serde_json::Value>,
) -> anyhow::Result<()> {
    sync::exchange(&mut open(config)?, node, alias, call).map(|_| ())
}

/// Changes with every committed Overmind write, so the worker can exchange at once.
pub fn revision(config: &crate::config::Config) -> String {
    std::fs::read_to_string(config.state.join("overmind/rev")).unwrap_or_default()
}
