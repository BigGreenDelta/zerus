//! `hgs mobile`: pair, list and revoke phones with this computer's mobile
//! connector credential. The desktop asks a gateway peer over SSH, one hop, the
//! way phones reach peers through gateways. The token stays in the private
//! connector configuration and reaches curl on standard input only.
use crate::{
    cli::{Error, Result},
    config::Config,
};
use serde_json::{json, Value};
use std::{
    io::{Read, Write},
    process::{Command, Stdio},
};

const USAGE: &str = "usage: hgs [@host] mobile phones|invite|revoke-phone <id> [--workspace ID --server URL] --json";
const CHANGED_RELAY: &str =
    "The mobile connection on this computer now uses another relay; refresh the phone list";
const CHANGED_WORKSPACE: &str =
    "The mobile connection on this computer now belongs to another workspace; refresh the phone list";
const MAX_CONFIG_BYTES: u64 = 65_536;
const MAX_RESPONSE_BYTES: u64 = 1_048_576;

pub fn dispatch(config: &Config, args: &[String], dry: bool) -> Result<i32> {
    let (action, rest) = match args {
        [action, rest @ ..] if action == "phones" => (Action::Phones, rest),
        [action, rest @ ..] if action == "invite" => (Action::Invite, rest),
        [action, id, rest @ ..] if action == "revoke-phone" => {
            if !device_id(id) {
                return Err(Error::new(1, "Choose a phone by its device ID"));
            }
            (Action::Revoke(id.clone()), rest)
        }
        _ => return Err(Error::new(1, USAGE)),
    };
    // The desktop names the relay and workspace it listed, so a changed
    // connection never issues or revokes for another one.
    let pin = match rest {
        [json] if json == "--json" => None,
        [w, workspace, s, server, json]
            if w == "--workspace"
                && s == "--server"
                && json == "--json"
                && !matches!(action, Action::Phones) =>
        {
            if !workspace_id(workspace) {
                return Err(Error::new(1, "Choose a workspace by its ID"));
            }
            Some((workspace.clone(), server.trim_end_matches('/').to_owned()))
        }
        _ => return Err(Error::new(1, USAGE)),
    };
    if dry {
        return Err(Error::new(1, USAGE));
    }
    let Some(connection) = connection(config)? else {
        if matches!(action, Action::Phones) {
            println!("{}", json!({"configured": false}));
            return Ok(0);
        }
        return Err(Error::new(
            1,
            "This computer has no mobile connection; set it up in Settings → Mobile connection",
        ));
    };
    if pin
        .as_ref()
        .is_some_and(|(_, server)| *server != connection.server)
    {
        return Err(Error::new(1, CHANGED_RELAY));
    }
    let workspace = pin.map(|(workspace, _)| workspace);
    let output = match action {
        Action::Phones => {
            let mut value = connection.call("GET", "/v1/node/devices", None, false)?;
            object(&mut value)?;
            let mut out = json!({"configured": true, "server_url": connection.server});
            out.as_object_mut()
                .unwrap()
                .extend(value.as_object().unwrap().clone());
            out
        }
        Action::Invite => {
            let body = match &workspace {
                Some(workspace) => json!({"workspace_id": workspace}),
                None => json!({}),
            };
            let mut value = connection.call(
                "POST",
                "/v1/node/invitations",
                Some(&body.to_string()),
                false,
            )?;
            if workspace
                .as_ref()
                .is_some_and(|workspace| value["workspace_id"] != json!(workspace))
            {
                return Err(Error::new(1, CHANGED_WORKSPACE));
            }
            let code = object(&mut value)?
                .get("pair_code")
                .and_then(Value::as_str)
                .filter(|code| code.len() <= 128)
                .ok_or_else(|| Error::new(1, "The relay returned an invalid invitation"))?
                .to_owned();
            let fields = value.as_object_mut().unwrap();
            fields.insert("server_url".into(), json!(connection.server));
            fields.insert(
                "pair_url".into(),
                json!(format!(
                    "zerus://pair?server={}&code={}",
                    encode(&connection.server),
                    encode(&code)
                )),
            );
            value
        }
        Action::Revoke(id) => {
            let scope = workspace
                .map(|workspace| format!("?workspace_id={workspace}"))
                .unwrap_or_default();
            connection.call(
                "DELETE",
                &format!("/v1/node/devices/{id}{scope}"),
                None,
                true,
            )?;
            json!({"revoked": true, "device_id": id})
        }
    };
    println!("{output}");
    Ok(0)
}

enum Action {
    Phones,
    Invite,
    Revoke(String),
}

struct Connection {
    server: String,
    token: String,
    insecure: bool,
}

