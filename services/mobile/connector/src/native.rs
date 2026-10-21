//! Native `hgs` calls, as `Connector._native` makes them
//! (`connector.py:344-460`): `hgs_path` from the configuration with PATH
//! lookup, the connector's environment minus exactly the calling agent's
//! identity (`CONTEXT_KEYS`), stdin always a pipe that carries the JSON
//! payload, stdout and stderr each capped (stderr is read and discarded), a
//! per-call timeout, and Python's exit-status and JSON rules. A timeout, an
//! oversized output or a dropped call SIGKILLs only the direct child, never
//! its process group, a tmux server or an agent, then drains its pipes for
//! 0.5 s without retaining anything.
use crate::request;
use serde_json::Value;
use std::future::Future;
use std::process::Stdio;
use std::time::Duration;
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWriteExt};
use tokio::process::{Child, Command};
use zerus_relay::json;

/// The calling agent's identity, which native hgs never receives
/// (`connector.py:46-52`). Everything else, HGS_CONFIG_DIR and HGS_PEERS
/// included, passes on.
pub const CONTEXT_KEYS: [&str; 23] = [
    "TMUX",
    "TMUX_PANE",
    "HGS_SESSION",
    "HGS_RUN_ID",
    "HGS_EXPECTED_ID",
    "HGS_EXECUTABLE",
    "HGS_AGENT",
    "HGS_FRESH",
    "HGS_FRESH_REPLACES",
    "HGS_REQUESTED_ID",
    "HGS_ARCHIVE_ID",
    "HGS_LAUNCH_ID",
    "HGS_FORK_PARENT_ID",
    "HGS_ACCOUNT_ID",
    "HGS_CLIENT",
    "CLAUDE_CONFIG_DIR",
    "CODEX_HOME",
    "KIMI_CODE_HOME",
    "DSH_HOME",
    "DSH_CONFIG_DIR",
    "OPENCODE_CONFIG_DIR",
    "OPENCODE_CONFIG",
    "OPENCODE_CONFIG_CONTENT",
];

/// Each of stdout and stderr (`MAX_BYTES`).
pub const MAX_BYTES: usize = 8 * 1024 * 1024;
/// Native stdin, as the relay's request limit (`MAX_REQUEST_BYTES`).
pub const MAX_PAYLOAD: usize = 30_408_704;
/// Python's `subprocess_timeout` default.
pub const DEFAULT_TIMEOUT: f64 = 45.0;
/// How long a killed child's pipes are drained before they are closed.
pub const DRAIN: Duration = Duration::from_millis(500);
/// A `worktrees create --mobile` supervisor is never cut short before this.
pub const MOBILE_CREATE_DRAIN: Duration = Duration::from_secs(35);

pub const COULD_NOT_START: &str = "native command could not start";
pub const FAILED: &str = "native command failed";
pub const INVALID_JSON: &str = "native command returned invalid JSON";
pub const TIMED_OUT: &str = "native command timed out";
pub const OUTPUT_TOO_LARGE: &str = "native output exceeds size limit";
pub const REQUEST_TOO_LARGE: &str = "native request exceeds size limit";

/// One native call: `Connector.native(argv, payload, timeout=...,
/// json_output=..., failure_json=..., max_output_bytes=...)`.
#[derive(Debug, Clone, PartialEq)]
pub struct Call {
    pub argv: Vec<String>,
    pub payload: Option<Value>,
    /// Seconds; `None` is the connector's default.
    pub timeout: Option<f64>,
    pub json_output: bool,
    pub failure_json: bool,
    pub max_output_bytes: Option<usize>,
}

impl Call {
    pub fn new<S: Into<String>>(argv: impl IntoIterator<Item = S>) -> Self {
        Call {
            argv: argv.into_iter().map(Into::into).collect(),
            payload: None,
            timeout: None,
            json_output: true,
            failure_json: false,
            max_output_bytes: None,
        }
    }
    pub fn payload(mut self, payload: Value) -> Self {
        self.payload = Some(payload);
        self
    }
    pub fn timeout(mut self, seconds: f64) -> Self {
        self.timeout = Some(seconds);
        self
    }
    pub fn text(mut self) -> Self {
        self.json_output = false;
        self
    }
    pub fn failure_json(mut self) -> Self {
        self.failure_json = true;
        self
    }
}

#[derive(Debug, Clone, PartialEq)]
pub enum Output {
    Json(Value),
    Text(String),
}

