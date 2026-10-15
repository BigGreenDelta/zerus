//! Relay URL rules: Python's `checked_url`, the managed relay migration of
//! `managed_relay.py`, and the transport origin the HTTP client connects to.
//!
//! Acceptance follows Python's `urllib.parse.urlsplit` and its `hostname`,
//! `port`, `username` and `password` properties exactly, because a URL Python
//! accepts must keep working after a cutover. No URL or IDNA library is used:
//! a non-ASCII transport host is refused instead (README divergence i), so
//! Python's NFKC check of non-ASCII hosts has no counterpart here.
use crate::error::{
    ConnectorError, Result, FOREIGN_IDENTITY, INSECURE_SERVER_URL, INVALID_SERVER_URL,
    NON_ASCII_HOST,
};
use std::net::{Ipv4Addr, Ipv6Addr};

/// Python's ValueError from `urlsplit` or the `port` property.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ValueError;

pub const CURRENT: &str = "https://relay.zerus.dev";
pub const LEGACY: &str = "https://zerus.dev.guthub.dev";

/// The parts of `urlsplit(value)` the connector reads.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Split<'a> {
    pub scheme: String,
    pub netloc: &'a str,
    pub path: &'a str,
    pub query: &'a str,
    pub fragment: &'a str,
}

/// `urlsplit(value)` for a value without whitespace or control characters,
/// which `checked_url` refuses first. `Err` stands for Python's ValueError.
pub fn urlsplit(value: &str) -> std::result::Result<Split<'_>, ValueError> {
    let mut scheme = String::new();
    let mut rest = value;
    if let Some(colon) = value.find(':') {
        let candidate = &value[..colon];
        if candidate.starts_with(|c: char| c.is_ascii_alphabetic())
            && candidate
                .chars()
                .all(|c| c.is_ascii_alphanumeric() || "+-.".contains(c))
        {
            scheme = candidate.to_ascii_lowercase();
            rest = &value[colon + 1..];
        }
    }
    let mut netloc = "";
    if let Some(after) = rest.strip_prefix("//") {
        let end = after.find(['/', '?', '#']).unwrap_or(after.len());
        netloc = &after[..end];
        rest = &after[end..];
        if netloc.contains('[') != netloc.contains(']') {
            return Err(ValueError);
        }
        if netloc.contains('[') {
            check_bracketed_netloc(netloc)?;
        }
    }
    let (rest, fragment) = rest.split_once('#').unwrap_or((rest, ""));
    let (path, query) = rest.split_once('?').unwrap_or((rest, ""));
    Ok(Split {
        scheme,
        netloc,
        path,
        query,
        fragment,
    })
}

fn check_bracketed_netloc(netloc: &str) -> std::result::Result<(), ValueError> {
    let host_and_port = netloc.rsplit_once('@').map_or(netloc, |(_, host)| host);
    let host = match host_and_port.split_once('[') {
        Some((before, bracketed)) => {
            if !before.is_empty() {
                return Err(ValueError);
            }
            let (host, port) = bracketed.split_once(']').unwrap_or((bracketed, ""));
            if !port.is_empty() && !port.starts_with(':') {
                return Err(ValueError);
            }
            host
        }
        None => host_and_port
            .split_once(':')
            .map_or(host_and_port, |(host, _)| host),
    };
    // `_check_bracketed_host`: IPvFuture, or an IPv6 (never IPv4) address.
    if let Some(future) = host.strip_prefix(['v', 'V']) {
        let (version, address) = future.split_once('.').ok_or(ValueError)?;
        let valid = !version.is_empty()
            && version.chars().all(|c| c.is_ascii_hexdigit())
            && !address.is_empty();
        return if valid { Ok(()) } else { Err(ValueError) };
    }
    if host.parse::<Ipv4Addr>().is_ok() {
        return Err(ValueError);
    }
    // ipaddress accepts a scope after `%`, which must be non-empty and contain no `%`.
    let address = match host.split_once('%') {
        Some((address, scope)) if !scope.is_empty() && !scope.contains('%') => address,
        Some(_) => return Err(ValueError),
        None => host,
    };
    address
        .parse::<Ipv6Addr>()
        .map(|_| ())
        .map_err(|_| ValueError)
}

