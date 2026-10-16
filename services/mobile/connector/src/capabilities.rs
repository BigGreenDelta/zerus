//! Native capabilities from `hgs --help` (`connector.py:498-544`): Python's
//! frozen help-marker table, its operation and feature formula
//! (`connector.py:490-494`), and the operations this build implements.
//!
//! An operation is advertised only when the native CLI offers it, as in
//! Python, and this connector implements it; a later slice adds to
//! [`IMPLEMENTED`]. Python resets every flag before probing, so a failed
//! probe disables everything for 300 s; here a probe that fails keeps the
//! previous flags (divergence a).
use serde_json::{json, Map, Value};
use std::collections::BTreeSet;
use zerus_relay::protocol::OPERATIONS;

/// The native help text is read again this often (seconds).
pub const REPROBE_SECONDS: f64 = 300.0;
/// `hgs --help` budget (seconds).
pub const PROBE_TIMEOUT: f64 = 3.0;

/// Operations this connector executes, with the session mutations of
/// [`crate::mutations::OPERATIONS`] and the creations of
/// [`crate::launch::OPERATIONS`]. Every other known operation is refused
/// as unsupported and never advertised.
pub const IMPLEMENTED: &[&str] = &[
    "archive_list",
    "attachment_read",
    "catalog",
    "dirs",
    "file_read",
    "history",
    "inspect",
    "mark_read",
    "process_output",
    "speech_cancel",
    "speech_config",
    "speech_config_status",
    "speech_profile_remove",
    "speech_profile_test",
    "speech_profile_upsert",
    "speech_profiles",
    "speech_stats",
    "speech_status",
    "speech_submit",
    "terminal_input",
    "terminal_snapshot",
    "voice_cancel",
    "voice_profiles",
    "voice_status",
    "voice_submit",
    "worktrees",
];

pub fn implemented(operation: &str) -> bool {
    IMPLEMENTED.contains(&operation)
        || crate::mutations::handles(operation)
        || crate::launch::handles(operation)
}

/// Python's `FEATURES | {"accounts_snapshot"}` and the conditional features,
/// each with the operation it needs.
const FEATURE_OPERATIONS: &[(&str, &str)] = &[
    ("voice_video", "voice_submit"),
    ("accounts_snapshot", "inspect"),
    ("inspect_after", "inspect"),
    ("inspect_agent", "inspect"),
    ("send_agent", "send"),
    ("directory_create_v1", "directory_create"),
    ("launch_project", "launch"),
    ("gateway_one_hop", "send"),
];

pub const LIFECYCLE: &[&str] = &[
    "archive",
    "forget",
    "fork",
    "pause",
    "rename",
    "restore",
    "resume",
    "terminate",
];
pub const TERMINAL: &[&str] = &["terminal_input", "terminal_snapshot"];
pub const LAUNCH: &[&str] = &[
    "catalog",
    "dirs",
    "directory_create",
    "launch",
    "worktree_create",
    "worktrees",
];
pub const WORKTREES: &[&str] = &["worktree_create", "worktrees"];
pub const SPEECH: &[&str] = &[
    "speech_cancel",
    "speech_profiles",
    "speech_stats",
    "speech_status",
    "speech_submit",
];
pub const VOICE: &[&str] = &[
    "voice_cancel",
    "voice_profiles",
    "voice_status",
    "voice_submit",
];
pub const SPEECH_CONFIG: &[&str] = &[
    "speech_config",
    "speech_config_status",
    "speech_profile_remove",
    "speech_profile_test",
    "speech_profile_upsert",
];

/// What the native CLI offers, from its help text.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Flags {
    pub archive: bool,
    pub lifecycle: bool,
    pub terminal: bool,
    pub recovery: bool,
    pub history: bool,
    pub launch: bool,
    pub project_launch: bool,
    pub worktree: bool,
    pub directory_create: bool,
    pub read: bool,
    pub preference: bool,
    pub attachment_read: bool,
    pub file_read: bool,
    pub speech: bool,
    pub voice: bool,
    pub voice_video: bool,
    pub speech_config: bool,
    /// Both peer markers (`fleet.supported`).
    pub peers: bool,
}

impl Flags {
    /// The frozen help markers (`operations.json` `capabilities.markers`).
    pub fn from_help(help: &str) -> Flags {
        let has = |marker: &str| help.contains(marker);
        Flags {
            archive: has("archives --json"),
            lifecycle: help
                .lines()
                .any(|line| line.contains("session-action <session> --json")),
            terminal: has("terminal <session> --json"),
            recovery: has("recovery action --scoped-json"),
            history: has("history <session> --json"),
            launch: has("--launch-id"),
            project_launch: has("swarm assign-launch --json"),
            worktree: has("Mobile worktree ABI: worktrees-v1"),
            directory_create: has("dirs --create <path>"),
            read: has("read <session> --json"),
            preference: has("swarm preference set mobile_delivery"),
            attachment_read: has("attachment <session> --request ID --index N"),
            file_read: has("file-read <session> --json"),
            speech: has("speech --json"),
            voice: has("voice <session> --json"),
            voice_video: has("voice-video-v1"),
            speech_config: has("Speech profile management v1"),
            peers: has("mobile-peers --json") && has("mobile-peer --json"),
        }
    }

