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
//!   be reached or started.
//! - `status`: the running host's hello and `host_status`, or
//!   `{"running": false}`; never starts a host.
//! - `attach '<attach request>' '<steps>'`: a scripted attach client for tests.
//!   Steps are `{"send": TEXT}`, `{"send_hex": HEX}`, `{"resize": {"cols": N,
//!   "rows": N}}`, `{"wait_ms": N}` and `{"detach": true}`; it prints
//!   `{"attached", "output_hex", "controls"}` once the stream has ended (or
//!   a second after the last step).
use super::{
    client::{connect, connect_or_start, AttachStream, ClientError, Connection, Event},
    protocol::{Attach, Op, Response, Role, Size},
    server,
};
use serde_json::{json, Value};
use std::{
    path::PathBuf,
    sync::mpsc,
    thread,
    time::{Duration, Instant},
};

const USAGE: &str = "usage: hgs __host serve | call [--start] '<request json>' | status | attach '<attach json>' '<steps json>'";

/// Run `__host` with the words after it; the exit code.
pub(crate) fn run(args: &[String]) -> i32 {
    let words: Vec<&str> = args.iter().map(String::as_str).collect();
    match words.as_slice() {
        ["serve"] => server::serve(),
        ["call", "--start", request] => call(request, true),
        ["call", request] => call(request, false),
        ["status"] => status(),
        ["attach", request, steps] => attach(request, steps),
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
    let state = server::state_dir().map_err(|e| ClientError::Connect(e.to_string()))?;
    if start {
        let image = host_image().map_err(|e| ClientError::Start(e.to_string()))?;
        connect_or_start(&state, role, &image)
    } else {
        connect(&state, role)
    }
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
    if text.len() % 2 != 0 || !text.bytes().all(|byte| byte.is_ascii_hexdigit()) {
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

/// The image a started host runs: until staging (D2) lands, this executable.
pub(crate) fn host_image() -> std::io::Result<PathBuf> {
    std::env::current_exe()
}
