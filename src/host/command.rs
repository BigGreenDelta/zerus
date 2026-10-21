//! `hgs.exe __host`, the hidden entry of the session host and its tools (D1).
//! `main` runs it before `cli::dispatch`, so before `prepare_environment` puts
//! hgs.exe into its own kill-on-close job: the host must see where it was
//! started (D4), and a started host must break away from the caller's job.
//! Windows only; on Unix these words keep the meaning they have.
//!
//! - `serve`: the host itself.
//! - `call [--start] '<request>'`: connect (with `--start`, start a host when
//!   none runs), send one request (`{"op": "list"}`, the protocol's JSON
//!   without `id`) and print the response as JSON. Exits 0 when it is `ok`,
//!   1 when it is an `error`, 3 when no host runs and 4 when the host cannot
//!   be reached or started. `--start` starts the staged host image once local
//!   sessions are enabled; for an isolated state directory, `HGS_HOST_IMAGE`
//!   names another executable (tests name the build under test), which needs
//!   `HGS_HOST_ALLOW_UNSTAGED=1` to serve (`gate`).
//! - `status`: the running host's hello and `host_status`, or
//!   `{"running": false}`; never starts a host.
//! - `attach '<attach request>' '<steps>'`: a scripted attach client for tests.
//!   Steps are `{"send": TEXT}`, `{"send_hex": HEX}`, `{"resize": {"cols": N,
//!   "rows": N}}`, `{"wait_ms": N}` and `{"detach": true}`; it prints
//!   `{"attached", "output_hex", "controls"}` once the stream has ended (or
//!   a second after the last step).
//! - `backend '<operation>'`: one operation of the terminal backend hgs uses
//!   for this computer's sessions (`state::backend::host`), for tests, as
//!   `{"op": "listing"}`, `panes`, `pane_info` (`target`, `fields` named as
//!   `PaneField`s in snake case), `capture` (`pane`, `styled`), `grow_unviewed`
//!   (`pane`, `rows`), `send` (`pane`,
//!   `keys`), `paste` (`pane`, `text`, `mode`: `composer`, `terminal` or
//!   `terminal_bracketed`; staged and pasted as message input does),
//!   `set_run_marker` (`name`, `run_id`), `rename` (`id`, `name`),
//!   `kill_session` (`target`), `kill_pane` (`pane`), `session_exists`
//!   (`name`), `session_name` (`id`), `input_state` (`pane`, `name`, `run_id`,
//!   `agent`, `pid`), `request_exit` (`pane`, `pid`: the pause request of
//!   `hgs pause`) and `on_terminal` (`pid`, `tty`). It prints `{"ok": ...}`
//!   and exits 0, or `{"error": message}` and exits 1. Like a listing, only a
//!   start ever starts a host, and none of these is one.
//!
//! Only `call --start` and `serve` create the state directory.
use super::{
    client::{connect, connect_or_start, AttachStream, ClientError, Connection, Event},
    gate,
    protocol::{Attach, Key, Op, Response, Role, Size, Target},
    server,
};
use crate::state::backend::{self, host::HOST, PaneField, TerminalBackend};
use serde_json::{json, Value};
use std::{
    sync::mpsc,
    thread,
    time::{Duration, Instant},
};

const USAGE: &str = "usage: hgs __host serve | call [--start] '<request json>' | status | attach '<attach json>' '<steps json>' | backend '<operation json>'";

/// Run `__host` with the words after it; the exit code.
pub(crate) fn run(args: &[String]) -> i32 {
    let words: Vec<&str> = args.iter().map(String::as_str).collect();
    match words.as_slice() {
        ["serve"] => server::serve(),
        ["call", "--start", request] => call(request, true),
        ["call", request] => call(request, false),
        ["status"] => status(),
        ["attach", request, steps] => attach(request, steps),
        ["backend", operation] => backend(operation),
        _ => {
            eprintln!("{USAGE}");
            2
        }
    }
}

fn fail(error: &ClientError) -> i32 {
    eprintln!("hgs: {error}");
    match error {
        ClientError::NoHost => 3,
        _ => 4,
    }
}

