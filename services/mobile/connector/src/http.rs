//! The relay client, as `Connector.http` uses aiohttp
//! (`connector.py:1237-1257`) with the request rules the relay and its nginx
//! impose: HTTP/1.1 only, no proxy (aiohttp ran with `trust_env=False`), no
//! redirects, `Content-Type: application/json` and a `Content-Length` on every
//! POST, no `Content-Encoding`, 35 s per call, and capped responses. The
//! bearer token goes only to the configured transport origin.
//!
//! TLS is rustls with ring and the system roots: `SSL_CERT_FILE` and
//! `SSL_CERT_DIR` when set, otherwise the platform store (the Keychain on
//! macOS). Without a single usable root every HTTPS call fails as an
//! unavailable relay; nothing falls back to an unverified connection.
use crate::request;
use crate::url::Transport;
use bytes::Bytes;
use http::header::{ACCEPT, AUTHORIZATION, CONTENT_TYPE, HOST, USER_AGENT};
use http::{Method, Request, StatusCode, Uri};
use http_body_util::{BodyExt, Full};
use hyper_util::client::legacy::connect::HttpConnector;
use hyper_util::client::legacy::Client;
use hyper_util::rt::TokioExecutor;
use serde_json::Value;
use std::sync::Arc;
use std::time::Duration;

/// aiohttp's `ClientTimeout(total=35)`: connection, request and response.
pub const CALL_TIMEOUT: Duration = Duration::from_secs(35);
/// `MAX_BYTES`, the default response cap.
pub const MAX_RESPONSE: usize = 8 * 1024 * 1024;
/// A poll may carry the largest request plus its batch envelope.
pub const MAX_POLL: usize = 30_408_704 + 4096;
/// A claim batch nests a request two levels deeper than the relay admits it
/// (depth 32, 50,000 values) and adds its envelope and route.
pub const BATCH_DEPTH: usize = 34;
pub const BATCH_VALUES: usize = 50_064;
/// Larger bodies are parsed on a blocking thread, as Python's `to_thread`.
const OFF_THREAD: usize = 1024 * 1024;

pub const FAILED: &str = "relay request failed";
pub const UNAVAILABLE: &str = "relay is unavailable";
pub const TOO_LARGE: &str = "relay response exceeds size limit";
pub const INVALID_JSON: &str = "relay returned invalid JSON";

/// A failed relay call: Python's RelayError (with its status) or one of the
/// fixed ConnectorError texts above.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum RelayError {
    Status(u16),
    Failed(&'static str),
}

impl RelayError {
    pub fn message(&self) -> &'static str {
        match self {
            RelayError::Status(_) => FAILED,
            RelayError::Failed(text) => text,
        }
    }
    pub fn status(&self) -> Option<u16> {
        match self {
            RelayError::Status(status) => Some(*status),
            RelayError::Failed(_) => None,
        }
    }
}

type Body = Full<Bytes>;

enum Transport_ {
    Plain(Client<HttpConnector, Body>),
    Tls(Client<hyper_rustls::HttpsConnector<HttpConnector>, Body>),
    /// HTTPS without any trusted root: every call fails closed.
    NoRoots,
}

pub struct Relay {
    transport: Transport_,
    /// `scheme://host[:port]` of the transport origin.
    origin: String,
    host: String,
    /// The configured path prefix, without a trailing slash.
    prefix: String,
    authorization: String,
}

/// The system roots, or `None` when not one certificate could be loaded.
pub fn system_roots() -> Option<rustls::RootCertStore> {
    let loaded = rustls_native_certs::load_native_certs();
    let mut roots = rustls::RootCertStore::empty();
    let (added, _) = roots.add_parsable_certificates(loaded.certs);
    (added > 0).then_some(roots)
}

fn connector() -> HttpConnector {
    let mut connector = HttpConnector::new();
    connector.set_nodelay(true);
    connector
}

fn client<C>(connector: C) -> Client<C, Body>
where
    C: hyper_util::client::legacy::connect::Connect + Clone + Send + Sync + 'static,
{
    Client::builder(TokioExecutor::new())
        .pool_idle_timeout(Duration::from_secs(90))
        .build(connector)
}

/// Bytes the request target may carry as they are; everything else is
/// percent-encoded, as yarl requotes the URL aiohttp sends.
fn target_byte(byte: u8) -> bool {
    byte.is_ascii_graphic() && !b"\"<>\\^`{|}".contains(&byte)
}

/// The request target for `server_url + path`, as yarl splits that text: a
/// fragment is never sent and an empty path is `/`.
pub fn target(prefix: &str, path: &str) -> String {
    let joined = format!("{prefix}{path}");
    let joined = joined.split('#').next().unwrap_or_default();
    let mut out = String::new();
    if !joined.starts_with('/') {
        out.push('/');
    }
    for byte in joined.bytes() {
        if target_byte(byte) {
            out.push(byte as char);
        } else {
            out.push_str(&format!("%{byte:02X}"));
        }
    }
    out
}

