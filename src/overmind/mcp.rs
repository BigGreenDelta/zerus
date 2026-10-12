//! `hgs overmind mcp`: the agent commands as a stdio MCP server (JSON-RPC 2.0,
//! one message per line). Stdout carries protocol messages only.
use super::model::{Actor, ActorKind, Refusal};
use super::store::Store;
use anyhow::Result;
use serde_json::{json, Value};
use std::io::{BufRead, Write};
use std::path::PathBuf;

/// What one tool call runs against: the store and the verified caller.
struct Session {
    store: Store,
    actor: Actor,
    cwd: Option<PathBuf>,
}

/// `require_session` is the launch flag `--session`: every tool call then needs a
/// verified agent session, so a server that lost its identity fails closed.
pub fn serve(config: &crate::config::Config, require_session: bool, input: impl BufRead, output: impl Write) -> Result<()> {
    let open = || -> Result<Session> {
        let machine = super::machine(config)?;
        let (actor, cwd) = super::identity::caller(&machine)?;
        let store = Store::open(&config.state.join("overmind"), &machine)?;
        Ok(Session { store, actor, cwd })
    };
    serve_with(&open, require_session, input, output)
}

const VERSIONS: [&str; 3] = ["2025-06-18", "2025-03-26", "2024-11-05"];
/// The largest message accepted on one line (an image travels as base64).
const MAX_LINE: u64 = 29 * 1024 * 1024;

const INSTRUCTIONS: &str = "Overmind tracks prompt-tasks for this project. Use my_tasks to see the tasks assigned to your session or sent to it for review, get_task to read one, and advance, comment, call_human, return_task, decompose, file_task, attach_file and review_task to work it. Done is for people: you can never move a task there, a person signs it off.";

fn error(id: &Value, code: i64, message: &str) -> Value {
    json!({"jsonrpc": "2.0", "id": id, "error": {"code": code, "message": message}})
}

fn tool(name: &str, description: &str, properties: Value, required: &[&str]) -> Value {
    json!({"name": name, "description": description,
        "inputSchema": {"type": "object", "properties": properties, "required": required}})
}

fn tools() -> Value {
    let task = json!({"type": "string", "description": "task reference such as om-1a2b, or its UUID"});
    let text = |d: &str| json!({"type": "string", "description": d});
    json!([
        tool("my_tasks", "List the tasks assigned to your session or sent to it for review.", json!({}), &[]),
        tool("get_task", "Read one task with its prompt, fields and journal.", json!({"task": task}), &["task"]),
        tool("comment", "Add a comment to a task's journal.", json!({"task": task, "text": text("the comment")}), &["task", "text"]),
        tool(
            "advance",
            "Move your task forward: Design to Build with a spec, Build to Review with a worklog and the commit as evidence. Never to Done.",
            json!({"task": task, "to": text("the stage to move to"), "spec": text("your plan, for Design to Build"),
                "worklog": text("what you did, for Build to Review"), "evidence": text("the commit SHA of the finished work"),
                "root_cause": text("the root cause, for a fix")}),
            &["task", "to"],
        ),
        tool("call_human", "Park the task until a person decides; ask your question.", json!({"task": task, "question": text("what the person must decide")}), &["task", "question"]),
        tool("return_task", "Give back a task that an external blocker prevents you from doing.", json!({"task": task, "reason": text("what blocks it")}), &["task", "reason"]),
        tool(
            "decompose",
            "Split a task into subtasks, optionally ordered.",
            json!({"task": task, "ordered": {"type": "boolean"},
                "subtasks": {"type": "array", "items": {"type": "object", "properties": {"title": {"type": "string"}, "prompt": {"type": "string"}}, "required": ["prompt"]}}}),
            &["task", "subtasks"],
        ),
        tool(
            "file_task",
            "Record an out-of-scope finding as a new task in Backlog.",
            json!({"prompt": text("the ready prompt of the new task"), "breaks": text("what it breaks or blocks"), "title": text("optional title"),
                "related": text("optional related task reference"), "project": text("optional project, defaults to the current task's")}),
            &["prompt", "breaks"],
        ),
        tool(
            "attach_file",
            "Attach a local image (png, jpg, gif or webp, at most 10 MiB) to a task.",
            json!({"task": task, "path": text("path of the image file"), "note": text("optional caption")}),
            &["task", "path"],
        ),
        tool(
            "review_task",
            "Record your review verdict for a task sent to you.",
            json!({"task": task, "verdict": {"type": "string", "enum": ["approve", "needs_work"]}, "report": text("your findings")}),
            &["task", "verdict", "report"],
        ),
        tool(
            "download_attachment",
            "Get the local path of a task's image by its sha256.",
            json!({"task": task, "sha256": text("the attachment's sha256")}),
            &["task", "sha256"],
        ),
    ])
}