impl Split<'_> {
    fn host_info(&self) -> (&str, Option<&str>) {
        let info = self
            .netloc
            .rsplit_once('@')
            .map_or(self.netloc, |(_, host)| host);
        let (host, port) = match info.split_once('[') {
            Some((_, bracketed)) => {
                let (host, after) = bracketed.split_once(']').unwrap_or((bracketed, ""));
                (host, after.split_once(':').map_or("", |(_, port)| port))
            }
            None => info.split_once(':').unwrap_or((info, "")),
        };
        (host, Some(port).filter(|port| !port.is_empty()))
    }
    /// `.hostname`: lowercased except an IPv6 scope.
    pub fn hostname(&self) -> Option<String> {
        let (host, _) = self.host_info();
        if host.is_empty() {
            return None;
        }
        let (name, zone) = host
            .split_once('%')
            .map_or((host, None), |(name, zone)| (name, Some(zone)));
        // str.lower(): only ASCII letters can reach a host the connector accepts.
        let mut lowered = name.to_ascii_lowercase();
        if let Some(zone) = zone {
            lowered.push('%');
            lowered.push_str(zone);
        }
        Some(lowered)
    }
    /// `.port`: `Err` for Python's ValueError (not ASCII digits, or out of range).
    pub fn port(&self) -> std::result::Result<Option<u16>, ValueError> {
        match self.host_info().1 {
            None => Ok(None),
            Some(port) if port.bytes().all(|b| b.is_ascii_digit()) => {
                let digits = port.trim_start_matches('0');
                if digits.len() > 5 {
                    return Err(ValueError);
                }
                u16::try_from(digits.parse::<u32>().unwrap_or(0))
                    .map(Some)
                    .map_err(|_| ValueError)
            }
            Some(_) => Err(ValueError),
        }
    }
    /// Whether `.username` or `.password` is not None.
    pub fn has_userinfo(&self) -> bool {
        self.netloc.contains('@')
    }
}

/// Python's `checked_url`: the URL without trailing slashes.
pub fn checked_url(value: &str, allow_insecure_localhost: bool) -> Result<String> {
    if value.chars().any(|c| (c as u32) < 33 || c as u32 == 127) {
        return Err(ConnectorError::new(INVALID_SERVER_URL));
    }
    let insecure = || ConnectorError::new(INSECURE_SERVER_URL);
    let parsed = urlsplit(value).map_err(|_| insecure())?;
    parsed.port().map_err(|_| insecure())?;
    let hostname = parsed.hostname();
    let valid = hostname.is_some()
        && !parsed.has_userinfo()
        && parsed.query.is_empty()
        && parsed.fragment.is_empty();
    let secure = parsed.scheme == "https";
    let local = allow_insecure_localhost
        && parsed.scheme == "http"
        && matches!(hostname.as_deref(), Some("localhost" | "127.0.0.1" | "::1"));
    if !valid || !(secure || local) {
        return Err(insecure());
    }
    Ok(value.trim_end_matches('/').to_owned())
}

/// `managed_origin(value)`: the official relay a URL names, if exactly one.
pub fn managed_origin(value: &str) -> Option<&'static str> {
    let url = urlsplit(value).ok()?;
    if url.scheme != "https"
        || !matches!(url.port(), Ok(None | Some(443)))
        || url.has_userinfo()
        || !url.query.is_empty()
        || !url.fragment.is_empty()
        || !matches!(url.path, "" | "/")
    {
        return None;
    }
    match url.hostname()?.as_str() {
        "zerus.dev.guthub.dev" => Some(LEGACY),
        "relay.zerus.dev" => Some(CURRENT),
        _ => None,
    }
}

/// `transport(value)`: the legacy managed relay is reached at its new origin.
pub fn transport(value: &str) -> String {
    if managed_origin(value) == Some(LEGACY) {
        CURRENT.to_owned()
    } else {
        value.to_owned()
    }
}

