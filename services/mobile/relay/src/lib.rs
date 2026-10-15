//! The API v1 codec, envelope validation and error type always build. The
//! relay server modules need the default `server` feature.
#[cfg(feature = "server")]
pub mod alerts;
#[cfg(feature = "server")]
pub mod config;
#[cfg(feature = "server")]
pub mod db;
pub mod error;
#[cfg(feature = "server")]
pub mod http;
pub mod json;
pub mod protocol;
#[cfg(feature = "server")]
pub mod push;
#[cfg(feature = "server")]
pub mod registry;
mod speech_config;
#[cfg(feature = "server")]
pub mod store;