impl Relay {
    /// `roots` is consulted only for an HTTPS transport.
    pub fn new(transport: &Transport, token: &str, roots: Option<rustls::RootCertStore>) -> Relay {
        let host = if transport.host.contains(':') {
            format!("[{}]", transport.host)
        } else {
            transport.host.clone()
        };
        let default = if transport.secure { 443 } else { 80 };
        let host = if transport.port == default {
            host
        } else {
            format!("{host}:{}", transport.port)
        };
        let scheme = if transport.secure { "https" } else { "http" };
        let client = if !transport.secure {
            Transport_::Plain(client(connector()))
        } else if let Some(roots) = roots {
            // HTTP/1.1 only, so no ALPN is offered (hyper-rustls requires it unset).
            let config = rustls::ClientConfig::builder_with_provider(Arc::new(
                rustls::crypto::ring::default_provider(),
            ))
            .with_safe_default_protocol_versions()
            .expect("ring supports the default protocol versions")
            .with_root_certificates(roots)
            .with_no_client_auth();
            let mut http = connector();
            http.enforce_http(false);
            let https = hyper_rustls::HttpsConnectorBuilder::new()
                .with_tls_config(config)
                .https_only()
                .enable_http1()
                .wrap_connector(http);
            Transport_::Tls(client(https))
        } else {
            Transport_::NoRoots
        };
        Relay {
            transport: client,
            origin: format!("{scheme}://{host}"),
            host,
            prefix: transport.prefix.clone(),
            authorization: format!("Bearer {token}"),
        }
    }

    /// `true` when HTTPS has no trusted root, so every call will fail.
    pub fn without_roots(&self) -> bool {
        matches!(self.transport, Transport_::NoRoots)
    }

    /// One call: the parsed JSON body of a 2xx answer (`None` when empty).
    pub async fn call(
        &self,
        method: Method,
        path: &str,
        body: Option<Vec<u8>>,
        max_response: usize,
    ) -> Result<Option<Value>, RelayError> {
        let raw = match tokio::time::timeout(
            CALL_TIMEOUT,
            self.exchange(method, path, body, max_response),
        )
        .await
        {
            Ok(result) => result?,
            Err(_) => return Err(RelayError::Failed(UNAVAILABLE)),
        };
        if raw.is_empty() {
            return Ok(None);
        }
        let large = raw.len() > OFF_THREAD;
        let parse = move || {
            request::parse(&raw, BATCH_DEPTH, BATCH_VALUES)
                .map(Some)
                .map_err(|_| RelayError::Failed(INVALID_JSON))
        };
        if large {
            tokio::task::spawn_blocking(parse)
                .await
                .unwrap_or(Err(RelayError::Failed(INVALID_JSON)))
        } else {
            parse()
        }
    }

    async fn exchange(
        &self,
        method: Method,
        path: &str,
        body: Option<Vec<u8>>,
        max_response: usize,
    ) -> Result<Bytes, RelayError> {
        let unavailable = |_| RelayError::Failed(UNAVAILABLE);
        let uri: Uri = format!("{}{}", self.origin, target(&self.prefix, path))
            .parse()
            .map_err(|_| RelayError::Failed(UNAVAILABLE))?;
        let mut builder = Request::builder()
            .method(method)
            .uri(uri)
            .header(HOST, &self.host)
            .header(AUTHORIZATION, &self.authorization)
            .header(ACCEPT, "application/json")
            .header(
                USER_AGENT,
                concat!("zerus-mobile-connector/", env!("CARGO_PKG_VERSION")),
            );
        if body.is_some() {
            // A Full body has an exact size, so hyper sends Content-Length.
            builder = builder.header(CONTENT_TYPE, "application/json");
        }
        let request = builder
            .body(Full::new(Bytes::from(body.unwrap_or_default())))
            .map_err(|_| RelayError::Failed(UNAVAILABLE))?;
        let response = match &self.transport {
            Transport_::Plain(client) => client.request(request).await.map_err(unavailable)?,
            Transport_::Tls(client) => client.request(request).await.map_err(unavailable)?,
            Transport_::NoRoots => return Err(RelayError::Failed(UNAVAILABLE)),
        };
        let status = response.status();
        if !(StatusCode::OK.as_u16()..300).contains(&status.as_u16()) {
            return Err(RelayError::Status(status.as_u16()));
        }
        let mut body = response.into_body();
        let mut data = Vec::new();
        while let Some(frame) = body.frame().await {
            let frame = frame.map_err(|_| RelayError::Failed(UNAVAILABLE))?;
            if let Ok(chunk) = frame.into_data() {
                data.extend_from_slice(&chunk);
                if data.len() > max_response {
                    return Err(RelayError::Failed(TOO_LARGE));
                }
            }
        }
        Ok(Bytes::from(data))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn targets_follow_the_text_aiohttp_sends() {
        assert_eq!(target("", "/v1/node/heartbeat"), "/v1/node/heartbeat");
        assert_eq!(
            target("/base", "/v1/node/requests?wait=1"),
            "/base/v1/node/requests?wait=1"
        );
        assert_eq!(target("/caf\u{e9}", "/v1"), "/caf%C3%A9/v1");
        assert_eq!(target("#", "/v1/node/heartbeat"), "/");
        assert_eq!(target("?", "/v1/x"), "/?/v1/x");
    }

    #[tokio::test]
    async fn https_without_roots_fails_closed() {
        let transport = Transport::new("https://relay.example.invalid").unwrap();
        let relay = Relay::new(&transport, "fixture-token", None);
        assert!(relay.without_roots());
        assert_eq!(
            relay
                .call(Method::GET, "/v1/node/requests", None, MAX_RESPONSE)
                .await,
            Err(RelayError::Failed(UNAVAILABLE))
        );
    }
}