fn open(start: bool, role: Role) -> Result<Connection, ClientError> {
    let failed = |error: std::io::Error| ClientError::Connect(error.to_string());
    if !start {
        // Without the state directory no host runs.
        return match server::existing_state_dir().map_err(failed)? {
            Some(state) => connect(&state, role),
            None => Err(ClientError::NoHost),
        };
    }
    let state = server::state_dir().map_err(failed)?;
    connect_or_start(&state, role, &|| {
        gate::host_image(&state).map_err(ClientError::Start)
    })
}

fn parse<T: serde::de::DeserializeOwned>(text: &str, what: &str) -> Result<T, i32> {
    serde_json::from_str(text).map_err(|error| {
        eprintln!("hgs: invalid {what}: {error}");
        2
    })
}

fn call(request: &str, start: bool) -> i32 {
    let op: Op = match parse(request, "session host request") {
        Ok(op) => op,
        Err(code) => return code,
    };
    if matches!(op, Op::Attach(_) | Op::SupervisorRegister(_)) {
        eprintln!("hgs: use __host attach for a stream");
        return 2;
    }
    // A host that decided to exit refuses a create as busy; with --start the
    // create goes to a new host instead (D3).
    let deadline = Instant::now() + Duration::from_secs(20);
    loop {
        let mut connection = match open(start, Role::Cli) {
            Ok(connection) => connection,
            Err(error) => return fail(&error),
        };
        let result = connection.request(op.clone());
        let retry = start
            && matches!(op, Op::CreateSession(_))
            && matches!(&result, Err(error) if error.code == super::protocol::ErrorCode::Busy)
            && Instant::now() < deadline;
        if retry {
            drop(connection);
            thread::sleep(Duration::from_millis(200));
            continue;
        }
        let ok = result.is_ok();
        println!("{}", json!(Response { id: 1, result }));
        return if ok { 0 } else { 1 };
    }
}

fn status() -> i32 {
    let mut connection = match open(false, Role::Cli) {
        Ok(connection) => connection,
        Err(ClientError::NoHost) => {
            println!("{}", json!({"running": false}));
            return 0;
        }
        Err(error) => return fail(&error),
    };
    let host = connection.host().clone();
    match connection.request(Op::HostStatus) {
        Ok(status) => {
            println!(
                "{}",
                json!({"running": true, "host": host, "status": status})
            );
            0
        }
        Err(error) => {
            eprintln!("hgs: {error}");
            1
        }
    }
}

/// One step of a scripted attach.
#[derive(serde::Deserialize)]
#[serde(rename_all = "snake_case")]
enum Step {
    Send(String),
    SendHex(String),
    Resize(Size),
    WaitMs(u64),
    Detach(bool),
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().fold(String::new(), |mut text, byte| {
        text.push_str(&format!("{byte:02x}"));
        text
    })
}

/// The bytes `text` spells as pairs of hex digits.
fn unhex(text: &str) -> Option<Vec<u8>> {
    if !text.len().is_multiple_of(2) || !text.bytes().all(|byte| byte.is_ascii_hexdigit()) {
        return None;
    }
    (0..text.len())
        .step_by(2)
        .map(|at| u8::from_str_radix(text.get(at..at + 2)?, 16).ok())
        .collect()
}

