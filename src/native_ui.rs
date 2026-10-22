//! Browser handoff for the native harness. Remote loopback stays behind SSH;
//! launch tokens never enter the session catalog or application logs.
#[cfg(unix)]
use crate::platform;
use crate::{
    cli::{Error, Result},
    config::Config,
};
#[cfg(unix)]
use serde_json::{json, Value};
#[cfg(unix)]
use sha2::{Digest, Sha256};
#[cfg(unix)]
use std::{
    fs,
    net::TcpListener,
    os::unix::fs::DirBuilderExt,
    process::{Command, Stdio},
};

/// The tunnels rely on OpenSSH connection sharing (ControlMaster), which the
/// Windows OpenSSH client does not provide.
#[cfg(not(unix))]
const UNSUPPORTED: &str =
    "The native DeepSeek UI is not available from Windows yet; open it on that machine.";
#[cfg(not(unix))]
pub fn address(_: &Config, _: &str, _: &str) -> Result<String> {
    Err(Error::new(2, UNSUPPORTED))
}
#[cfg(not(unix))]
pub fn open(_: &Config, _: &str, _: &str, _: bool) -> Result<i32> {
    Err(Error::new(2, UNSUPPORTED))
}

// Shared by the embedded view and external browser. The returned launch URL
// is private; callers must never put it in ordinary session state or logs.
#[cfg(unix)]
pub fn address(config: &Config, target: &str, name: &str) -> Result<String> {
    let remote = !target.is_empty() && !config.is_self(target);
    let mut url = if remote {
        if target.starts_with('-') || target.chars().any(char::is_whitespace) {
            return Err(Error::new(1, "Invalid SSH target"));
        }
        let command = format!("~/.local/bin/hgs __dsh-ui-url {}", platform::remote_quote(name));
        let out = platform::ssh()
            .args(crate::machines::ssh_options(config, target))
            .args([
                "-o",
                "BatchMode=yes",
                "-o",
                "ConnectTimeout=5",
                target,
                &command,
            ])
            .output()?;
        if !out.status.success() {
            return Err(Error::new(1, String::from_utf8_lossy(&out.stderr).trim()));
        }
        let response: Value = serde_json::from_slice(&out.stdout)
            .map_err(|_| Error::new(1, "Invalid native UI response"))?;
        response["url"]
            .as_str()
            .ok_or_else(|| Error::new(1, "Missing native UI address"))?
            .to_owned()
    } else {
        crate::state::dsh::native_ui_url(name).map_err(|e| Error::new(1, e))?
    };
    let pattern =
        regex::Regex::new(r"^http://127\.0\.0\.1:([0-9]+)/\?token=[A-Za-z0-9_%.-]+$").unwrap();
    let captures = pattern
        .captures(&url)
        .ok_or_else(|| Error::new(1, "Invalid native loopback URL"))?;
    let port = captures[1]
        .parse::<u16>()
        .map_err(|_| Error::new(1, "Invalid native UI port"))?;
    if remote {
        let dir = config.dir.join("native-tunnels");
        fs::DirBuilder::new()
            .recursive(true)
            .mode(0o700)
            .create(&dir)?;
        let key = format!(
            "{:x}",
            Sha256::digest(format!("{target}:{port}").as_bytes())
        );
        let control = dir.join(format!("{}.sock", &key[..16]));
        let port_file = dir.join(format!("{}.json", &key[..16]));
        let alive = platform::ssh()
            .arg("-S")
            .arg(&control)
            .args(["-O", "check", target])
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status()?
            .success();
        let saved = fs::read(&port_file)
            .ok()
            .and_then(|v| serde_json::from_slice::<Value>(&v).ok())
            .and_then(|v| v["port"].as_u64())
            .filter(|n| *n > 0 && *n <= 65535);
        let local = if let Some(port) = saved.filter(|_| alive) {
            port as u16
        } else {
            let listener = TcpListener::bind("127.0.0.1:0")?;
            let local = listener.local_addr()?.port();
            drop(listener);
            let status = platform::ssh()
                .args(crate::machines::ssh_options(config, target))
                .args(["-M", "-S"])
                .arg(&control)
                .args([
                    "-fN",
                    "-o",
                    "BatchMode=yes",
                    "-o",
                    "ConnectTimeout=5",
                    "-o",
                    "ExitOnForwardFailure=yes",
                    "-o",
                    "ControlPersist=600",
                    "-L",
                ])
                .arg(format!("127.0.0.1:{local}:127.0.0.1:{port}"))
                .arg(target)
                .status()?;
            if !status.success() {
                return Err(Error::new(1, "Could not open the native UI SSH tunnel"));
            }
            fs::write(port_file, json!({"port":local}).to_string())?;
            local
        };
        url = url.replacen(&format!(":{port}/"), &format!(":{local}/"), 1);
    }
    Ok(url)
}

#[cfg(unix)]
pub fn open(config: &Config, target: &str, name: &str, dry: bool) -> Result<i32> {
    if dry {
        println!("Open official DeepSeek UI for {name}");
        return Ok(0);
    }
    let url = address(config, target, name)?;
    let opener = if cfg!(target_os = "macos") {
        "open"
    } else {
        "xdg-open"
    };
    let status = Command::new(opener)
        .arg(&url)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()?;
    if !status.success() {
        return Err(Error::new(
            1,
            "Could not open the browser for the native DeepSeek UI",
        ));
    }
    Ok(0)
}
