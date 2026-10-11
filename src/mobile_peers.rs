//! Mobile reaches the local machine or one enabled, directly configured SSH peer.
//! The remote shell sees only fixed helper commands; native argv travels as data.
use crate::{cli, config::Config, machines, platform, swarm};
use anyhow::{bail, ensure, Context, Result};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::{
    collections::{BTreeMap, BTreeSet},
    io::{Read, Seek, Write},
    os::fd::AsRawFd,
    process::{Command, Stdio},
    sync::{
        atomic::{AtomicBool, Ordering},
        mpsc,
    },
    thread,
    time::{Duration, Instant},
};

const MAX_BYTES: usize = 32 * 1024 * 1024;
const MAX_ERROR: usize = 64 * 1024;
const MAX_PEERS: usize = 32;
const CAPABILITY: &str = "mobile-peer ABI 1";
const IDENTITY_HELPER: &str = "~/.local/bin/hgs swarm __mobile-peer-identity --json";
const LOCAL_HELPER: &str = "~/.local/bin/hgs swarm __mobile-peer-local --json";
static TRANSPORT_STOP: AtomicBool = AtomicBool::new(false);

#[derive(Clone, Copy, Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
enum Helper {
    Help,
    Identity,
    Local,
    OwnIdentity,
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct TransportRequest {
    schema: u32,
    via: String,
    helper: Helper,
    request: Option<LocalRequest>,
    parent_pid: u32,
    deadline_ms: u64,
}

fn monotonic_ms() -> Result<u64> {
    let mut time = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    ensure!(
        unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut time) } == 0,
        "mobile transport clock unavailable"
    );
    Ok(time.tv_sec as u64 * 1000 + time.tv_nsec as u64 / 1_000_000)
}

extern "C" fn stop_transport(_: libc::c_int) {
    TRANSPORT_STOP.store(true, Ordering::Relaxed);
}

