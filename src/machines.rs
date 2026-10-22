//! Local connection overrides. The shared/Ansible config and OpenSSH files stay intact.
use crate::{
    cli::{Error, Result},
    config::Config,
    platform,
};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::{
    collections::{BTreeMap, BTreeSet},
    fs,
    io::{Read, Seek, SeekFrom, Write},
    path::Path,
    process::{Command, Output, Stdio},
    time::{Duration, Instant},
};

fn yes() -> bool {
    true
}
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
#[serde(default, deny_unknown_fields)]
pub struct Connection {
    pub hostname: String,
    pub user: String,
    pub port: Option<u16>,
    pub identity_file: String,
    #[serde(default = "yes")]
    pub enabled: bool,
}
impl Default for Connection {
    fn default() -> Self {
        Self {
            hostname: String::new(),
            user: String::new(),
            port: None,
            identity_file: String::new(),
            enabled: true,
        }
    }
}
#[derive(Default, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Store {
    #[serde(default)]
    machines: BTreeMap<String, Connection>,
}

fn read(dir: &Path) -> Result<Vec<u8>> {
    match fs::read(dir.join("machines.local.json")) {
        Ok(bytes) => Ok(bytes),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(Vec::new()),
        Err(e) => Err(e.into()),
    }
}
fn decode(bytes: &[u8]) -> Result<Store> {
    if bytes.is_empty() {
        return Ok(Store::default());
    }
    let store: Store = serde_json::from_slice(bytes)
        .map_err(|e| Error::new(1, format!("invalid machines.local.json: {e}")))?;
    for (alias, connection) in &store.machines {
        validate(alias, connection)?;
    }
    Ok(store)
}
pub fn load(dir: &Path) -> Result<BTreeMap<String, Connection>> {
    Ok(decode(&read(dir)?)?.machines)
}
fn revision(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}
fn validate_alias(alias: &str) -> Result<()> {
    if alias.is_empty()
        || alias.len() > 80
        || alias.starts_with('-')
        || !alias
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || b"_.-".contains(&c))
    {
        return Err(Error::new(
            1,
            "Machine alias: use letters, digits, dots, underscores and hyphens; no leading hyphen.",
        ));
    }
    Ok(())
}
fn validate(alias: &str, c: &Connection) -> Result<()> {
    validate_alias(alias)?;
    if c.hostname.len() > 253
        || c.hostname.starts_with('-')
        || !c
            .hostname
            .bytes()
            .all(|ch| ch.is_ascii_alphanumeric() || b"._:%-".contains(&ch))
    {
        return Err(Error::new(
            1,
            "Hostname must be a DNS name or IPv4/IPv6 address, without spaces.",
        ));
    }
    if c.user.len() > 128
        || c.user.starts_with('-')
        || !c
            .user
            .bytes()
            .all(|ch| ch.is_ascii_alphanumeric() || b"_.-".contains(&ch))
    {
        return Err(Error::new(1, "SSH user contains unsupported characters."));
    }
    if c.port == Some(0) {
        return Err(Error::new(1, "SSH port must be between 1 and 65535."));
    }
    if c.identity_file.len() > 4096
        || c.identity_file.chars().any(char::is_control)
        || (!c.identity_file.is_empty()
            && !c.identity_file.starts_with('/')
            && !c.identity_file.starts_with("~/")
            // The key belongs to this machine's ssh.exe (machines.local.json is never shared).
            && !(cfg!(windows)
                && (Path::new(&c.identity_file).is_absolute()
                    || c.identity_file.starts_with("~\\"))))
    {
        return Err(Error::new(
            1,
            "Choose an absolute SSH key path or a path starting with ~/ .",
        ));
    }
    Ok(())
}
pub fn effective_peers(
    base: &[String],
    overrides: &BTreeMap<String, Connection>,
    environment: bool,
) -> Vec<String> {
    if environment {
        return base.to_vec();
    }
    let mut peers: Vec<String> = base
        .iter()
        .filter(|name| overrides.get(*name).is_none_or(|c| c.enabled))
        .cloned()
        .collect();
    for (alias, connection) in overrides {
        if connection.enabled && !peers.contains(alias) {
            peers.push(alias.clone());
        }
    }
    peers
}
pub fn ssh_options(config: &Config, alias: &str) -> Vec<String> {
    let Some(c) = config.machines.get(alias) else {
        return Vec::new();
    };
    let mut options = Vec::new();
    if !c.hostname.is_empty() {
        options.extend(["-o".into(), format!("HostName={}", c.hostname)]);
    }
    if !c.user.is_empty() {
        options.extend(["-l".into(), c.user.clone()]);
    }
    if let Some(port) = c.port {
        options.extend(["-p".into(), port.to_string()]);
    }
    if !c.identity_file.is_empty() {
        options.extend(["-i".into(), config.expand_home(&c.identity_file)]);
    }
    options
}
fn write_profile(
    config: &Config,
    alias: &str,
    connection: Option<Connection>,
    expected: Option<&str>,
    create_only: bool,
) -> Result<()> {
    fs::create_dir_all(&config.dir)?;
    let lock = fs::OpenOptions::new()
        .create(true)
        .truncate(false)
        .read(true)
        .write(true)
        .open(config.dir.join("machines.lock"))?;
    let _lock = platform::FileLock::exclusive(lock)?;
    let bytes = read(&config.dir)?;
    if expected.is_some_and(|value| value != revision(&bytes)) {
        return Err(Error::new(
            1,
            "Machine settings changed elsewhere. Reload the page before saving.",
        ));
    }
    let mut store = decode(&bytes)?;
    if create_only
        && (store.machines.contains_key(alias) || config.base_peers.iter().any(|p| p == alias))
    {
        return Err(Error::new(
            1,
            "A machine with this name already exists. Select it to edit its connection.",
        ));
    }
    if let Some(c) = connection {
        validate(alias, &c)?;
        store.machines.insert(alias.into(), c);
    } else {
        store.machines.remove(alias);
    }
    let mut stage = tempfile::NamedTempFile::new_in(&config.dir)?;
    let mut output = serde_json::to_vec_pretty(&store).map_err(|e| Error::new(1, e.to_string()))?;
    output.push(b'\n');
    stage.write_all(&output)?;
    stage.as_file().sync_all()?;
    platform::persist(stage, config.dir.join("machines.local.json"))
        .map_err(|e| Error::new(1, e.to_string()))?;
    platform::sync_dir(&config.dir)?;
    Ok(())
}
fn list(config: &Config) -> Result<Value> {
    // Profile values and the optimistic-lock revision must describe one snapshot.
    let bytes = read(&config.dir)?;
    let machines = decode(&bytes)?.machines;
    let peers = effective_peers(&config.base_peers, &machines, config.peers_from_environment);
    let aliases: BTreeSet<&String> = config.base_peers.iter().chain(machines.keys()).collect();
    let rows: Vec<Value> = aliases
        .into_iter()
        .filter(|alias| !config.is_self(alias))
        .map(|alias| {
            let connection = machines.get(alias).cloned().unwrap_or_default();
            json!({"alias":alias,"connection":connection,"enabled":peers.contains(alias),
            "source":if machines.contains_key(alias) {"local"} else {"config"},
            "inherited":config.base_peers.contains(alias)})
        })
        .collect();
    Ok(
        json!({"local":config.host(),"machines":rows,"revision":revision(&bytes),"environment_override":config.peers_from_environment}),
    )
}
// `ssh -G` never opens a network connection, but a user's Match exec directive
// may launch a process. Bound its lifetime and output just like a network probe.
fn resolve_output(command: &mut Command) -> Result<Output> {
    const LIMIT: u64 = 128 * 1024;
    let mut stdout = tempfile::tempfile()?;
    let mut stderr = tempfile::tempfile()?;
    let mut child = platform::spawn_group(
        command
            .stdin(Stdio::null())
            .stdout(stdout.try_clone()?)
            .stderr(stderr.try_clone()?),
    )?;
    let deadline = Instant::now() + Duration::from_secs(4);
    let status = loop {
        let error = if stdout.metadata()?.len() > LIMIT || stderr.metadata()?.len() > LIMIT {
            Some("SSH configuration output exceeded its limit.")
        } else if Instant::now() >= deadline {
            Some("SSH configuration resolution timed out.")
        } else {
            None
        };
        if let Some(error) = error {
            // Our own process group contains only this invocation and children
            // started by SSH configuration; never signal the app's process group.
            platform::kill_process_group(&mut child);
            let _ = child.wait();
            return Err(Error::new(1, error));
        }
        if let Some(status) = child.try_wait()? {
            break status;
        }
        std::thread::sleep(Duration::from_millis(20));
    };
    stdout.seek(SeekFrom::Start(0))?;
    stderr.seek(SeekFrom::Start(0))?;
    let mut output = Output {
        status,
        stdout: Vec::new(),
        stderr: Vec::new(),
    };
    stdout.take(LIMIT + 1).read_to_end(&mut output.stdout)?;
    stderr.take(LIMIT + 1).read_to_end(&mut output.stderr)?;
    if output.stdout.len() as u64 > LIMIT || output.stderr.len() as u64 > LIMIT {
        return Err(Error::new(
            1,
            "SSH configuration output exceeded its limit.",
        ));
    }
    Ok(output)
}
fn resolve(config: &Config, alias: &str) -> Result<Value> {
    let out = resolve_output(
        platform::ssh()
            .args(ssh_options(config, alias))
            .args(["-G", "-T", alias]),
    )?;
    if !out.status.success() {
        return Err(Error::new(
            1,
            format!(
                "Could not resolve SSH settings: {}",
                String::from_utf8_lossy(&out.stderr)
                    .chars()
                    .take(2000)
                    .collect::<String>()
                    .trim()
            ),
        ));
    }
    let mut effective = json!({"identity_files":[],"proxy_command":false});
    for line in String::from_utf8_lossy(&out.stdout).lines() {
        let Some((key, value)) = line.split_once(char::is_whitespace) else {
            continue;
        };
        let value = value.trim();
        if value.len() > 4096 {
            continue;
        }
        match key {
            "hostname" | "user" | "port" => effective[key] = json!(value),
            "proxyjump" if value != "none" => effective["proxy_jump"] = json!(value),
            "proxycommand" => effective["proxy_command"] = json!(value != "none"),
            "identityfile" => {
                let files = effective["identity_files"].as_array_mut().unwrap();
                if files.len() < 16 && !files.contains(&json!(value)) {
                    files.push(json!(value));
                }
            }
            _ => {} // Never expose arbitrary options, environment values or proxy secrets.
        }
    }
    if !["hostname", "user", "port"]
        .iter()
        .all(|key| effective[*key].as_str().is_some_and(|s| !s.is_empty()))
    {
        return Err(Error::new(
            1,
            "SSH did not return the effective hostname, user and port.",
        ));
    }
    Ok(json!({"alias":alias,"effective":effective}))
}
const PROBE: &str = r#"export PATH="$HOME/.cargo/bin:/opt/homebrew/bin:/usr/local/bin:$PATH"; printf '\n__HGS_MACHINE__\n'; printf 'os='; uname -s; printf 'arch='; uname -m; printf 'hgs='; if test -x "$HOME/.local/bin/hgs"; then "$HOME/.local/bin/hgs" --version 2>/dev/null; else printf '\n'; fi; printf 'tmux='; tmux -V 2>/dev/null || printf '\n'; printf 'cargo='; command -v cargo || true"#;
fn check(config: &Config, alias: &str) -> Result<Value> {
    let out = platform::ssh_output(
        platform::ssh()
            .args(ssh_options(config, alias))
            .args([
                "-o",
                "BatchMode=yes",
                "-o",
                "ConnectTimeout=6",
                "-o",
                "ServerAliveInterval=5",
                "-o",
                "ServerAliveCountMax=2",
                "-o",
                "StrictHostKeyChecking=yes",
                alias,
                &platform::remote_sh(PROBE),
            ])
            .stdin(Stdio::null()),
    )?;
    let text = String::from_utf8_lossy(&out.stdout);
    let mut result = json!({"alias":alias,"ok":false,"ssh_ok":out.status.success(),"error":String::from_utf8_lossy(&out.stderr).chars().take(3000).collect::<String>()});
    if out.status.success() {
        if let Some((_, probe)) = text.split_once("__HGS_MACHINE__\n") {
            for line in probe.lines() {
                if let Some((key, value)) = line.split_once('=') {
                    if matches!(key, "os" | "arch" | "hgs" | "tmux" | "cargo") {
                        result[key] = json!(value);
                    }
                }
            }
            result["ok"] = json!(
                result["hgs"]
                    .as_str()
                    .is_some_and(|s| s.starts_with("hgs "))
                    && result["tmux"]
                        .as_str()
                        .is_some_and(|s| s.starts_with("tmux "))
            );
        } else {
            result["error"] =
                json!("SSH connected, but the machine did not return a setup report.");
        }
    }
    Ok(result)
}
fn setup(config: &Config, alias: &str, source: Option<&str>, dry: bool) -> Result<i32> {
    let root = source
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|| Path::new(&config.home).join(".local/src/hgs"));
    if !root.join("Cargo.toml").is_file()
        || !root.join("install.sh").is_file()
        || !root.join("src/main.rs").is_file()
    {
        return Err(Error::new(1, "HGS source folder not found. Choose the hgs checkout containing Cargo.toml and install.sh."));
    }
    if cfg!(windows) {
        // The peer's shell and Python cannot run scripts with CRLF line endings,
        // which a Git checkout on Windows may produce.
        for file in ["install.sh", "hgs_state.py"] {
            if fs::read(root.join(file)).is_ok_and(|bytes| bytes.contains(&b'\r')) {
                return Err(Error::new(1, format!("{file} in the HGS source folder has Windows line endings. Check the source out again with LF line endings, then retry.")));
            }
        }
    }
    // Install in a unique staging directory: don't overwrite the peer's checkout,
    // shared configuration, tmux configuration, or any running process.
    let script = r#"set -eu
