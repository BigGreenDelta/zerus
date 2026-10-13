//! The Windows session host (spec `docs/superpowers/specs/2026-10-13-windows-native-host-design.md`,
//! D1-D6, D9-D11, D13, D14): `hgs.exe __host serve` owns the ConPTY sessions of
//! one user and state directory and serves the CLI, attach clients and
//! supervisors over a private named pipe.
//!
//! Plain Rust that compiles and runs its tests on every platform: `protocol`
//! (framing and the protocol 1 vocabulary), `endpoint` (pipe name, secret and
//! host lock), `staging` (explicit enablement and the staged CLI and host
//! images, D2), the argument handling of `admin` (`hgs session-host`), the
//! portable half of `spawn` (environment blocks and command lines), `keys`
//! (tmux's key names and bytes), `sequences` (query and reply filtering), `link`
//! (connections as frames), `session` (a session's screen model, pumps and
//! attached clients), `registry` (sessions and requests), `channel` (the
//! supervisor channel and input evidence), `server`'s connection protocol,
//! `client`'s connection, `worker` (D14) and `log`.
//! Windows only: `pipe`, `token`, `conpty`, `job`, `console` (sessions on
//! ConPTY), `server::serve`, `client::{connect, connect_or_start}` and
//! `command` (`hgs.exe __host`).
// On Unix nothing outside the tests uses the host; the Windows lifecycle (M4)
// and attach client (M5) use more of it than the host process does today.
#![cfg_attr(not(all(windows, test)), allow(dead_code))]

pub(crate) mod admin;
mod channel;
mod client;
#[cfg(windows)]
mod command;
#[cfg(windows)]
mod conpty;
#[cfg(windows)]
mod console;
pub(crate) mod endpoint;
#[cfg(windows)]
pub(crate) mod job;
pub(crate) mod keys;
mod link;
mod log;
#[cfg(windows)]
pub(crate) mod pipe;
pub(crate) mod protocol;
mod registry;
pub(crate) mod sequences;
mod server;
mod session;
pub(crate) mod spawn;
pub(crate) mod staging;
#[cfg(test)]
mod tests;
#[cfg(windows)]
pub(crate) mod token;
mod worker;

#[cfg(windows)]
pub(crate) use self::command::run;