fn open_nonblocking(path: &std::path::Path) -> std::io::Result<std::fs::File> {
    use std::os::unix::fs::OpenOptionsExt;
    std::fs::OpenOptions::new().read(true).custom_flags(libc::O_NONBLOCK).open(path)
}

/// Reads an image the agent names and turns it into an `attach` request.
fn attach_request(arguments: &Value) -> Result<Value> {
    #[derive(serde::Deserialize)]
    #[serde(deny_unknown_fields)]
    struct Args {
        task: String,
        path: String,
        #[serde(default)]
        note: String,
    }
    let args: Args = super::commands::parse(arguments)?;
    let path = std::path::Path::new(&args.path);
    let extension = path.extension().and_then(|e| e.to_str()).unwrap_or_default().to_ascii_lowercase();
    let mime = match extension.as_str() {
        "png" => "image/png",
        "jpg" | "jpeg" => "image/jpeg",
        "gif" => "image/gif",
        "webp" => "image/webp",
        _ => return Err(Refusal::field("invalid_field", "path", "invalid", "the file must be a png, jpg, jpeg, gif or webp image").into()),
    };
    let limit = super::commands::MAX_ATTACHMENT as u64;
    let unreadable = |e: std::io::Error| Refusal::field("invalid_field", "path", "invalid", format!("cannot read {}: {e}", args.path));
    let not_a_file = || Refusal::field("invalid_field", "path", "invalid", "the path is not a file");
    // Checked before opening, and opened without waiting: a path swapped for a
    // FIFO in between must not wait for a writer either.
    if !std::fs::metadata(path).map_err(unreadable)?.is_file() {
        return Err(not_a_file().into());
    }
    let file = open_nonblocking(path).map_err(unreadable)?;
    if !file.metadata().map_err(unreadable)?.is_file() {
        return Err(not_a_file().into());
    }
    let mut bytes = Vec::new();
    std::io::Read::read_to_end(&mut std::io::Read::take(file, limit + 1), &mut bytes).map_err(unreadable)?;
    if bytes.len() as u64 > limit {
        return Err(Refusal::field("invalid_field", "path", "invalid", "the image is larger than 10 MiB").into());
    }
    let name = path.file_name().and_then(|n| n.to_str()).unwrap_or("image").to_owned();
    let data = base64::Engine::encode(&base64::engine::general_purpose::STANDARD, &bytes);
    Ok(json!({"task": args.task, "name": name, "mime": mime, "data_base64": data, "note": args.note}))
}

fn run_tool(session: &mut Session, name: &str, arguments: Value) -> Result<Value> {
    use super::{agent, commands, remote};
    let mut ctx = commands::Ctx { store: &mut session.store, actor: session.actor.clone(), cwd: session.cwd.clone() };
    match name {
        "my_tasks" => agent::mine(&mut ctx),
        "get_task" => commands::show(&mut ctx, arguments),
        "comment" => commands::comment(&mut ctx, arguments),
        "advance" => agent::advance(&mut ctx, arguments),
        "call_human" => agent::call_human(&mut ctx, arguments),
        "return_task" => agent::return_task(&mut ctx, arguments),
        "decompose" => agent::decompose(&mut ctx, arguments),
        "file_task" => agent::file(&mut ctx, arguments),
        "attach_file" => agent::attach(&mut ctx, attach_request(&arguments)?),
        "review_task" => agent::review(&mut ctx, arguments),
        "download_attachment" => remote::attachment(&mut ctx, arguments),
        _ => unreachable!("tool names are checked before the call"),
    }
}