    /// Python's advertised operations, before this build's own filter.
    /// `fleet` is whether a fleet exists (always in the connector).
    pub fn python_operations(&self, fleet: bool) -> BTreeSet<&'static str> {
        let mut removed: BTreeSet<&str> = BTreeSet::new();
        let mut remove = |gate: bool, operations: &[&'static str]| {
            if !gate {
                removed.extend(operations.iter().copied());
            }
        };
        remove(self.worktree, WORKTREES);
        remove(self.archive, &["archive_list"]);
        remove(self.directory_create, &["directory_create"]);
        remove(self.lifecycle, LIFECYCLE);
        remove(self.terminal, TERMINAL);
        let launch: Vec<&'static str> = LAUNCH
            .iter()
            .copied()
            .filter(|operation| *operation != "directory_create")
            .collect();
        remove(self.launch, &launch);
        remove(self.history, &["history"]);
        remove(self.recovery, &["recovery_action"]);
        remove(self.read, &["mark_read"]);
        remove(self.attachment_read, &["attachment_read"]);
        remove(self.file_read, &["file_read"]);
        remove(self.preference && fleet, &["set_mobile_delivery"]);
        remove(self.speech, SPEECH);
        remove(self.voice, VOICE);
        remove(self.speech_config, SPEECH_CONFIG);
        OPERATIONS
            .iter()
            .copied()
            .filter(|operation| !removed.contains(operation))
            .collect()
    }

    /// Python's features. `one_hop` is `fleet.available`.
    pub fn python_features(&self, one_hop: bool) -> BTreeSet<&'static str> {
        let mut features: BTreeSet<&str> = [
            "accounts_snapshot",
            "inspect_after",
            "inspect_agent",
            "send_agent",
        ]
        .into();
        if self.voice && self.voice_video {
            features.insert("voice_video");
        }
        if self.directory_create {
            features.insert("directory_create_v1");
        }
        if one_hop {
            features.insert("gateway_one_hop");
        }
        if self.project_launch {
            features.insert("launch_project");
        }
        features
    }

    /// `mobile_capabilities` of the main connector, which has a fleet.
    pub fn advertised(&self, one_hop: bool) -> Value {
        self.advertised_for(true, one_hop)
    }

    /// `mobile_capabilities` as this build advertises it: Python's sets
    /// limited to implemented operations and the features they carry. A
    /// peer context has no fleet.
    pub fn advertised_for(&self, fleet: bool, one_hop: bool) -> Value {
        let operations: Vec<&str> = self
            .python_operations(fleet)
            .into_iter()
            .filter(|operation| implemented(operation))
            .collect();
        let features: Vec<&str> = self
            .python_features(one_hop)
            .into_iter()
            .filter(|feature| {
                FEATURE_OPERATIONS
                    .iter()
                    .find(|(name, _)| name == feature)
                    .is_some_and(|(_, operation)| operations.contains(operation))
            })
            .collect();
        let mut reasons = Map::new();
        if !self.lifecycle {
            reasons.insert(
                "session_actions".into(),
                json!("Native CLI lacks the scoped session-action ABI"),
            );
        }
        if !self.terminal {
            reasons.insert(
                "terminal".into(),
                json!("Native CLI lacks the scoped Terminal ABI"),
            );
        }
        json!({"protocol_version": 1, "operations": operations, "features": features,
               "reasons": reasons})
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn substring_markers_follow_python() {
        // "file-read <session> --json" holds "read <session> --json".
        let flags = Flags::from_help("hgs file-read <session> --json\n");
        assert!(flags.file_read && flags.read);
        assert!(!Flags::from_help("hgs swarm mobile-peers --json").peers);
        assert!(Flags::from_help("x session-action <session> --json y").lifecycle);
    }

    #[test]
    fn only_implemented_operations_and_their_features_are_advertised() {
        let all = Flags::from_help(
            "session-action <session> --json\nterminal <session> --json\nhistory <session> --json\n--launch-id\nMobile worktree ABI: worktrees-v1\ndirs --create <path>\nread <session> --json\n",
        );
        let advertised = all.advertised(false);
        assert_eq!(
            advertised["operations"],
            json!([
                "answer",
                "archive",
                "catalog",
                "clear_context",
                "compact_context",
                "directory_create",
                "dirs",
                "forget",
                "fork",
                "history",
                "inspect",
                "interrupt",
                "launch",
                "mark_read",
                "pause",
                "process_output",
                "process_stop",
                "rename",
                "restore",
                "resume",
                "send",
                "send_now",
                "settings",
                "terminal_input",
                "terminal_snapshot",
                "terminate",
                "worktree_create",
                "worktrees"
            ])
        );
        assert_eq!(
            advertised["features"],
            json!([
                "accounts_snapshot",
                "directory_create_v1",
                "inspect_after",
                "inspect_agent",
                "send_agent"
            ])
        );
        assert_eq!(advertised["reasons"], json!({}));
        let none = Flags::default().advertised(false);
        assert_eq!(
            none["operations"],
            json!([
                "answer",
                "clear_context",
                "compact_context",
                "inspect",
                "interrupt",
                "process_output",
                "process_stop",
                "send",
                "send_now",
                "settings"
            ])
        );
        assert_eq!(none["reasons"].as_object().unwrap().len(), 2);
    }
}