/// `identity_url(server_url, explicit)`: the spelling the binding hash keeps.
pub fn identity_url(server_url: &str, explicit: Option<&str>) -> Result<String> {
    match explicit {
        None => Ok(server_url.to_owned()),
        Some(explicit) if explicit == server_url => Ok(server_url.to_owned()),
        Some(explicit)
            if managed_origin(server_url) == Some(CURRENT)
                && managed_origin(explicit) == Some(LEGACY) =>
        {
            Ok(explicit.to_owned())
        }
        Some(_) => Err(ConnectorError::new(FOREIGN_IDENTITY)),
    }
}

/// Where the HTTP client connects. Requests go to `prefix` + the API path, as
/// Python appends the path to `server_url`; the bearer token goes only here.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Transport {
    pub url: String,
    pub secure: bool,
    /// Lowercase, without IPv6 brackets.
    pub host: String,
    pub port: u16,
    /// Everything after the authority: the path, plus an empty `?` or `#`
    /// that `checked_url` allows.
    pub prefix: String,
}

impl Transport {
    /// Splits a URL that `checked_url` accepted (or the managed origin).
    pub fn new(url: &str) -> Result<Self> {
        let insecure = || ConnectorError::new(INSECURE_SERVER_URL);
        let parsed = urlsplit(url).map_err(|_| insecure())?;
        let host = parsed.hostname().ok_or_else(insecure)?;
        if !host.is_ascii() {
            return Err(ConnectorError::new(NON_ASCII_HOST));
        }
        let secure = parsed.scheme == "https";
        let port = parsed
            .port()
            .map_err(|_| insecure())?
            .unwrap_or(if secure { 443 } else { 80 });
        let authority_end = url
            .find("//")
            .map_or(0, |start| start + 2 + parsed.netloc.len());
        Ok(Self {
            url: url.to_owned(),
            secure,
            host,
            port,
            prefix: url[authority_end..].to_owned(),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn splits_like_python() {
        let split = urlsplit("HTTPS://User@Relay.Example.Test:8443/v1?q#f").unwrap();
        assert_eq!(split.scheme, "https");
        assert_eq!(split.netloc, "User@Relay.Example.Test:8443");
        assert_eq!((split.path, split.query, split.fragment), ("/v1", "q", "f"));
        assert_eq!(split.hostname().as_deref(), Some("relay.example.test"));
        assert_eq!(split.port(), Ok(Some(8443)));
        assert!(split.has_userinfo());
        assert_eq!(
            urlsplit("https://[fe80::1%Lo0]:1/")
                .unwrap()
                .hostname()
                .as_deref(),
            Some("fe80::1%Lo0")
        );
        for invalid in [
            "https://[::1",
            "https://::1]",
            "https://x[::1]",
            "https://[::1]x",
            "https://[1.2.3.4]",
            "https://[v.x]",
            "https://[::1%]",
            "https://[nothex]",
        ] {
            assert!(urlsplit(invalid).is_err(), "{invalid}");
        }
        assert!(urlsplit("https://[v1F.anything]").is_ok());
        assert_eq!(
            urlsplit("https://h:00000000443").unwrap().port(),
            Ok(Some(443))
        );
        assert!(urlsplit("https://h:65536").unwrap().port().is_err());
        assert!(urlsplit("https://h:+1").unwrap().port().is_err());
        // An empty port is no port; a colon-only scheme is no scheme.
        assert_eq!(urlsplit("https://h:").unwrap().port(), Ok(None));
        assert_eq!(urlsplit("1http://h").unwrap().scheme, "");
    }

    #[test]
    fn transport_is_the_authority_and_its_prefix() {
        let plain = Transport::new("https://relay.example.test").unwrap();
        assert_eq!(
            (
                plain.secure,
                plain.host.as_str(),
                plain.port,
                plain.prefix.as_str()
            ),
            (true, "relay.example.test", 443, "")
        );
        let local = Transport::new("http://[::1]:8787/base").unwrap();
        assert_eq!(
            (
                local.secure,
                local.host.as_str(),
                local.port,
                local.prefix.as_str()
            ),
            (false, "::1", 8787, "/base")
        );
        assert_eq!(
            Transport::new("https://r\u{e9}lay.example.test")
                .unwrap_err()
                .message(),
            NON_ASCII_HOST
        );
    }
}