impl Output {
    pub fn json(self) -> Value {
        match self {
            Output::Json(value) => value,
            Output::Text(text) => Value::String(text),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum NativeError {
    /// Python's ConnectorError with one of the fixed texts above.
    Failed(&'static str),
    /// Text output that is not UTF-8: Python's UnicodeError, which no
    /// ConnectorError handler catches.
    Unicode,
    /// The connector is stopping: no new native call starts (divergence h).
    Stopping,
}

pub type NativeResult = Result<Output, NativeError>;

/// The native seam: a process in production, a script in unit tests.
#[allow(async_fn_in_trait)]
pub trait Native {
    async fn call(&self, call: Call) -> NativeResult;
}

/// `Connector.native`'s argv rewriting: worktree catalogs go through the
/// state-only entry point (`__state worktrees ...`).
pub fn native_argv(argv: Vec<String>) -> Vec<String> {
    if argv.first().map(String::as_str) == Some("worktrees") {
        std::iter::once("__state".to_owned()).chain(argv).collect()
    } else {
        argv
    }
}

/// `json.dumps(payload, ensure_ascii=False).encode()`. Keys come out sorted
/// where Python keeps their order; the length is Python's.
pub fn stdin_bytes(payload: &Value) -> Result<Vec<u8>, NativeError> {
    let mut out = Vec::new();
    json::python_default_encode(payload, false, &mut out)
        .map_err(|_| NativeError::Failed(COULD_NOT_START))?;
    Ok(out)
}

fn peer_envelope(argv: &[String]) -> bool {
    matches!(
        argv,
        [first, second, ..] if first == "swarm" && (second == "mobile-peer" || second == "__mobile-peer-local")
    )
}

/// `bytes.strip()`: ASCII whitespace including vertical tab.
fn strip(bytes: &[u8]) -> &[u8] {
    let space = |b: &u8| b" \t\n\r\x0b\x0c".contains(b);
    let start = bytes.iter().position(|b| !space(b)).unwrap_or(bytes.len());
    let end = bytes
        .iter()
        .rposition(|b| !space(b))
        .map_or(start, |end| end + 1);
    &bytes[start..end]
}

/// Native stdout as JSON: Python's `json.loads(bytes)` within the 8 MiB cap,
/// with its 4,300-digit integer limit. A UTF-8 byte order mark is skipped,
/// as Python's encoding detection does.
pub fn parse_output(stdout: &[u8]) -> Result<Value, NativeError> {
    let stdout = stdout.strip_prefix(b"\xef\xbb\xbf").unwrap_or(stdout);
    request::parse(stdout, 1000, usize::MAX).map_err(|_| NativeError::Failed(INVALID_JSON))
}

/// Python's rules for a finished call.
pub fn interpret(call: &Call, success: bool, stdout: Vec<u8>) -> NativeResult {
    // `hgs read` answers an unknown session with JSON and exit status 1.
    let answered = call.failure_json && !strip(&stdout).is_empty();
    if !success && !answered {
        return Err(NativeError::Failed(FAILED));
    }
    if !call.json_output {
        return String::from_utf8(stdout)
            .map(Output::Text)
            .map_err(|_| NativeError::Unicode);
    }
    let result = parse_output(&stdout)?;
    // A failing exit carries only a failure answer, never a success receipt.
    let refusal = result.is_object() && result.get("ok") == Some(&Value::Bool(false));
    if !success && !refusal {
        return Err(NativeError::Failed(FAILED));
    }
    Ok(Output::Json(result))
}

/// Spawning holds this gate on macOS, whose standard library marks pipe
/// descriptors close-on-exec only after creating them; Linux creates them
/// with `pipe2(O_CLOEXEC)` and needs none. Nothing else, DNS above all,
/// runs under it.
static SPAWN_GATE: tokio::sync::Mutex<()> = tokio::sync::Mutex::const_new(());

async fn spawn(command: &mut Command) -> std::io::Result<Child> {
    let _gate = if cfg!(target_os = "macos") {
        Some(SPAWN_GATE.lock().await)
    } else {
        None
    };
    command.spawn()
}

/// Production native calls.
pub struct Process {
    pub hgs: String,
    /// Python's `subprocess_timeout` (45 s; the test hook may change it).
    pub default_timeout: f64,
    pub max_bytes: usize,
}

impl Process {
    pub fn new(hgs: String, default_timeout: f64) -> Self {
        Process {
            hgs,
            default_timeout,
            max_bytes: MAX_BYTES,
        }
    }
}

enum Read {
    Done(Vec<u8>),
    Oversized,
}

/// Reads one pipe to its end under `cap`. Over the cap, an ordinary call
/// stops reading at once; a mobile worktree creation keeps draining its
/// supervisor without retaining the excess.
async fn read_capped(
    mut pipe: impl AsyncRead + Unpin,
    cap: usize,
    keep_draining: bool,
    retain: bool,
) -> std::io::Result<Read> {
    let mut data = Vec::new();
    let mut size = 0usize;
    let mut oversized = false;
    let mut chunk = vec![0u8; 65536];
    loop {
        let read = pipe.read(&mut chunk).await?;
        if read == 0 {
            break;
        }
        size += read;
        if size > cap {
            if !keep_draining {
                return Ok(Read::Oversized);
            }
            oversized = true;
            continue;
        }
        if retain {
            data.extend_from_slice(&chunk[..read]);
        }
    }
    Ok(if oversized {
        Read::Oversized
    } else {
        Read::Done(data)
    })
}

async fn discard(pipe: Option<impl AsyncRead + Unpin>) {
    if let Some(mut pipe) = pipe {
        let mut chunk = vec![0u8; 65536];
        while matches!(pipe.read(&mut chunk).await, Ok(read) if read > 0) {}
    }
}

/// Kills a child that has not exited when the call future is dropped.
struct Owned(Option<Child>);

impl Drop for Owned {
    fn drop(&mut self) {
        if let Some(child) = self.0.as_mut() {
            if matches!(child.try_wait(), Ok(None)) {
                // Only this connector-owned CLI child.
                let _ = child.start_kill();
            }
        }
    }
}

impl Native for Process {
    async fn call(&self, call: Call) -> NativeResult {
        let stdin = match &call.payload {
            None => None,
            Some(payload) => Some(stdin_bytes(payload)?),
        };
        let extra = if peer_envelope(&call.argv) { 4096 } else { 0 };
        if stdin
            .as_ref()
            .is_some_and(|bytes| bytes.len() > MAX_PAYLOAD + extra)
        {
            return Err(NativeError::Failed(REQUEST_TOO_LARGE));
        }
        let inner: Vec<&str> = call
            .payload
            .as_ref()
            .and_then(|payload| payload.get("argv"))
            .and_then(Value::as_array)
            .map(|argv| argv.iter().filter_map(Value::as_str).collect())
            .unwrap_or_else(|| call.argv.iter().map(String::as_str).collect());
        let mobile_create = ["worktrees", "create", "--mobile"]
            .iter()
            .all(|word| inner.contains(word));
        let cap = call.max_output_bytes.unwrap_or(self.max_bytes);
        let timeout = call.timeout.unwrap_or(self.default_timeout);
        let started = tokio::time::Instant::now();

        let mut command = Command::new(&self.hgs);
        command
            .args(&call.argv)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        for key in CONTEXT_KEYS {
            command.env_remove(key);
        }
        #[cfg(windows)]
        {
            command.creation_flags(0x08000000); // CREATE_NO_WINDOW, hgs owns SSH's adapter.
            command
                .env_remove("HGS_HOST_SESSION")
                .env_remove("HGS_HOST_PANE");
        }
        let child = spawn(&mut command)
            .await
            .map_err(|_| NativeError::Failed(COULD_NOT_START))?;
        let mut owned = Owned(Some(child));
        let child = owned.0.as_mut().expect("spawned");
        let mut stdin_pipe = child.stdin.take();
        let stdout_pipe = child.stdout.take();
        let stderr_pipe = child.stderr.take();

        let write = async {
            if let Some(mut pipe) = stdin_pipe.take() {
                let written = match &stdin {
                    Some(bytes) => pipe.write_all(bytes).await,
                    None => Ok(()),
                };
                drop(pipe);
                match written {
                    Err(error)
                        if !matches!(
                            error.kind(),
                            std::io::ErrorKind::BrokenPipe | std::io::ErrorKind::ConnectionReset
                        ) =>
                    {
                        return Err(error);
                    }
                    _ => {}
                }
            }
            Ok(())
        };
        let mut stdout_pipe = stdout_pipe;
        let mut stderr_pipe = stderr_pipe;
        let outcome = {
            let stdout_read = async {
                match stdout_pipe.as_mut() {
                    Some(pipe) => read_capped(pipe, cap, mobile_create, true).await,
                    None => Ok(Read::Done(Vec::new())),
                }
            };
            let stderr_read = async {
                match stderr_pipe.as_mut() {
                    Some(pipe) => read_capped(pipe, cap, mobile_create, false).await,
                    None => Ok(Read::Done(Vec::new())),
                }
            };
            let joined = run_call(write, stdout_read, stderr_read, child.wait());
            let budget = Duration::from_secs_f64(timeout.max(0.0));
            if mobile_create {
                // The supervisor cleans its owned Git group before it exits.
                let deadline = std::cmp::max(budget, MOBILE_CREATE_DRAIN);
                match tokio::time::timeout_at(started + deadline, joined).await {
                    Ok(result) if started.elapsed() <= budget => result,
                    Ok(_) | Err(_) => Err(Ended::TimedOut),
                }
            } else {
                tokio::time::timeout(budget, joined)
                    .await
                    .unwrap_or(Err(Ended::TimedOut))
            }
        };
        let child = owned.0.as_mut().expect("spawned");
        let exited = match &outcome {
            Ok((_, success)) => Some(*success),
            Err(_) => None,
        };
        if exited.is_none() && matches!(child.try_wait(), Ok(None)) {
            let _ = child.start_kill();
        }
        if exited.is_none() {
            // Drain without retaining, then close: a descendant may still
            // hold the pipes, and it is never killed.
            let cleanup = async {
                futures_util::future::join3(
                    discard(stdout_pipe.take()),
                    discard(stderr_pipe.take()),
                    child.wait(),
                )
                .await
            };
            let _ = tokio::time::timeout(DRAIN, cleanup).await;
        }
        match outcome {
            Ok((stdout, success)) => interpret(&call, success, stdout),
            Err(Ended::TimedOut) => Err(NativeError::Failed(TIMED_OUT)),
            Err(Ended::Oversized) => Err(NativeError::Failed(OUTPUT_TOO_LARGE)),
            Err(Ended::Io) => Err(NativeError::Failed(COULD_NOT_START)),
        }
    }
}

enum Ended {
    TimedOut,
    Oversized,
    Io,
}

/// `asyncio.gather(read(stdout), read(stderr), write(), process.wait())`:
/// the first failure ends the call, as gather propagates it.
async fn run_call(
    write: impl Future<Output = std::io::Result<()>>,
    stdout: impl Future<Output = std::io::Result<Read>>,
    stderr: impl Future<Output = std::io::Result<Read>>,
    wait: impl Future<Output = std::io::Result<std::process::ExitStatus>>,
) -> Result<(Vec<u8>, bool), Ended> {
    let fail = |read: std::io::Result<Read>| match read {
        Ok(Read::Done(bytes)) => Ok(bytes),
        Ok(Read::Oversized) => Err(Ended::Oversized),
        Err(_) => Err(Ended::Io),
    };
    let stdout = async { fail(stdout.await) };
    let stderr = async { fail(stderr.await).map(drop) };
    let write = async { write.await.map_err(|_| Ended::Io) };
    let wait = async { wait.await.map_err(|_| Ended::Io) };
    let (stdout, (), (), status) =
        futures_util::future::try_join4(stdout, stderr, write, wait).await?;
    Ok((stdout, status.success()))
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn exit_status_and_json_rules_follow_python() {
        let call = Call::new(["read", "s", "--json"]);
        assert_eq!(
            interpret(&call, false, b"{\"ok\": false}".to_vec()),
            Err(NativeError::Failed(FAILED))
        );
        let lenient = call.clone().failure_json();
        assert_eq!(
            interpret(
                &lenient,
                false,
                b"{\"ok\": false, \"error\": \"not_found\"}\n".to_vec()
            ),
            Ok(Output::Json(json!({"ok": false, "error": "not_found"})))
        );
        assert_eq!(
            interpret(&lenient, false, b"{\"ok\": true}".to_vec()),
            Err(NativeError::Failed(FAILED))
        );
        assert_eq!(
            interpret(&lenient, false, b" \x0b\x0c\n".to_vec()),
            Err(NativeError::Failed(FAILED))
        );
        assert_eq!(
            interpret(&call, true, b"\xef\xbb\xbf[1]".to_vec()),
            Ok(Output::Json(json!([1])))
        );
        assert_eq!(
            interpret(&call, true, b"NaN".to_vec()),
            Err(NativeError::Failed(INVALID_JSON))
        );
        assert_eq!(
            interpret(&call, true, b"".to_vec()),
            Err(NativeError::Failed(INVALID_JSON))
        );
        let text = Call::new(["--help"]).text();
        assert_eq!(
            interpret(&text, true, b"usage\n".to_vec()),
            Ok(Output::Text("usage\n".into()))
        );
        assert_eq!(
            interpret(&text, true, b"\xff".to_vec()),
            Err(NativeError::Unicode)
        );
    }

    #[test]
    fn worktree_catalogs_take_the_state_entry_point() {
        assert_eq!(
            native_argv(vec!["worktrees".into(), "--path".into()]),
            ["__state", "worktrees", "--path"]
        );
        assert_eq!(native_argv(vec!["inspect".into()]), ["inspect"]);
    }

    #[test]
    fn stdin_keeps_python_lengths() {
        let payload = json!({"text": "\u{e9}\u{754c}", "n": [1, 2.5]});
        assert_eq!(
            stdin_bytes(&payload).unwrap().len(),
            "{\"text\": \"\u{e9}\u{754c}\", \"n\": [1, 2.5]}".len()
        );
    }
}