fn connection(config: &Config) -> Result<Option<Connection>> {
    let path = config.dir.join("mobile/connector.json");
    #[cfg(unix)]
    let opened = std::fs::File::open(&path);
    #[cfg(windows)]
    let opened = crate::platform::refuse_link(std::fs::OpenOptions::new().read(true)).open(&path);
    let file = match opened {
        Ok(file) => file,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(_) => {
            return Err(Error::new(
                1,
                "Cannot read the mobile connection configuration",
            ))
        }
    };
    #[cfg(windows)]
    if !crate::platform::verify_private_file(&file).unwrap_or(false) {
        return Err(Error::new(
            1,
            "The mobile connection configuration must be private to this Windows user",
        ));
    }
    let mut bytes = Vec::new();
    file.take(MAX_CONFIG_BYTES + 1).read_to_end(&mut bytes)?;
    let settings: Value = serde_json::from_slice(&bytes)
        .ok()
        .filter(|_| bytes.len() as u64 <= MAX_CONFIG_BYTES)
        .ok_or_else(|| Error::new(1, "Invalid mobile connection configuration"))?;
    let server = settings["server_url"]
        .as_str()
        .unwrap_or("")
        .trim_end_matches('/');
    let token = settings["node_token"].as_str().unwrap_or("");
    if token.is_empty() || token.len() > 8192 || !token.bytes().all(|b| (33..=126).contains(&b)) {
        return Err(Error::new(
            1,
            "The mobile connection has no valid computer credential",
        ));
    }
    let loopback = ["http://127.0.0.1", "http://[::1]", "http://localhost"]
        .iter()
        .any(|origin| {
            server
                .strip_prefix(origin)
                .is_some_and(|rest| rest.is_empty() || rest.starts_with([':', '/']))
        });
    let insecure =
        loopback && std::env::var("HGS_MOBILE_ALLOW_INSECURE_LOCALHOST").as_deref() == Ok("1");
    let valid = server.len() <= 2048
        && (server.starts_with("https://") || insecure)
        && !server
            .bytes()
            .any(|b| b <= b' ' || b == 127 || b == b'"' || b == b'\\');
    if !valid {
        return Err(Error::new(
            1,
            "The mobile connection needs an HTTPS relay address",
        ));
    }
    Ok(Some(Connection {
        server: server.to_owned(),
        token: token.to_owned(),
        insecure,
    }))
}

impl Connection {
    /// One bounded request: no curlrc (`-q` must come first), so no redirects
    /// or traces from a user's defaults; no environment proxies (like the
    /// connector); and the bearer token only in curl's configuration on stdin.
    fn call(&self, method: &str, path: &str, body: Option<&str>, revoke: bool) -> Result<Value> {
        let mut command = Command::new("curl");
        command.args([
            "-q",
            "--silent",
            "--show-error",
            "--noproxy",
            "*",
            "--proto",
        ]);
        command.arg(if self.insecure {
            "=http,https"
        } else {
            "=https"
        });
        command.args([
            "--connect-timeout",
            "5",
            "--max-time",
            "20",
            "--max-filesize",
            "1048576",
        ]);
        command.args([
            "--config",
            "-",
            "--write-out",
            "\n%{http_code}",
            "--request",
            method,
        ]);
        // Schannel ignores curl's CA environment override; pass the explicitly
        // selected bundle without changing default native trust or verification.
        #[cfg(windows)]
        apply_ca_bundle(&mut command, std::env::var_os("CURL_CA_BUNDLE").as_deref());
        if let Some(body) = body {
            command.args(["--header", "Content-Type: application/json", "--data", body]);
        }
        command.arg(format!("{}{path}", self.server));
        command
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        let mut child = command
            .spawn()
            .map_err(|_| Error::new(1, "Could not reach the relay: curl is not installed"))?;
        let header = self.token.replace('\\', "\\\\").replace('"', "\\\"");
        let written = child
            .stdin
            .take()
            .map(|mut stdin| writeln!(stdin, "header = \"Authorization: Bearer {header}\""));
        let mut stdout = Vec::new();
        child
            .stdout
            .take()
            .unwrap()
            .take(MAX_RESPONSE_BYTES + 16)
            .read_to_end(&mut stdout)?;
        let mut stderr = String::new();
        child
            .stderr
            .take()
            .unwrap()
            .take(4096)
            .read_to_string(&mut stderr)
            .ok();
        let status = child.wait()?;
        if !status.success() || !matches!(written, Some(Ok(()))) {
            let reason = stderr.lines().next().unwrap_or("").trim();
            return Err(Error::new(
                1,
                format!("Could not reach the relay: {reason}")
                    .trim_end_matches([':', ' '])
                    .to_owned(),
            ));
        }
        let text = String::from_utf8_lossy(&stdout);
        let (body, code) = text.rsplit_once('\n').unwrap_or(("", &text));
        match code.trim() {
            "200" => serde_json::from_str(body)
                .map_err(|_| Error::new(1, "The relay returned an invalid response")),
            "401" | "403" => Err(Error::new(
                1,
                "The relay rejected this computer's credential",
            )),
            "404" if revoke => Err(Error::new(
                1,
                "That phone is not linked to this workspace; it may already be revoked",
            )),
            "404" => Err(Error::new(
                1,
                "The relay does not support phone management yet; update it",
            )),
            "409" => Err(Error::new(1, CHANGED_WORKSPACE)),
            "429" => Err(Error::new(
                1,
                "Too many requests to the relay; try again in a minute",
            )),
            "503" => Err(Error::new(1, "The relay is temporarily unavailable")),
            other => Err(Error::new(1, format!("The relay returned HTTP {other}"))),
        }
    }
}