enum CaptureMode {
    Supervisor,
    OwnedSsh { parent_pid: u32, deadline_ms: u64 },
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct LocalRequest {
    schema: u32,
    target_machine_id: String,
    argv: Vec<String>,
    payload: Option<Value>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct PeerRequest {
    schema: u32,
    via: String,
    target_machine_id: String,
    argv: Vec<String>,
    payload: Option<Value>,
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Identity {
    schema: u32,
    machine_id: String,
    name: String,
}

fn uuid(value: &str) -> bool {
    uuid::Uuid::parse_str(value).is_ok_and(|id| !id.is_nil() && id.to_string() == value)
}

fn text(value: &str, max: usize, empty: bool) -> bool {
    (empty || !value.is_empty()) && value.len() <= max && !value.chars().any(char::is_control)
}

fn alias(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 80
        && !value.starts_with('-')
        && value
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b"_.-".contains(&b))
}

fn direct(config: &Config, via: &str) -> bool {
    alias(via)
        && !config.is_self(via)
        && config.peers.iter().any(|p| p == via)
        && config.machines.get(via).is_none_or(|m| m.enabled)
}

fn input<T: serde::de::DeserializeOwned>() -> Result<T> {
    let mut bytes = Vec::new();
    std::io::stdin()
        .take((MAX_BYTES + 1) as u64)
        .read_to_end(&mut bytes)?;
    ensure!(bytes.len() <= MAX_BYTES, "mobile request is too large");
    serde_json::from_slice(&bytes).context("invalid mobile request")
}

fn options<'a>(
    args: &'a [String],
    switches: &[&str],
    values: &[&str],
) -> Result<BTreeMap<&'a str, &'a str>> {
    let mut result = BTreeMap::new();
    let mut index = 0;
    while index < args.len() {
        let flag = args[index].as_str();
        ensure!(!result.contains_key(flag), "duplicate native option");
        if switches.contains(&flag) {
            result.insert(flag, "");
            index += 1;
        } else {
            ensure!(values.contains(&flag), "native option is not allowed");
            let value = args.get(index + 1).context("missing native option value")?;
            ensure!(
                text(value, 4096, flag == "--conversation")
                    && !value.starts_with('-')
                    && !value.starts_with('@'),
                "invalid native option value"
            );
            result.insert(flag, value.as_str());
            index += 2;
        }
    }
    Ok(result)
}

fn scoped(payload: &Option<Value>) -> Result<()> {
    let value = payload
        .as_ref()
        .context("scoped native payload is required")?;
    ensure!(value.is_object(), "native payload must be an object");
    ensure!(
        value
            .get("request_id")
            .and_then(Value::as_str)
            .is_some_and(uuid),
        "native request UUID is required"
    );
    ensure!(
        value
            .get("expected_run_id")
            .and_then(Value::as_str)
            .is_some_and(|v| text(v, 128, false)),
        "native run identity is required"
    );
    ensure!(
        value
            .get("expected_conversation_id")
            .and_then(Value::as_str)
            .is_some_and(|v| text(v, 256, true)),
        "native conversation identity is required"
    );
    Ok(())
}

/// C1 stdin: exactly the run/conversation identity and two nullable marks.
fn read_mark(payload: &Option<Value>) -> Result<()> {
    let value = payload.as_ref().context("read mark payload is required")?;
    let object = value.as_object().context("native payload must be an object")?;
    ensure!(
        object.len() == 4
            && ["run_id", "conversation_id", "reply_id", "attention_signature"]
                .iter()
                .all(|key| object.contains_key(*key)),
        "invalid read mark fields"
    );
    ensure!(
        value["run_id"].as_str().is_some_and(|v| text(v, 128, false))
            && value["conversation_id"].as_str().is_some_and(|v| text(v, 256, false)),
        "native run and conversation identity are required"
    );
    for key in ["reply_id", "attention_signature"] {
        ensure!(
            value[key].is_null() || value[key].as_str().is_some_and(|v| text(v, 256, false)),
            "invalid read mark value"
        );
    }
    Ok(())
}

/// Validate complete argv shapes before calling the CLI's permissive legacy dispatcher.
/// Session operations retain their native scoped-payload validation and receipts.
fn validate(request: &LocalRequest) -> Result<()> {
    ensure!(
        request.schema == 1 && uuid(&request.target_machine_id),
        "invalid mobile target"
    );
    let args = &request.argv;
    ensure!(!args.is_empty() && args.len() <= 24, "invalid native argv");
    ensure!(
        args.iter().all(|v| text(v, 4096, true)),
        "invalid native argv"
    );
    ensure!(
        request.payload.as_ref().is_none_or(Value::is_object),
        "native payload must be an object"
    );
    let op = args[0].as_str();
    let rest = &args[1..];
    let no_payload = || -> Result<()> {
        ensure!(
            request.payload.is_none(),
            "this native operation has no payload"
        );
        Ok(())
    };
    match op {
        "--help" => {
            ensure!(rest.is_empty(), "invalid help request");
            no_payload()?;
        }
        "ls" => {
            let flags = options(rest, &["--json", "--local"], &[])?;
            ensure!(flags.len() == 2, "snapshot must be local JSON");
            no_payload()?;
        }
        "swarm" => {
            if rest == ["assign-launch", "--json"] {
                scoped(&request.payload)?;
                ensure!(
                    request
                        .payload
                        .as_ref()
                        .and_then(|v| v["name"].as_str())
                        .is_some_and(session),
                    "invalid native session"
                );
                // The local native ABI validates project/swarm/folder identity,
                // exact launch UUID and its durable metadata receipt as one action.
            } else if rest.len() == 5
                && rest[0] == "preference"
                && rest[1] == "set"
                && rest[2] == "mobile_delivery"
                && crate::swarm::MOBILE_DELIVERY.contains(&rest[3].as_str())
                && rest[4] == "--json"
            {
                // The gateway connector changes the swarm-wide delivery choice.
                no_payload()?;
            } else {
                ensure!(
                    rest.len() == 1 && matches!(rest[0].as_str(), "get" | "hello"),
                    "swarm operation is not allowed"
                );
                no_payload()?;
            }
        }
        "account" => {
            ensure!(
                rest == ["ls"]
                    || (rest.len() == 2
                        && rest[0] == "inspect"
                        && !rest[1].starts_with(['-', '@'])
                        && !rest[1].is_empty())
                    || (rest.len() == 3
                        && rest[0] == "inspect"
                        && rest[1] == "--session"
                        && session(&rest[2])),
                "account operation is not allowed"
            );
            no_payload()?;
        }
        "dirs" => {
            ensure!(
                rest.len() == 1 && session(&rest[0]),
                "invalid directory request"
            );
            no_payload()?;
        }
        "worktrees" => {
            no_payload()?;
            let create = rest.first().is_some_and(|v| v == "create");
            let values = if create {
                options(
                    &rest[1..],
                    &["--json", "--mobile"],
                    &[
                        "--path",
                        "--common-dir",
                        "--destination",
                        "--branch",
                        "--base",
                        "--request-id",
                    ],
                )?
            } else {
                options(rest, &["--json", "--refresh"], &["--path"])?
            };
            ensure!(
                values.len() == if create { 8 } else { 3 },
                "incomplete worktree request"
            );
            for field in if create {
                vec!["--path", "--common-dir", "--destination"]
            } else {
                vec!["--path"]
            } {
                ensure!(
                    values
                        .get(field)
                        .is_some_and(|v| std::path::Path::new(v).is_absolute()),
                    "worktree path must be absolute"
                );
            }
            if create {
                ensure!(
                    values.contains_key("--mobile"),
                    "mobile creation deadline is required"
                );
                ensure!(
                    values.get("--request-id").is_some_and(|v| uuid(v)),
                    "invalid worktree request UUID"
                );
                for field in ["--branch", "--base"] {
                    ensure!(
                        values
                            .get(field)
                            .is_some_and(|v| text(v, 256, false) && !v.starts_with('-')),
                        "invalid worktree branch or revision"
                    );
                }
            }
        }
        "inspect" => {
            ensure!(
                rest.first().is_some_and(|v| session(v)),
                "invalid native session"
            );
            let flags = options(
                &rest[1..],
                &["--skip-processes"],
                &["--archive", "--agent", "--after"],
            )?;
            if let Some(after) = flags.get("--after") {
                ensure!(after.parse::<u64>().is_ok(), "invalid history cursor");
            }
            if let Some(id) = flags.get("--archive") {
                ensure!(uuid(id), "invalid archive UUID");
            }
            no_payload()?;
        }
        "recovery" => {
            ensure!(
                rest == ["action", "--scoped-json"],
                "recovery operation is not allowed"
            );
            scoped(&request.payload)?;
            ensure!(
                request
                    .payload
                    .as_ref()
                    .and_then(|v| v["name"].as_str())
                    .is_some_and(session),
                "invalid native session"
            );
        }
        "read" => {
            ensure!(
                rest.len() == 2 && session(&rest[0]) && rest[1] == "--json",
                "native read requires scoped JSON"
            );
            read_mark(&request.payload)?;
        }
        "history" | "terminal" | "session-action" | "send" | "send-now" | "settings" | "answer"
        | "interrupt" | "clear-context" | "compact-context" | "effort" => {
            ensure!(
                rest.len() == 2 && session(&rest[0]) && rest[1] == "--json",
                "native operation requires scoped JSON"
            );
            scoped(&request.payload)?;
        }
        "processes" => {
            ensure!(
                rest.first().is_some_and(|v| session(v)),
                "invalid native session"
            );
            let flags = options(
                &rest[1..],
                &[],
                &[
                    "--output",
                    "--stop",
                    "--run",
                    "--conversation",
                    "--generation",
                    "--archive",
                ],
            )?;
            ensure!(
                flags.contains_key("--output") != flags.contains_key("--stop"),
                "choose one native process action"
            );
            ensure!(
                flags.contains_key("--run") && flags.contains_key("--conversation"),
                "native process identity is required"
            );
            if let Some(id) = flags.get("--archive") {
                ensure!(uuid(id), "invalid archive UUID");
            }
            no_payload()?;
        }
        "attachment" => {
            ensure!(
                rest.first().is_some_and(|v| session(v)),
                "invalid native session"
            );
            if rest.len() == 3 && rest[1] == "--stage" && rest[2] == "--json" {
                scoped(&request.payload)?;
            } else {
                let flags = options(
                    &rest[1..],
                    &[],
                    &[
                        "--request",
                        "--index",
                        "--conversation",
                        "--archive",
                        "--agent",
                    ],
                )?;
                ensure!(
                    flags.get("--request").is_some_and(|v| uuid(v))
                        && flags
                            .get("--index")
                            .is_some_and(|v| v.parse::<u8>().is_ok_and(|n| n < 8))
                        && flags.contains_key("--conversation"),
                    "invalid attachment identity"
                );
                if let Some(id) = flags.get("--archive") {
                    ensure!(uuid(id), "invalid archive UUID");
                }
                no_payload()?;
            }
        }
        "claude" | "codex" | "kimi" | "dsh" => {
            ensure!(
                rest.len() == 7 || rest.len() == 9,
                "native launch shape is not allowed"
            );
            ensure!(
                session(&rest[0])
                    && rest[1] == "--new"
                    && rest[2] == "-n"
                    && session(&rest[3])
                    && rest[4] == "-d"
                    && rest[5] == "--launch-id"
                    && uuid(&rest[6]),
                "native launch shape is not allowed"
            );
            if rest.len() == 9 {
                ensure!(
                    rest[7] == "--account" && session(&rest[8]),
                    "native launch account is invalid"
                );
            }
            no_payload()?;
        }
        _ => bail!("native operation is not allowed"),
    }
    Ok(())
}

fn session(value: &str) -> bool {
    text(value, 4096, false) && !value.starts_with(['-', '@'])
}

struct Output {
    code: i32,
    stdout: Vec<u8>,
    stderr: Vec<u8>,
}

fn nonblocking(fd: i32) -> Result<()> {
    let flags = unsafe { libc::fcntl(fd, libc::F_GETFL) };
    ensure!(
        flags >= 0 && unsafe { libc::fcntl(fd, libc::F_SETFL, flags | libc::O_NONBLOCK) } >= 0,
        "cannot prepare mobile transport"
    );
    Ok(())
}

fn read_pipe<T: Read>(pipe: &mut Option<T>, bytes: &mut Vec<u8>, cap: usize) -> Result<()> {
    let Some(reader) = pipe.as_mut() else {
        return Ok(());
    };
    let mut buffer = [0; 16 * 1024];
    // A bounded turn prevents a continuously writing child starving the deadline.
    for _ in 0..16 {
        match reader.read(&mut buffer) {
            Ok(0) => {
                *pipe = None;
                break;
            }
            Ok(n) => {
                ensure!(bytes.len() + n <= cap, "mobile output is too large");
                bytes.extend_from_slice(&buffer[..n]);
            }
            Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => break,
            Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(e.into()),
        }
    }
    Ok(())
}

/// No reader threads or blocking joins: inherited pipe descriptors cannot extend
/// the whole deadline. Only this owned helper/SSH child is stopped on failure.
fn capture_control(
    mut command: Command,
    input: &[u8],
    deadline: Instant,
    cap: usize,
    mode: CaptureMode,
) -> Result<Output> {
    ensure!(Instant::now() < deadline, "mobile helper timed out");
    if let CaptureMode::OwnedSsh {
        parent_pid,
        deadline_ms,
    } = mode
    {
        ensure!(
            unsafe { libc::getppid() } as u32 == parent_pid
                && !TRANSPORT_STOP.load(Ordering::Relaxed)
                && monotonic_ms()? < deadline_ms,
            "mobile transport parent changed"
        );
    }
    command
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    let mut child = command.spawn().context("mobile helper unavailable")?;
    let result = (|| {
        let mut stdin = child.stdin.take();
        let mut stdout = child.stdout.take();
        let mut stderr = child.stderr.take();
        nonblocking(stdin.as_ref().unwrap().as_raw_fd())?;
        nonblocking(stdout.as_ref().unwrap().as_raw_fd())?;
        nonblocking(stderr.as_ref().unwrap().as_raw_fd())?;
        let mut sent = 0;
        let mut out = Vec::new();
        let mut err = Vec::new();
        let mut status = None;
        loop {
            if let CaptureMode::OwnedSsh {
                parent_pid,
                deadline_ms,
            } = mode
            {
                ensure!(
                    !TRANSPORT_STOP.load(Ordering::Relaxed)
                        && unsafe { libc::getppid() } as u32 == parent_pid,
                    "mobile transport cancelled; delivery may be uncertain"
                );
                ensure!(
                    monotonic_ms()? < deadline_ms,
                    "mobile helper timed out; delivery may be uncertain"
                );
            }
            ensure!(
                Instant::now() < deadline,
                "mobile helper timed out; delivery may be uncertain"
            );
            if sent == input.len() {
                stdin = None;
            }
            if let Some(writer) = stdin.as_mut() {
                match writer.write(&input[sent..input.len().min(sent + 65536)]) {
                    Ok(n) => sent += n,
                    Err(e)
                        if e.kind() == std::io::ErrorKind::WouldBlock
                            || e.kind() == std::io::ErrorKind::Interrupted => {}
                    Err(e) if e.kind() == std::io::ErrorKind::BrokenPipe => {
                        stdin = None;
                    }
                    Err(e) => return Err(e.into()),
                }
            }
            read_pipe(&mut stdout, &mut out, cap)?;
            read_pipe(&mut stderr, &mut err, MAX_ERROR)?;
            if status.is_none() {
                status = child.try_wait()?;
            }
            if status.is_some() && stdout.is_none() && stderr.is_none() {
                return Ok(Output {
                    code: platform::code(status.unwrap()),
                    stdout: out,
                    stderr: err,
                });
            }
            thread::sleep(Duration::from_millis(5));
        }
    })();
    if result.is_err() {
        if matches!(mode, CaptureMode::Supervisor) {
            // The supervisor must reap its SSH child before it exits. Killing
            // the supervisor first would destroy that cancellation watchdog.
            unsafe {
                libc::kill(child.id() as i32, libc::SIGTERM);
            }
            let grace = Instant::now() + Duration::from_secs(1);
            while Instant::now() < grace {
                if child.try_wait().ok().flatten().is_some() {
                    return result;
                }
                thread::sleep(Duration::from_millis(5));
            }
        }
        let _ = child.kill();
        let _ = child.wait();
    }
    result
}

fn transport(
    via: &str,
    helper: Helper,
    request: Option<LocalRequest>,
    deadline: Instant,
    cap: usize,
) -> Result<Output> {
    let remaining = deadline
        .saturating_duration_since(Instant::now())
        .as_millis();
    ensure!(
        remaining > 0 && remaining <= 30_000,
        "invalid mobile transport deadline"
    );
    let envelope = TransportRequest {
        schema: 1,
        via: via.to_owned(),
        helper,
        request,
        parent_pid: std::process::id(),
        deadline_ms: monotonic_ms()? + remaining as u64,
    };
    let bytes = serde_json::to_vec(&envelope)?;
    ensure!(bytes.len() <= MAX_BYTES, "mobile request is too large");
    let mut command = Command::new(std::env::current_exe()?);
    command.args(["swarm", "__mobile-peer-transport", "--json"]);
    capture_control(command, &bytes, deadline, cap, CaptureMode::Supervisor)
}

fn supervise() -> Result<i32> {
    // This helper performs no native operations itself. It owns exactly one
    // SSH/identity child and survives cancellation of the gateway wrapper.
    unsafe {
        libc::signal(
            libc::SIGTERM,
            stop_transport as *const () as libc::sighandler_t,
        );
    }
    let envelope: TransportRequest = input()?;
    let now = monotonic_ms()?;
    ensure!(
        envelope.schema == 1
            && envelope.parent_pid > 1
            && unsafe { libc::getppid() } as u32 == envelope.parent_pid
            && !TRANSPORT_STOP.load(Ordering::Relaxed),
        "mobile transport parent changed"
    );
    ensure!(
        envelope.deadline_ms > now && envelope.deadline_ms - now <= 30_000,
        "invalid mobile transport deadline"
    );
    let config = Config::load()?;
    let (command, bytes, cap) = match envelope.helper {
        Helper::OwnIdentity => {
            ensure!(
                envelope.via.is_empty() && envelope.request.is_none(),
                "invalid local identity transport"
            );
            let mut command = Command::new(std::env::current_exe()?);
            command.args(["swarm", "__mobile-peer-identity", "--json"]);
            (command, vec![], 4096)
        }
        Helper::Help | Helper::Identity => {
            ensure!(
                direct(&config, &envelope.via) && envelope.request.is_none(),
                "invalid mobile transport route"
            );
            let (helper, cap) = if matches!(envelope.helper, Helper::Help) {
                ("~/.local/bin/hgs --help", 256 * 1024)
            } else {
                (IDENTITY_HELPER, 4096)
            };
            (ssh(&config, &envelope.via, helper), vec![], cap)
        }
        Helper::Local => {
            ensure!(
                direct(&config, &envelope.via),
                "invalid mobile transport route"
            );
            let request = envelope
                .request
                .context("missing mobile transport request")?;
            validate(&request)?;
            (
                ssh(&config, &envelope.via, LOCAL_HELPER),
                serde_json::to_vec(&request)?,
                MAX_BYTES,
            )
        }
    };
    ensure!(
        unsafe { libc::getppid() } as u32 == envelope.parent_pid
            && !TRANSPORT_STOP.load(Ordering::Relaxed),
        "mobile transport parent changed"
    );
    let remaining = envelope.deadline_ms.saturating_sub(monotonic_ms()?);
    let output = capture_control(
        command,
        &bytes,
        Instant::now() + Duration::from_millis(remaining),
        cap,
        CaptureMode::OwnedSsh {
            parent_pid: envelope.parent_pid,
            deadline_ms: envelope.deadline_ms,
        },
    )?;
    std::io::stdout().write_all(&output.stdout)?;
    std::io::stderr().write_all(&output.stderr)?;
    Ok(output.code)
}

fn ssh(config: &Config, via: &str, helper: &str) -> Command {
    let mut command = Command::new("ssh");
    command.args([
        "-T",
        "-o",
        "BatchMode=yes",
        "-o",
        "ConnectTimeout=3",
        "-o",
        "ServerAliveInterval=5",
        "-o",
        "ServerAliveCountMax=1",
    ]);
    command.args(machines::ssh_options(config, via));
    command.arg(via).arg(helper);
    command
}

fn supported(via: &str, deadline: Instant) -> Result<()> {
    let output = transport(
        via,
        Helper::Help,
        None,
        deadline.min(Instant::now() + Duration::from_secs(3)),
        256 * 1024,
    )?;
    ensure!(
        output.code == 0 && String::from_utf8_lossy(&output.stdout).contains(CAPABILITY),
        "mobile peer upgrade required"
    );
    Ok(())
}

fn own_identity(config: &Config) -> Result<Identity> {
    ensure!(text(config.host(), 256, false), "invalid machine name");
    Ok(Identity {
        schema: 1,
        machine_id: swarm::local_node_id(config)?,
        name: config.host().to_owned(),
    })
}

fn identity(output: Output) -> Result<Identity> {
    ensure!(output.code == 0, "mobile peer unavailable");
    let identity: Identity = serde_json::from_slice(&output.stdout)?;
    ensure!(
        identity.schema == 1 && uuid(&identity.machine_id) && text(&identity.name, 256, false),
        "invalid mobile peer identity"
    );
    Ok(identity)
}

fn discovery(config: &Config) -> Result<Value> {
    let deadline = Instant::now() + Duration::from_secs(10);
    let local = identity(transport(
        "",
        Helper::OwnIdentity,
        None,
        deadline.min(Instant::now() + Duration::from_secs(3)),
        4096,
    )?)?;
    let peers: BTreeSet<_> = config
        .peers
        .iter()
        .filter(|via| direct(config, via))
        .cloned()
        .collect();
    ensure!(
        peers.len() <= MAX_PEERS,
        "too many direct mobile peers (maximum 32)"
    );
    let peers: Vec<_> = peers.into_iter().collect();
    let mut rows = Vec::new();
    // Four owned worker threads, each bounded by the shared discovery deadline.
    thread::scope(|scope| {
        let (sender, receiver) = mpsc::channel();
        for worker in 0..4 {
            let sender = sender.clone();
            let peers = &peers;
            scope.spawn(move || {
                for via in peers.iter().skip(worker).step_by(4) {
                    let outcome = (|| {
                        supported(via, deadline)?;
                        identity(transport(via, Helper::Identity, None, deadline.min(Instant::now() + Duration::from_secs(3)), 4096)?)
                    })();
                    let row = match outcome {
                        Ok(id) => json!({"via":via,"machine_id":id.machine_id,"name":id.name,"online":true,"error":null}),
                        Err(_) => json!({"via":via,"machine_id":null,"name":via,"online":false,"error":"unavailable"}),
                    };
                    if sender.send(row).is_err() { return; }
                }
            });
        }
        drop(sender);
        rows.extend(receiver);
    });
    rows.sort_by(|a, b| a["via"].as_str().cmp(&b["via"].as_str()));
    Ok(json!({"schema":1,"local":{"machine_id":local.machine_id,"name":local.name},"peers":rows}))
}

fn local(config: &Config, request: LocalRequest) -> Result<i32> {
    validate(&request)?;
    ensure!(
        swarm::local_node_id(config)? == request.target_machine_id,
        "mobile target identity changed; refresh computers"
    );
    // Rebind stdin to a private anonymous file, then invoke the existing native
    // dispatcher in this same process. No shell, routing, or second hop exists.
    let mut input = tempfile::tempfile()?;
    if let Some(payload) = request.payload {
        serde_json::to_writer(&mut input, &payload)?;
    }
    input.rewind()?;
    ensure!(
        unsafe { libc::dup2(input.as_raw_fd(), libc::STDIN_FILENO) } >= 0,
        "cannot prepare native input"
    );
    cli::dispatch(request.argv).map_err(anyhow::Error::new)
}

fn execute(config: &Config, args: &[String]) -> Result<i32> {
    ensure!(
        args.len() == 2 && args[1] == "--json",
        "mobile commands require --json"
    );
    match args[0].as_str() {
        "__mobile-peer-identity" => {
            println!("{}", serde_json::to_string(&own_identity(config)?)?);
            Ok(0)
        }
        "mobile-peers" => {
            println!("{}", discovery(config)?);
            Ok(0)
        }
        "__mobile-peer-local" => local(config, input()?),
        "__mobile-peer-transport" => supervise(),
        "mobile-peer" => {
            let request: PeerRequest = input()?;
            let config = &Config::load()?;
            ensure!(
                direct(config, &request.via),
                "mobile route is not an enabled direct peer"
            );
            let local = LocalRequest {
                schema: request.schema,
                target_machine_id: request.target_machine_id,
                argv: request.argv,
                payload: request.payload,
            };
            validate(&local)?;
            let deadline = Instant::now() + Duration::from_secs(30);
            supported(&request.via, deadline)?;
            // A capability probe may take seconds; a disabled/reconfigured route
            // must not be authorized by the earlier snapshot of local settings.
            let config = &Config::load()?;
            ensure!(
                direct(config, &request.via),
                "mobile route is not an enabled direct peer"
            );
            let output = transport(
                &request.via,
                Helper::Local,
                Some(local),
                deadline,
                MAX_BYTES,
            )?;
            std::io::stdout().write_all(&output.stdout)?;
            std::io::stderr().write_all(&output.stderr)?;
            Ok(output.code)
        }
        _ => bail!("unknown mobile command"),
    }
}

pub(crate) fn dispatch(config: &Config, args: &[String]) -> cli::Result<i32> {
    execute(config, args).map_err(|e| cli::Error::new(1, format!("{e:#}")))
}

#[cfg(test)]
mod tests {
    use super::*;
    const ID: &str = "11111111-1111-4111-8111-111111111111";

