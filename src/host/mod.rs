//! The Windows session host (spec `docs/superpowers/specs/2026-10-13-windows-native-host-design.md`,
//! D1-D6 and D9): `hgs.exe __host serve` owns the ConPTY sessions of one user and
//! state directory and serves the CLI, attach clients and supervisors over a
//! private named pipe.
//!
//! `protocol` (framing and the protocol 1 vocabulary), `endpoint` (pipe name,
//! secret and host lock), `staging` (explicit enablement and the staged CLI and
//! host images, D2), the argument handling of `admin` (`hgs session-host`) and
//! the portable half of `spawn` (environment blocks and command lines) are plain
//! Rust and compile and run their tests on every platform; `pipe`, `token`,
//! `conpty` and `job` wrap the Windows APIs.
// The host server, the backend client and the supervisor channel wire these
// modules in during M3; until then only their tests and `hgs session-host` use
// them.
#![cfg_attr(not(test), allow(dead_code))]

pub(crate) mod admin;
#[cfg(windows)]
mod conpty;
pub(crate) mod endpoint;
#[cfg(windows)]
pub(crate) mod job;
#[cfg(windows)]
pub(crate) mod pipe;
pub(crate) mod protocol;
pub(crate) mod spawn;
pub(crate) mod staging;
#[cfg(windows)]
pub(crate) mod token;