#[cfg(any(windows, test))]
fn apply_ca_bundle(command: &mut Command, bundle: Option<&std::ffi::OsStr>) {
    if let Some(path) = bundle.filter(|path| !path.is_empty()) {
        command.arg("--cacert").arg(path);
    }
}

fn object(value: &mut Value) -> Result<&serde_json::Map<String, Value>> {
    value
        .as_object()
        .ok_or_else(|| Error::new(1, "The relay returned an invalid response"))
}

fn workspace_id(id: &str) -> bool {
    (1..=64).contains(&id.len()) && id.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-')
}

fn device_id(id: &str) -> bool {
    id.len() == 36
        && id.bytes().enumerate().all(|(i, b)| {
            if matches!(i, 8 | 13 | 18 | 23) {
                b == b'-'
            } else {
                b.is_ascii_hexdigit()
            }
        })
}

/// Percent-encodes everything but RFC 3986 unreserved characters.
fn encode(text: &str) -> String {
    text.bytes()
        .map(|b| match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'.' | b'_' | b'~' => {
                (b as char).to_string()
            }
            _ => format!("%{b:02X}"),
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn absent_or_empty_windows_ca_bundle_preserves_curl_arguments() {
        for bundle in [None, Some(std::ffi::OsStr::new(""))] {
            let mut command = Command::new("curl");
            command.args(["-q", "--proto", "=https", "--max-time", "20"]);
            let before: Vec<_> = command.get_args().map(|arg| arg.to_owned()).collect();
            apply_ca_bundle(&mut command, bundle);
            let after: Vec<_> = command.get_args().map(|arg| arg.to_owned()).collect();
            assert_eq!(after, before);
        }
    }

    #[test]
    fn windows_ca_bundle_is_one_path_argument_without_verification_bypass() {
        for path in [
            r"C:\private TLS\Δ CA.pem",
            r"\\fixture.example.test\share\CA file.pem",
        ] {
            let mut command = Command::new("curl");
            command.args([
                "-q",
                "--silent",
                "--show-error",
                "--noproxy",
                "*",
                "--proto",
                "=https",
                "--connect-timeout",
                "5",
                "--max-time",
                "20",
                "--max-filesize",
                "1048576",
                "--config",
                "-",
            ]);
            let before: Vec<_> = command.get_args().map(|arg| arg.to_owned()).collect();
            apply_ca_bundle(&mut command, Some(std::ffi::OsStr::new(path)));
            let args: Vec<_> = command.get_args().map(|arg| arg.to_owned()).collect();
            assert_eq!(&args[..before.len()], before.as_slice());
            assert_eq!(args.len(), before.len() + 2);
            assert_eq!(args[before.len()].as_os_str(), "--cacert");
            assert_eq!(
                args[before.len() + 1].as_os_str(),
                std::ffi::OsStr::new(path)
            );
            assert!(!args.iter().any(|arg| {
                ["--insecure", "-k", "--ssl-no-revoke"]
                    .iter()
                    .any(|flag| arg.as_os_str() == std::ffi::OsStr::new(flag))
            }));
        }
    }

    #[test]
    fn pairing_links_encode_the_server_and_device_ids_are_exact() {
        assert_eq!(
            encode("https://relay.example.test:8443/a b"),
            "https%3A%2F%2Frelay.example.test%3A8443%2Fa%20b"
        );
        assert!(device_id("00000000-0000-4000-8000-0000000000d1"));
        assert!(!device_id("00000000-0000-4000-8000-0000000000d"));
        assert!(!device_id("00000000-0000-4000-8000/000000000d1"));
        assert!(!device_id("../devices/00000000-0000-4000-8000-0"));
    }
}
