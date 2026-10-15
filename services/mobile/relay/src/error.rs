// The leading `::` names the crate, not this library's server `http` module.
use ::http::StatusCode;
#[cfg(feature = "server")]
use axum::{
    http::header,
    response::{IntoResponse, Response},
    Json,
};
#[cfg(feature = "server")]
use serde_json::json;

/// Deliberately contains no driver diagnostics, request values or credentials.
#[derive(Debug, Clone, Copy)]
pub struct Error(pub StatusCode, pub &'static str);
pub type Result<T> = std::result::Result<T, Error>;
impl Error {
    pub const BAD: Self = Self(StatusCode::BAD_REQUEST, "invalid request");
    pub const BUSY: Self = Self(StatusCode::TOO_MANY_REQUESTS, "relay capacity is busy");
    pub const UNAVAILABLE: Self = Self(
        StatusCode::SERVICE_UNAVAILABLE,
        "relay temporarily unavailable",
    );
    pub const UNAUTHORIZED: Self = Self(StatusCode::UNAUTHORIZED, "credential not authorized");
    pub const NOT_FOUND: Self = Self(StatusCode::NOT_FOUND, "not found");
    pub const CONFLICT: Self = Self(StatusCode::CONFLICT, "request identity or route conflicts");
    pub const LARGE: Self = Self(StatusCode::PAYLOAD_TOO_LARGE, "payload exceeds limit");
    pub const TIMEOUT: Self = Self(StatusCode::REQUEST_TIMEOUT, "request timed out");
}
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.1)
    }
}
impl std::error::Error for Error {}
#[cfg(feature = "server")]
impl From<sqlx::Error> for Error {
    fn from(_: sqlx::Error) -> Self {
        Self::UNAVAILABLE
    }
}
impl From<serde_json::Error> for Error {
    fn from(_: serde_json::Error) -> Self {
        Self::BAD
    }
}
impl From<std::io::Error> for Error {
    fn from(_: std::io::Error) -> Self {
        Self::UNAVAILABLE
    }
}
#[cfg(feature = "server")]
impl IntoResponse for Error {
    fn into_response(self) -> Response {
        let mut r = (self.0, Json(json!({"error":self.1}))).into_response();
        if self.0 == StatusCode::TOO_MANY_REQUESTS || self.0 == StatusCode::SERVICE_UNAVAILABLE {
            r.headers_mut()
                .insert(header::RETRY_AFTER, "1".parse().unwrap());
        }
        r
    }
}