    fn request(argv: &[&str], payload: Option<Value>) -> LocalRequest {
        LocalRequest {
            schema: 1,
            target_machine_id: ID.into(),
            argv: argv.iter().map(|v| (*v).into()).collect(),
            payload,
        }
    }

    fn scope() -> Value {
        json!({"request_id":ID,"expected_run_id":"run","expected_conversation_id":"conversation"})
    }

    #[test]
    fn supports_existing_connector_shapes_without_global_dispatch() {
        for argv in [
            vec!["--help"],
            vec!["ls", "--json", "--local"],
            vec!["swarm", "get"],
            vec!["swarm", "hello"],
            vec!["account", "ls"],
            vec!["account", "inspect", "native-codex"],
            vec!["account", "inspect", "--session", "codex/project"],
            vec!["dirs", "/fixture/ordinary folder"],
            vec![
                "inspect",
                "codex/project",
                "--after",
                "2",
                "--agent",
                "child",
                "--skip-processes",
            ],
            vec![
                "processes",
                "codex/project",
                "--output",
                "process",
                "--run",
                "run",
                "--conversation",
                "conversation",
            ],
            vec![
                "processes",
                "codex/project",
                "--stop",
                "process",
                "--run",
                "run",
                "--conversation",
                "",
                "--generation",
                "generation",
            ],
            vec![
                "attachment",
                "codex/project",
                "--request",
                ID,
                "--index",
                "0",
                "--conversation",
                "conversation",
            ],
            vec![
                "codex",
                "/fixture/project",
                "--new",
                "-n",
                "fixture",
                "-d",
                "--launch-id",
                ID,
            ],
            vec![
                "kimi",
                "/fixture/project",
                "--new",
                "-n",
                "fixture",
                "-d",
                "--launch-id",
                ID,
                "--account",
                "fixture-account",
            ],
        ] {
            assert!(validate(&request(&argv, None)).is_ok(), "{argv:?}");
        }
        for op in [
            "history",
            "terminal",
            "session-action",
            "send",
            "send-now",
            "settings",
            "answer",
            "interrupt",
            "clear-context",
            "compact-context",
            "effort",
        ] {
            assert!(
                validate(&request(&[op, "codex/project", "--json"], Some(scope()))).is_ok(),
                "{op}"
            );
            assert!(
                validate(&request(&[op, "codex/project", "--json"], None)).is_err(),
                "{op}"
            );
        }
        assert!(validate(&request(
            &["attachment", "codex/project", "--stage", "--json"],
            Some(scope())
        ))
        .is_ok());
        let mut recovery = scope();
        recovery["name"] = json!("codex/project");
        assert!(validate(&request(
            &["recovery", "action", "--scoped-json"],
            Some(recovery)
        ))
        .is_ok());
        let mut assignment = scope();
        assignment["name"] = json!("codex/project");
        assert!(validate(&request(
            &["swarm", "assign-launch", "--json"],
            Some(assignment)
        ))
        .is_ok());
        assert!(validate(&request(&["swarm", "assign-launch", "--json"], None)).is_err());
    }