export PATH="$HOME/.cargo/bin:/opt/homebrew/bin:/usr/local/bin:$PATH"
case "$(uname -s)" in Linux|Darwin) ;; *) echo 'HGS supports Linux and macOS peers.' >&2; exit 1;; esac
command -v cargo >/dev/null 2>&1 || { echo 'Rust is missing. Install the newest stable Rust on this machine, then retry.' >&2; exit 1; }
command -v tmux >/dev/null 2>&1 || { echo 'tmux is missing. Install tmux on this machine, then retry.' >&2; exit 1; }
mkdir -p "$HOME/.local/share/hgs" "$HOME/.config/hgs"
hgs_stage=$(mktemp -d "$HOME/.local/share/hgs/setup.XXXXXXXX")
trap 'rm -rf "$hgs_stage"' EXIT HUP INT TERM
tar xzf - -C "$hgs_stage"
printf 'Building HGS for this machine…\n'
HGS_INSTALL_PREFIX="$HOME/.local" bash "$hgs_stage/install.sh"
if test ! -e "$HOME/.config/hgs/config"; then
  (set -C; printf 'HGS_SELF="%s"\n' "$1" > "$HOME/.config/hgs/config") || true
fi
printf 'Setup complete: '
"$HOME/.local/bin/hgs" --version
"#;
    let remote = format!(
        "sh -c {} hgs-setup {}",
        platform::remote_quote(script),
        platform::remote_quote(alias)
    );
    let mut arguments = ssh_options(config, alias);
    arguments.extend([
        "-o".into(),
        "BatchMode=yes".into(),
        "-o".into(),
        "ConnectTimeout=6".into(),
        "-o".into(),
        "StrictHostKeyChecking=yes".into(),
        alias.into(),
        remote,
    ]);
    if dry {
        println!("Bundle CLI sources from {} and run a native build/install on {alias} (no session restart).", root.display());
        return Ok(0);
    }
    let bundle = tempfile::NamedTempFile::new()?;
    let result = platform::status(
        Command::new("tar")
            .args(["czf", "-"])
            .arg("-C")
            .arg(&root)
            .args([
                "Cargo.toml",
                "Cargo.lock",
                "build.rs",
                "src",
                "scripts",
                "install.sh",
                "hgs_state.py",
            ])
            .stdout(bundle.reopen()?),
    )?;
    if !result.success() {
        return Err(Error::new(1, "Could not package the HGS CLI sources."));
    }
    let code = platform::ssh_status(
        platform::ssh().args(arguments),
        fs::File::open(bundle.path())?,
    )?;
    Ok(code.code().unwrap_or(1))
}