fn tool_result(text: String, is_error: bool) -> Value {
    json!({"content": [{"type": "text", "text": text}], "isError": is_error})
}

fn call_tool(open: &dyn Fn() -> Result<Session>, require_session: bool, name: &str, arguments: Value) -> Value {
    // The caller is checked on every call: a session may be renamed meanwhile.
    let outcome = open().and_then(|mut session| {
        if require_session && session.actor.kind != ActorKind::Agent {
            return Err(Refusal::new("session_mismatch", "this MCP server was started for an agent session, but no verified session identity is present").into());
        }
        run_tool(&mut session, name, arguments)
    });
    match outcome {
        Ok(value) => tool_result(value.to_string(), false),
        Err(failure) => match failure.downcast_ref::<Refusal>() {
            Some(refusal) => tool_result(json!({"ok": false, "error": refusal}).to_string(), true),
            None => tool_result(format!("{failure:#}"), true),
        },
    }
}

/// The answer to one parsed message, or `None` for a notification.
fn handle(open: &dyn Fn() -> Result<Session>, require_session: bool, message: &Value) -> Option<Value> {
    let Some(object) = message.as_object() else {
        return Some(error(&Value::Null, -32600, "a message must be a JSON object"));
    };
    let Some(id) = object.get("id") else {
        // A notification is never answered, not even when it is malformed.
        let method = object.get("method").and_then(Value::as_str);
        if object.get("jsonrpc").and_then(Value::as_str) != Some("2.0") || method.is_none() {
            eprintln!("hgs overmind mcp: ignored a notification without jsonrpc \"2.0\" or a method");
        }
        return None;
    };
    if !(id.is_string() || id.is_number() || id.is_null()) {
        return Some(error(&Value::Null, -32600, "the id must be a string or a number"));
    }
    if object.get("jsonrpc").and_then(Value::as_str) != Some("2.0") {
        return Some(error(id, -32600, "jsonrpc must be \"2.0\""));
    }
    let Some(method) = object.get("method").and_then(Value::as_str) else {
        // A response to a request of ours: there are none.
        if object.contains_key("result") || object.contains_key("error") {
            return None;
        }
        return Some(error(id, -32600, "a request needs a method"));
    };
    let params = object.get("params");
    let reply = |result: Value| json!({"jsonrpc": "2.0", "id": id, "result": result});
    Some(match method {
        "initialize" => {
            let asked = params.and_then(|p| p.get("protocolVersion")).and_then(Value::as_str).unwrap_or_default();
            let version = VERSIONS.iter().find(|v| **v == asked).copied().unwrap_or(VERSIONS[0]);
            reply(json!({"protocolVersion": version, "capabilities": {"tools": {}},
                "serverInfo": {"name": "overmind", "version": env!("CARGO_PKG_VERSION")}, "instructions": INSTRUCTIONS}))
        }
        "ping" => reply(json!({})),
        "tools/list" => reply(json!({"tools": tools()})),
        "tools/call" => {
            let Some(params) = params.and_then(Value::as_object) else {
                return Some(error(id, -32602, "tools/call needs params with a tool name"));
            };
            let Some(name) = params.get("name").and_then(Value::as_str) else {
                return Some(error(id, -32602, "tools/call needs a tool name"));
            };
            if !tools().as_array().is_some_and(|all| all.iter().any(|t| t["name"] == name)) {
                return Some(error(id, -32602, &format!("unknown tool {name}")));
            }
            let arguments = match params.get("arguments") {
                None | Some(Value::Null) => json!({}),
                Some(v) if v.is_object() => v.clone(),
                Some(_) => return Some(error(id, -32602, "arguments must be an object")),
            };
            reply(call_tool(open, require_session, name, arguments))
        }
        _ => error(id, -32601, &format!("method not found: {method}")),
    })
}