    #[test]
    fn shared_delivery_setting_accepts_only_exact_known_values() {
        let set = ["swarm", "preference", "set", "mobile_delivery", "away_10", "--json"];
        assert!(validate(&request(&set, None)).is_ok());
        assert!(validate(&request(&set, Some(json!({})))).is_err());
        for argv in [
            vec!["swarm", "preference", "set", "mobile_delivery", "later", "--json"],
            vec!["swarm", "preference", "set", "theme", "away", "--json"],
            vec!["swarm", "preference", "set", "mobile_delivery", "away"],
            vec!["swarm", "preference", "get", "--json"],
        ] {
            assert!(validate(&request(&argv, None)).is_err(), "{argv:?}");
        }
    }

    #[test]
    fn worktree_operations_are_exact_and_creation_is_bounded() {
        let read = ["worktrees", "--path", "/repo", "--refresh", "--json"];
        assert!(validate(&request(&read, None)).is_ok());
        let create = [
            "worktrees",
            "create",
            "--path",
            "/repo",
            "--common-dir",
            "/repo/.git",
            "--destination",
            "/linked",
            "--branch",
            "feature",
            "--base",
            "HEAD",
            "--request-id",
            ID,
            "--json",
            "--mobile",
        ];
        assert!(validate(&request(&create, None)).is_ok());
        assert!(validate(&request(&create[..create.len() - 1], None)).is_err());
        let mut extra = create.to_vec();
        extra.extend(["--shell", "bad"]);
        assert!(validate(&request(&extra, None)).is_err());
        assert!(validate(&request(&read, Some(scope()))).is_err());
    }