fn attach(request: &str, steps: &str) -> i32 {
    let request: Attach = match parse(request, "attach request") {
        Ok(request) => request,
        Err(code) => return code,
    };
    let steps: Vec<Step> = match parse(steps, "attach steps") {
        Ok(steps) => steps,
        Err(code) => return code,
    };
    if steps
        .iter()
        .any(|step| matches!(step, Step::SendHex(text) if unhex(text).is_none()))
    {
        eprintln!("hgs: invalid attach steps: send_hex takes pairs of hex digits");
        return 2;
    }
    let connection = match open(false, Role::Attach) {
        Ok(connection) => connection,
        Err(error) => return fail(&error),
    };
    let (attached, stream) = match connection.attach(request) {
        Ok(attached) => attached,
        Err(error) => {
            println!(
                "{}",
                json!(Response {
                    id: 1,
                    result: Err(error)
                })
            );
            return 1;
        }
    };
    let (events, received) = mpsc::channel();
    let reader: AttachStream = stream.clone();
    thread::spawn(move || {
        while let Ok(Some(event)) = reader.next() {
            if events.send(event).is_err() {
                return;
            }
        }
    });
    let mut sent = Ok(());
    for step in steps {
        sent = sent.and_then(|()| match step {
            Step::Send(text) => stream.send(text.as_bytes()),
            Step::SendHex(text) => stream.send(&unhex(&text).unwrap_or_default()),
            Step::Resize(size) => stream.resize(size),
            Step::WaitMs(millis) => {
                thread::sleep(Duration::from_millis(millis));
                Ok(())
            }
            Step::Detach(detach) => {
                if detach {
                    stream.detach()
                } else {
                    Ok(())
                }
            }
        });
    }
    let mut output = Vec::new();
    let mut controls = Vec::new();
    let deadline = Instant::now() + Duration::from_secs(1);
    while let Ok(event) = received.recv_timeout(deadline.saturating_duration_since(Instant::now()))
    {
        match event {
            Event::Output(bytes) => output.extend(bytes),
            Event::Control(control) => controls.push(control),
        }
    }
    let result: Value = json!({
        "attached": attached,
        "output_hex": hex(&output),
        "controls": controls,
        "sent": sent.is_ok(),
    });
    println!("{result}");
    0
}

/// One `__host backend` operation.
#[derive(serde::Deserialize)]
#[serde(tag = "op", rename_all = "snake_case")]
enum BackendOp {
    Listing,
    Panes,
    PaneInfo {
        target: Target,
        fields: Vec<String>,
    },
    Capture {
        pane: String,
        #[serde(default)]
        styled: bool,
    },
    GrowUnviewed {
        pane: String,
        rows: u16,
    },
    Send {
        pane: String,
        keys: Vec<Key>,
    },
    Paste {
        pane: String,
        text: String,
        mode: String,
    },
    SetRunMarker {
        name: String,
        run_id: String,
    },
    Rename {
        id: String,
        name: String,
    },
    KillSession {
        target: Target,
    },
    KillPane {
        pane: String,
    },
    SessionExists {
        name: String,
    },
    SessionName {
        id: String,
    },
    InputState {
        pane: String,
        name: String,
        run_id: String,
        agent: String,
        #[serde(default)]
        pid: Option<u64>,
    },
    RequestExit {
        pane: String,
        pid: u32,
    },
    OnTerminal {
        pid: String,
        tty: String,
    },
}

fn pane_field(name: &str) -> Option<PaneField> {
    use PaneField::*;
    Some(match name {
        "session_id" => SessionId,
        "session_name" => SessionName,
        "pane_id" => PaneId,
        "server_pid" => ServerPid,
        "pane_pid" => PanePid,
        "width" => Width,
        "height" => Height,
        "cursor_x" => CursorX,
        "cursor_y" => CursorY,
        "in_mode" => InMode,
        "current_command" => CurrentCommand,
        "dead" => Dead,
        "run_marker" => RunMarker,
        "bracketed_paste" => BracketedPaste,
        "tty" => Tty,
        _ => return None,
    })
}

fn backend_target(target: &Target) -> backend::Target<'_> {
    match target {
        Target::Name(name) => backend::Target::Name(name),
        Target::Session(id) => backend::Target::Session(id),
        Target::Pane(id) => backend::Target::Pane(id),
    }
}

