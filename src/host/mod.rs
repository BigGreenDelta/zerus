//! The Windows session host (Phase 2, `hgs.exe __host serve`): the process that
//! owns native sessions the way a tmux server owns its panes. See
//! docs/superpowers/specs/2026-10-13-windows-native-host-design.md.
// The host server wires these modules in M3; until then only their tests use them.
#![cfg_attr(not(test), allow(dead_code))]

#[cfg(windows)]
mod conpty;
#[cfg(windows)]
mod job;
pub(crate) mod spawn;