pub fn dispatch(config: &Config, args: &[String], dry: bool) -> Result<i32> {
    let command = args.first().map(String::as_str).unwrap_or("ls");
    if command == "ls" {
        if args.len() > 1 {
            return Err(Error::new(1, "usage: hgs machine ls"));
        }
        println!("{}", list(config)?);
        return Ok(0);
    }
    let alias = args.get(1).ok_or_else(|| {
        Error::new(
            1,
            "usage: hgs machine <set|remove|reset|resolve|check|ssh|setup> <alias>",
        )
    })?;
    validate_alias(alias)?;
    if config.is_self(alias) {
        return Err(Error::new(
            1,
            "This is the local machine; SSH profiles are for remote machines.",
        ));
    }
    match command {
        "resolve" => {
            if args.len() != 2 {
                return Err(Error::new(1, "usage: hgs machine resolve <alias>"));
            }
            println!("{}", resolve(config, alias)?);
            Ok(0)
        }
        "set" | "remove" | "reset" => {
            let mut connection = None;
            let mut expected = None;
            let mut create_only = false;
            let mut index = 2;
            while index < args.len() {
                if args[index] == "--create" && command == "set" {
                    create_only = true;
                    index += 1;
                    continue;
                }
                let value = args
                    .get(index + 1)
                    .ok_or_else(|| Error::new(1, "Machine option needs a value"))?;
                match args[index].as_str() {
                    "--json" if command == "set" => {
                        connection = Some(
                            serde_json::from_str::<Connection>(value)
                                .map_err(|e| Error::new(1, e.to_string()))?,
                        )
                    }
                    "--revision" => expected = Some(value.as_str()),
                    _ => return Err(Error::new(1, "Unknown machine option")),
                }
                index += 2;
            }
            if command == "set" && connection.is_none() {
                return Err(Error::new(1, "machine set needs --json <connection>"));
            }
            if command == "remove" && config.base_peers.contains(alias) {
                if config.peers_from_environment {
                    return Err(Error::new(1, "HGS_PEERS in the environment controls this machine; change that environment setting first."));
                }
                connection = Some(Connection {
                    enabled: false,
                    ..config.machines.get(alias).cloned().unwrap_or_default()
                });
            }
            if let Some(c) = &connection {
                validate(alias, c)?;
            }
            if !dry {
                write_profile(config, alias, connection, expected, create_only)?;
            }
            println!("{}", json!({"ok":true,"alias":alias,"dry_run":dry}));
            Ok(0)
        }
        "setup" => {
            let source = match &args[2..] {
                [] => None,
                [flag, path] if flag == "--source" => Some(path.as_str()),
                _ => {
                    return Err(Error::new(
                        1,
                        "usage: hgs machine setup <alias> [--source <checkout>]",
                    ))
                }
            };
            setup(config, alias, source, dry)
        }
        "check" => {
            if args.len() != 2 {
                return Err(Error::new(1, "usage: hgs machine check <alias>"));
            }
            if dry {
                return Err(Error::new(1, "machine check does not support --dry-run"));
            }
            println!("{}", check(config, alias)?);
            Ok(0)
        }
        "ssh" => {
            let directory = match &args[2..] {
                [] => None,
                // The folder is on the remote POSIX machine, whatever this one is.
                [flag, path]
                    if flag == "--directory" && path.starts_with('/') && !path.contains('\0') =>
                {
                    Some(path)
                }
                _ => {
                    return Err(Error::new(
                        1,
                        "usage: hgs machine ssh <alias> [--directory <absolute folder>]",
                    ))
                }
            };
            let mut arguments = ssh_options(config, alias);
            if directory.is_some() {
                arguments.push("-t".into());
            }
            arguments.push(alias.clone());
            if let Some(path) = directory {
                // The login shell itself may be fish; sh expands its path.
                arguments.push(platform::remote_sh(&format!(
                    "cd -- {} && exec \"${{SHELL:-/bin/sh}}\" -l",
                    platform::quote(path)
                )));
            }
            if dry {
                println!("ssh {}", platform::join(&arguments));
                return Ok(0);
            }
            platform::wait_interactive(platform::ssh().args(arguments))
        }
        _ => Err(Error::new(1, "Unknown machine command")),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn list_returns_profile_fields_and_revision_from_the_same_snapshot() {
        let directory = tempfile::tempdir().unwrap();
        let config = Config {
            home: directory.path().to_string_lossy().into_owned(),
            dir: directory.path().to_owned(),
            state: directory.path().join("state"),
            selves: vec!["local".into()],
            peers: Vec::new(),
            base_peers: Vec::new(),
            peers_from_environment: false,
            machines: BTreeMap::new(),
            tab: false,
            tab_colors: String::new(),
        };
        // Another editor saves after this process has already loaded Config.
        let bytes = br#"{"machines":{"new-peer":{"hostname":"new.example"}}}"#;
        fs::write(config.dir.join("machines.local.json"), bytes).unwrap();
        let result = list(&config).unwrap();
        assert_eq!(result["revision"], revision(bytes));
        assert_eq!(result["machines"][0]["alias"], "new-peer");
        assert_eq!(
            result["machines"][0]["connection"]["hostname"],
            "new.example"
        );
        assert_eq!(result["machines"][0]["enabled"], true);
    }
}