    #[test]
    fn denies_forwarding_secrets_shell_and_unscoped_mutations() {
        for argv in [
            vec!["@peer", "send", "codex/project", "--json"],
            vec!["--run", "sh"],
            vec!["sh", "-c", "echo fixture"],
            vec!["__state", "run", "sh"],
            vec!["mobile-peer", "--json"],
            vec!["__mobile-peer-local", "--json"],
            vec!["ls", "--json"],
            vec!["ls", "--json", "--local", "--json"],
            vec!["swarm", "inventory"],
            vec!["swarm", "sync"],
            vec!["swarm", "export"],
            vec!["account", "export", "fixture"],
            vec!["account", "copy", "fixture", "peer"],
            vec!["account", "inspect", "fixture", "--refresh"],
            vec!["send", "codex/project", "text"],
            vec!["processes", "codex/project", "--stop", "process"],
            vec!["inspect", "@peer"],
            vec!["inspect", "codex/project", "--dry-run"],
            vec![
                "codex",
                "/fixture/project",
                "--new",
                "-n",
                "fixture",
                "-d",
                "--launch-id",
                ID,
                "--",
                "--dangerously-bypass-approvals-and-sandbox",
            ],
            vec![
                "attachment",
                "codex/project",
                "--request",
                ID,
                "--index",
                "8",
                "--conversation",
                "conversation",
            ],
        ] {
            assert!(validate(&request(&argv, None)).is_err(), "{argv:?}");
        }
        for field in ["via", "path", "depth", "command", "expected_machine_id"] {
            let mut value = serde_json::to_value(request(&["--help"], None)).unwrap();
            value[field] = json!("fixture");
            assert!(
                serde_json::from_value::<LocalRequest>(value).is_err(),
                "{field}"
            );
        }
        for route in [
            "-oProxyCommand=fixture",
            "peer;fixture",
            "user@peer",
            "peer/child",
            "peer\nchild",
            "",
        ] {
            assert!(!alias(route));
        }
    }

    #[test]
    fn read_marks_require_an_exact_scoped_payload() {
        let mark = json!({"run_id":"run","conversation_id":"conversation","reply_id":null,"attention_signature":"a".repeat(64)});
        assert!(validate(&request(&["read", "codex/project", "--json"], Some(mark.clone()))).is_ok());
        assert!(validate(&request(&["read", "codex/project", "--json"], None)).is_err());
        assert!(validate(&request(&["read", "@peer", "--json"], Some(mark.clone()))).is_err());
        assert!(validate(&request(&["read", "codex/project"], Some(mark.clone()))).is_err());
        let mut extra = mark.clone();
        extra["request_id"] = json!(ID);
        assert!(validate(&request(&["read", "codex/project", "--json"], Some(extra))).is_err());
        let mut empty = mark;
        empty["run_id"] = json!("");
        assert!(validate(&request(&["read", "codex/project", "--json"], Some(empty))).is_err());
    }
}