fn backend(operation: &str) -> i32 {
    let operation: BackendOp = match parse(operation, "backend operation") {
        Ok(operation) => operation,
        Err(code) => return code,
    };
    let terminal: &dyn TerminalBackend = &HOST;
    let result: Result<Value, String> = match operation {
        BackendOp::Listing => terminal.listing().map(|listing| {
            json!({
                "clients": listing.clients.iter().map(|client| json!({
                    "session": client.session, "terminal": client.terminal,
                })).collect::<Vec<_>>(),
                "sessions": listing.sessions.iter().map(|session| json!({
                    "name": session.name, "attached": session.attached,
                    "created": session.created, "launch_id": session.launch_id,
                })).collect::<Vec<_>>(),
            })
        }),
        BackendOp::Panes => terminal.panes().map(|panes| {
            json!(panes
                .iter()
                .map(|pane| json!({
                    "session": pane.session, "pane": pane.pane,
                    "dead": pane.dead, "run_marker": pane.run_marker,
                }))
                .collect::<Vec<_>>())
        }),
        BackendOp::PaneInfo { target, fields } => fields
            .iter()
            .map(|name| pane_field(name).ok_or_else(|| format!("unknown pane field {name}")))
            .collect::<Result<Vec<_>, _>>()
            .and_then(|fields| terminal.pane_info(backend_target(&target), &fields))
            .map(|info| json!({"text": info.fields().join("\t")})),
        BackendOp::Capture { pane, styled } => terminal
            .capture(
                &pane,
                if styled {
                    backend::Capture::Styled
                } else {
                    backend::Capture::Plain
                },
            )
            .map(|bytes| json!({"text": String::from_utf8_lossy(&bytes)})),
        BackendOp::GrowUnviewed { pane, rows } => terminal
            .grow_unviewed(&pane, rows)
            .map(|grown| json!({"grown": grown})),
        BackendOp::Send { pane, keys } => {
            let keys: Vec<_> = keys
                .iter()
                .map(|key| match key {
                    Key::Named(name) => backend::Key::Named(name),
                    Key::Literal(text) => backend::Key::Literal(text),
                })
                .collect();
            terminal.send(&pane, &keys).map(|()| json!({}))
        }
        BackendOp::Paste { pane, text, mode } => {
            let mode = match mode.as_str() {
                "composer" => Ok(backend::Paste::Composer),
                "terminal" => Ok(backend::Paste::Terminal { bracketed: false }),
                "terminal_bracketed" => Ok(backend::Paste::Terminal { bracketed: true }),
                other => Err(format!("unknown paste mode {other}")),
            };
            let buffer = format!("hgs-backend-{}", std::process::id());
            mode.and_then(|mode| {
                if !terminal.stage(&buffer, text.as_bytes(), backend::StageErrors::Read)? {
                    return Err("the text was not staged".into());
                }
                let pasted = terminal.paste(&buffer, &pane, mode);
                terminal.discard(&buffer);
                pasted.map(|()| json!({}))
            })
        }
        BackendOp::SetRunMarker { name, run_id } => {
            terminal.set_run_marker(&name, &run_id).map(|()| json!({}))
        }
        BackendOp::Rename { id, name } => terminal.rename(&id, &name).map(|()| json!({})),
        BackendOp::KillSession { target } => terminal
            .kill_session(backend_target(&target))
            .map(|()| json!({})),
        BackendOp::KillPane { pane } => terminal.kill_pane(&pane).map(|()| json!({})),
        BackendOp::SessionExists { name } => Ok(json!({"exists": terminal.session_exists(&name)})),
        BackendOp::SessionName { id } => {
            terminal.session_name(&id).map(|name| json!({"name": name}))
        }
        BackendOp::InputState {
            pane,
            name,
            run_id,
            agent,
            pid,
        } => terminal
            .input_state(&backend::InputClaim {
                pane: &pane,
                name: &name,
                run_id: &run_id,
                agent: &agent,
                pid,
            })
            .map(|state| json!({"cursor_x": state.cursor_x, "cursor_y": state.cursor_y})),
        BackendOp::RequestExit { pane, pid } => {
            terminal.request_exit(&pane, pid).map(|()| json!({}))
        }
        BackendOp::OnTerminal { pid, tty } => terminal
            .on_terminal(&pid, &tty)
            .map(|attached| json!({"attached": attached})),
    };
    match result {
        Ok(value) => {
            println!("{}", json!({"ok": value}));
            0
        }
        Err(error) => {
            println!("{}", json!({"error": error}));
            1
        }
    }
}
