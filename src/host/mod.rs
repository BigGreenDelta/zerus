//! The Windows session host (spec `docs/superpowers/specs/2026-10-13-windows-native-host-design.md`,
//! D1-D6 and D9): `hgs.exe __host serve` owns the ConPTY sessions of one user and
//! state directory and serves the CLI, attach clients and supervisors over a
//! private named pipe.
//!
//! `protocol` (framing and the protocol 1 vocabulary) and `endpoint` (pipe name,
//! secret and host lock) are plain Rust and compile and run their tests on every
//! platform; `pipe` and `token` wrap the Windows APIs.
// The host server, the backend client and the supervisor channel wire these
// modules in during M3; until then only their tests use them.
#![cfg_attr(not(test), allow(dead_code))]

pub(crate) mod endpoint;
#[cfg(windows)]
pub(crate) mod pipe;
pub(crate) mod protocol;
#[cfg(windows)]
pub(crate) mod token;