fn serve_with(open: &dyn Fn() -> Result<Session>, require_session: bool, mut input: impl BufRead, mut output: impl Write) -> Result<()> {
    let mut line = Vec::new();
    loop {
        line.clear();
        let read = std::io::Read::take(&mut input, MAX_LINE + 1).read_until(b'\n', &mut line)?;
        if read == 0 {
            return Ok(());
        }
        let mut answer = None;
        if !line.ends_with(b"\n") && line.len() as u64 > MAX_LINE {
            // Too long: skip the rest of the line and say so.
            loop {
                let (skip, done) = {
                    let buffer = input.fill_buf()?;
                    match buffer.iter().position(|b| *b == b'\n') {
                        Some(at) => (at + 1, true),
                        None => (buffer.len(), buffer.is_empty()),
                    }
                };
                input.consume(skip);
                if done {
                    break;
                }
            }
            answer = Some(error(&Value::Null, -32600, "the message exceeds 29 MiB"));
        } else if !line.iter().all(u8::is_ascii_whitespace) {
            answer = match serde_json::from_slice::<Value>(&line) {
                Ok(message) => handle(open, require_session, &message),
                Err(e) => {
                    eprintln!("hgs overmind mcp: unparsable message: {e}");
                    Some(error(&Value::Null, -32700, "parse error"))
                }
            };
        }
        if let Some(answer) = answer {
            writeln!(output, "{answer}")?;
            output.flush()?;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::overmind::commands::{self, Ctx};
    use crate::overmind::model::SessionRef;
    use crate::overmind::workflow::Action;
    use std::collections::BTreeMap;

    const MACHINE: &str = "11111111-1111-4111-8111-111111111111";

    struct Fixture {
        dir: tempfile::TempDir,
        task: String,
    }

    fn fixture() -> Fixture {
        let dir = tempfile::tempdir().unwrap();
        let mut store = Store::open(dir.path(), MACHINE).unwrap();
        let mut ctx = Ctx { store: &mut store, actor: Actor::human(MACHINE), cwd: None };
        let task = commands::create(&mut ctx, json!({"project": "p", "prompt": "Do it", "stage": "Queue"})).unwrap()["task"]["uuid"].as_str().unwrap().to_owned();
        store
            .write(|tx| {
                let current = commands::load(tx, &task)?;
                commands::transition(tx, &Actor::human(MACHINE), &current, Action::Claim, "Design", BTreeMap::new(), Some(SessionRef { machine: MACHINE.into(), name: "w1".into() }), vec![], "")?;
                Ok(())
            })
            .unwrap();
        Fixture { dir, task }
    }

    /// Runs the lines through a server whose caller is the session `w1`.
    fn talk(fx: &Fixture, lines: &[Value]) -> Vec<Value> {
        talk_raw(fx, &lines.iter().map(|l| l.to_string()).collect::<Vec<_>>().join("\n"), Some("w1"))
    }

    fn talk_raw(fx: &Fixture, input: &str, session: Option<&str>) -> Vec<Value> {
        let open = || -> Result<Session> {
            let store = Store::open(fx.dir.path(), MACHINE)?;
            let actor = match session {
                Some(name) => Actor::agent(MACHINE, name),
                None => return Err(Refusal::new("session_mismatch", "no session").into()),
            };
            Ok(Session { store, actor, cwd: None })
        };
        let mut out = Vec::new();
        serve_with(&open, false, input.as_bytes(), &mut out).unwrap();
        String::from_utf8(out).unwrap().lines().map(|l| serde_json::from_str(l).unwrap()).collect()
    }

    fn call(id: u64, name: &str, arguments: Value) -> Value {
        json!({"jsonrpc": "2.0", "id": id, "method": "tools/call", "params": {"name": name, "arguments": arguments}})
    }

    fn payload(reply: &Value) -> Value {
        serde_json::from_str(reply["result"]["content"][0]["text"].as_str().unwrap()).unwrap()
    }

    #[test]
    fn initialize_and_list_the_eleven_tools() {
        let fx = fixture();
        let replies = talk(&fx, &[
            json!({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2024-11-05", "capabilities": {}}}),
            json!({"jsonrpc": "2.0", "method": "notifications/initialized"}),
            json!({"jsonrpc": "2.0", "id": 2, "method": "ping"}),
            json!({"jsonrpc": "2.0", "id": 3, "method": "tools/list"}),
        ]);
        assert_eq!(replies.len(), 3, "a notification gets no answer");
        assert_eq!(replies[0]["result"]["protocolVersion"], "2024-11-05");
        assert_eq!(replies[0]["result"]["serverInfo"]["name"], "overmind");
        assert!(replies[0]["result"]["capabilities"]["tools"].is_object());
        assert!(replies[0]["result"]["instructions"].as_str().unwrap().contains("Done"));
        assert_eq!(replies[1]["result"], json!({}));
        let names: Vec<&str> = replies[2]["result"]["tools"].as_array().unwrap().iter().map(|t| t["name"].as_str().unwrap()).collect();
        assert_eq!(names, ["my_tasks", "get_task", "comment", "advance", "call_human", "return_task", "decompose", "file_task", "attach_file", "review_task", "download_attachment"]);
        for tool in replies[2]["result"]["tools"].as_array().unwrap() {
            assert!(tool["description"].as_str().unwrap().len() > 5);
            assert_eq!(tool["inputSchema"]["type"], "object");
            assert!(tool["inputSchema"]["required"].is_array());
        }
        let other = talk(&fx, &[json!({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "1999-01-01"}})]);
        assert_eq!(other[0]["result"]["protocolVersion"], "2025-06-18");
    }

    #[test]
    fn my_tasks_and_get_task_work() {
        let fx = fixture();
        let replies = talk(&fx, &[call(1, "my_tasks", json!({})), call(2, "get_task", json!({"task": fx.task})), call(3, "comment", json!({"task": fx.task, "text": "hello"}))]);
        assert_ne!(replies[0]["result"]["isError"], true);
        assert_eq!(payload(&replies[0])["tasks"][0]["uuid"], fx.task);
        assert_eq!(payload(&replies[1])["task"]["prompt"], "Do it");
        assert_eq!(payload(&replies[2])["ok"], true);
    }

    #[test]
    fn done_is_unreachable_through_every_tool() {
        let fx = fixture();
        let replies = talk(&fx, &[
            call(1, "advance", json!({"task": fx.task, "to": "Done", "spec": "x"})),
            call(2, "advance", json!({"task": fx.task, "to": "Done", "worklog": "x", "evidence": "abc1234"})),
            call(3, "review_task", json!({"task": fx.task, "verdict": "approve", "report": "fine"})),
            call(4, "return_task", json!({"task": fx.task, "reason": "blocked", "to": "Done"})),
            call(5, "call_human", json!({"task": fx.task, "question": "ok?", "to": "Done"})),
        ]);
        for reply in &replies {
            assert_eq!(reply["result"]["isError"], true, "{reply}");
            assert_eq!(payload(reply)["ok"], false);
        }
        let show = talk(&fx, &[call(1, "get_task", json!({"task": fx.task}))]);
        assert_eq!(payload(&show[0])["task"]["stage"], "Design");
    }

    #[test]
    fn a_refusal_is_an_error_result_with_its_json() {
        let fx = fixture();
        let replies = talk_raw(&fx, &call(1, "my_tasks", json!({})).to_string(), None);
        assert_eq!(replies[0]["result"]["isError"], true);
        assert_eq!(payload(&replies[0])["error"]["code"], "session_mismatch");
        let unknown = talk(&fx, &[call(1, "advance", json!({"task": "nope", "to": "Build", "spec": "x"}))]);
        assert_eq!(unknown[0]["result"]["isError"], true);
    }

    #[test]
    fn a_server_started_with_session_refuses_calls_without_a_verified_agent() {
        let fx = fixture();
        // Nobody calls as a person: the launch flag makes a missing identity an error.
        let human = || -> Result<Session> {
            Ok(Session { store: Store::open(fx.dir.path(), MACHINE)?, actor: Actor::human(MACHINE), cwd: None })
        };
        let agent = || -> Result<Session> {
            Ok(Session { store: Store::open(fx.dir.path(), MACHINE)?, actor: Actor::agent(MACHINE, "w1"), cwd: None })
        };
        let input = [call(1, "my_tasks", json!({})), json!({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})]
            .iter()
            .map(|l| l.to_string())
            .collect::<Vec<_>>()
            .join("\n");
        let run = |open: &dyn Fn() -> Result<Session>, require: bool| -> Vec<Value> {
            let mut out = Vec::new();
            serve_with(open, require, input.as_bytes(), &mut out).unwrap();
            String::from_utf8(out).unwrap().lines().map(|l| serde_json::from_str(l).unwrap()).collect()
        };
        let refused = run(&human, true);
        assert_eq!(refused[0]["result"]["isError"], true);
        assert_eq!(payload(&refused[0])["error"]["code"], "session_mismatch");
        assert!(refused[1]["result"]["tools"].is_array(), "the protocol itself still works");
        let by_hand = run(&human, false);
        // Without the flag a person keeps today's behaviour: the command itself answers.
        assert_ne!(payload(&by_hand[0])["error"]["code"], "session_mismatch");
        let verified = run(&agent, true);
        assert_eq!(verified[0]["result"]["isError"], false);
    }

    #[test]
    fn malformed_and_unknown_requests_get_errors() {
        let fx = fixture();
        let input = [
            "{not json".to_string(),
            json!({"jsonrpc": "2.0", "id": 1, "method": "nothing/here"}).to_string(),
            json!({"id": 2, "method": "ping"}).to_string(),
            json!({"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": "x"}).to_string(),
            json!({"jsonrpc": "2.0", "id": 4, "method": "tools/call", "params": {"name": "no_such_tool"}}).to_string(),
            json!({"jsonrpc": "2.0", "id": 5, "method": "tools/call", "params": {"name": "comment", "arguments": [1]}}).to_string(),
            json!({"jsonrpc": "2.0", "id": 6, "method": "tools/call", "params": {"name": "comment", "arguments": {"task": fx.task}}}).to_string(),
            "[1,2]".to_string(),
            json!({"jsonrpc": "2.0", "method": "nothing/here"}).to_string(),
            json!({"jsonrpc": "2.0", "id": 7, "method": "ping"}).to_string(),
        ]
        .join("\n");
        let replies = talk_raw(&fx, &input, Some("w1"));
        assert_eq!(replies.len(), 9, "{replies:?}");
        assert_eq!(replies[0]["error"]["code"], -32700);
        assert!(replies[0]["id"].is_null());
        assert_eq!(replies[1]["error"]["code"], -32601);
        assert_eq!(replies[2]["error"]["code"], -32600);
        assert_eq!(replies[3]["error"]["code"], -32602);
        assert_eq!(replies[4]["error"]["code"], -32602);
        assert_eq!(replies[5]["error"]["code"], -32602);
        assert_eq!(replies[6]["result"]["isError"], true);
        assert_eq!(replies[7]["error"]["code"], -32600);
        assert_eq!(replies[8]["result"], json!({}), "the server keeps answering after errors");
    }

    #[test]
    fn eof_ends_cleanly_and_a_missing_final_newline_is_read() {
        let fx = fixture();
        assert!(talk_raw(&fx, "", Some("w1")).is_empty());
        assert_eq!(talk_raw(&fx, "\n\n{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}", Some("w1")).len(), 1);
    }

    #[test]
    fn attach_file_reads_an_image_and_download_returns_a_path() {
        let fx = fixture();
        let files = tempfile::tempdir().unwrap();
        let png = files.path().join("shot.png");
        std::fs::write(&png, b"\x89PNG\r\n\x1a\nxx").unwrap();
        let text = files.path().join("notes.txt");
        std::fs::write(&text, b"hi").unwrap();
        let replies = talk(&fx, &[
            call(1, "attach_file", json!({"task": fx.task, "path": png, "note": "screen"})),
            call(2, "attach_file", json!({"task": fx.task, "path": text})),
            call(3, "attach_file", json!({"task": fx.task, "path": files.path().join("missing.png")})),
        ]);
        let body = payload(&replies[0]);
        assert_eq!(body["ok"], true, "{body}");
        assert_eq!(replies[1]["result"]["isError"], true);
        assert_eq!(replies[2]["result"]["isError"], true);
        let sha = body["task"]["attachments"][0]["sha256"].as_str().unwrap().to_owned();
        let download = talk(&fx, &[call(1, "download_attachment", json!({"task": fx.task, "sha256": sha}))]);
        let got = payload(&download[0]);
        assert!(std::path::Path::new(got["path"].as_str().unwrap()).is_file());
        assert!(got.get("data_base64").is_none());
    }

    #[test]
    fn a_message_without_an_id_is_never_answered() {
        let fx = fixture();
        let input = [
            json!({"method": "notifications/initialized"}).to_string(),
            json!({"jsonrpc": "1.0", "method": "notifications/cancelled"}).to_string(),
            json!({"jsonrpc": 2, "method": "notifications/progress"}).to_string(),
            json!({"jsonrpc": "2.0"}).to_string(),
            json!({"jsonrpc": "2.0", "method": "notifications/initialized"}).to_string(),
            json!({"jsonrpc": "2.0", "id": 1, "method": "ping"}).to_string(),
        ]
        .join("\n");
        let replies = talk_raw(&fx, &input, Some("w1"));
        assert_eq!(replies.len(), 1, "{replies:?}");
        assert_eq!(replies[0]["id"], 1);
    }

    #[test]
    fn a_fifo_opens_without_waiting_for_a_writer() {
        let dir = tempfile::tempdir().unwrap();
        let fifo = dir.path().join("pipe.png");
        let made = std::process::Command::new("mkfifo").arg(&fifo).status().unwrap();
        assert!(made.success());
        let file = open_nonblocking(&fifo).unwrap();
        assert!(!file.metadata().unwrap().is_file());
    }

    #[test]
    fn attach_file_refuses_a_fifo_without_waiting_for_a_writer() {
        use std::os::unix::ffi::OsStrExt;
        let fx = fixture();
        let files = tempfile::tempdir().unwrap();
        let fifo = files.path().join("pipe.png");
        let name = std::ffi::CString::new(fifo.as_os_str().as_bytes()).unwrap();
        assert_eq!(unsafe { libc::mkfifo(name.as_ptr(), 0o600) }, 0);
        let request = call(1, "attach_file", json!({"task": fx.task, "path": fifo}));
        let (sender, receiver) = std::sync::mpsc::channel();
        std::thread::spawn(move || {
            let _ = sender.send(talk(&fx, &[request]));
        });
        match receiver.recv_timeout(std::time::Duration::from_secs(10)) {
            Ok(replies) => {
                assert_eq!(replies[0]["result"]["isError"], true, "{replies:?}");
                assert_eq!(payload(&replies[0])["error"]["field"], "path");
            }
            Err(_) => {
                // Release the blocked reader before failing.
                let _ = std::fs::OpenOptions::new().write(true).open(&fifo);
                panic!("attach_file waited for a writer on a FIFO");
            }
        }
    }

    #[test]
    fn attach_file_refuses_a_file_over_ten_mib() {
        let fx = fixture();
        let files = tempfile::tempdir().unwrap();
        let big = files.path().join("big.png");
        std::fs::write(&big, vec![0u8; 10 * 1024 * 1024 + 1]).unwrap();
        let replies = talk(&fx, &[call(1, "attach_file", json!({"task": fx.task, "path": big}))]);
        assert_eq!(replies[0]["result"]["isError"], true);
    }
}
