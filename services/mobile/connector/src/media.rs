//! Terminal screens and input, attachment and file reads, voice messages,
//! speech jobs and speech configuration: `Connector.validate` and
//! `_execute_claimed` (`connector.py:618-1235`) for these operations, with
//! Python's order of checks, capability gates, preflights, native argv,
//! budgets and acknowledgement rules.
//!
//! [`Connector::media`] makes the whole receipt of such a request; the
//! shared pipeline in `execute.rs` claims, routes, sanitizes and journals
//! it, as for every other operation.
use crate::capabilities::{Flags, SPEECH, SPEECH_CONFIG, TERMINAL, VOICE};
use crate::execute::{Failure, GENERIC, MAX_RESULT, TOO_LARGE};
use crate::files;
use crate::journal::is_mutation;
use crate::native::{self, Call, Native, NativeError};
use crate::pyvalue::{bounded, hashable, is_str, py_eq, PyError, PyNumber, PyResult};
use crate::request::receipt;
use crate::runtime::Connector;
use crate::speech;
use crate::validate::{self, Context, Validated};
use serde_json::{json, Map, Value};
use std::sync::Arc;
use zerus_relay::json;

/// The 21 keys a phone may send to a terminal (`terminal.KEYS`).
pub const TERMINAL_KEYS: [&str; 21] = [
    "Enter", "Escape", "Tab", "BTab", "BSpace", "Up", "Down", "Left", "Right", "Home", "End",
    "PPage", "NPage", "DC", "C-c", "C-d", "C-l", "C-a", "C-e", "C-u", "C-w",
];
/// Literal terminal text, in UTF-8 bytes.
pub const MAX_TERMINAL_TEXT: usize = 64 * 1024;
/// Terminal input must still be claimable this long after the claim, at
/// most (seconds): the relay's deadline is shorter.
pub const INPUT_WINDOW: f64 = 6.0;
/// Native budgets (seconds); attachment and file reads use the default.
pub const TERMINAL_TIMEOUT: f64 = 3.0;
pub const SPEECH_TIMEOUT: f64 = 10.0;
pub const VOICE_TIMEOUT: f64 = 30.0;
pub const SPEECH_CONFIG_TIMEOUT: f64 = 35.0;

pub const EXPIRED: &str = "terminal input expired before native delivery";
pub const TERMINAL_CHANGED: &str = "native Terminal binding or input support changed";
pub const TERMINAL_MISMATCH: &str =
    "native Terminal acknowledgement identity or status did not match";
pub const VOICE_MISMATCH: &str = "native voice acknowledgement did not match";
pub const SPEECH_MISMATCH: &str = "native speech acknowledgement did not match";
pub const CONFIG_MISMATCH: &str = "native speech configuration acknowledgement did not match";

const IDENTITY: [&str; 3] = ["request_id", "expected_run_id", "expected_conversation_id"];
const READS: [&str; 2] = ["attachment_read", "file_read"];

/// Whether this module runs `operation`.
pub fn handles(operation: &str) -> bool {
    [TERMINAL, VOICE, SPEECH, SPEECH_CONFIG, &READS]
        .iter()
        .any(|family| family.contains(&operation))
}

/// Whether a claimed request belongs to this module.
pub fn claims(request: &Map<String, Value>) -> bool {
    request
        .get("operation")
        .and_then(Value::as_str)
        .is_some_and(handles)
}

fn text(error: PyError) -> String {
    match error {
        PyError::Value(message) => message,
        PyError::Type => GENERIC.into(),
    }
}

fn py(error: PyError) -> Failure {
    match error {
        PyError::Value(message) => Failure::Connector(message),
        PyError::Type => Failure::Generic,
    }
}

fn exact(payload: &Map<String, Value>, keys: &[&str]) -> bool {
    payload.len() == keys.len() && keys.iter().all(|key| payload.contains_key(*key))
}

/// `terminal.validate(operation, payload, request_id)`: the exact identity,
/// then either one allowlisted key or literal text without hidden controls.
pub fn validate_terminal(operation: &str, payload: &Value, request_id: &str) -> PyResult<()> {
    let input = operation == "terminal_input";
    let extras: &[&str] = if input {
        &["terminal_binding_id", "text", "enter", "key"]
    } else {
        &[]
    };
    let Some(object) = payload.as_object().filter(|payload| {
        IDENTITY.iter().all(|key| payload.contains_key(*key))
            && payload
                .keys()
                .all(|key| IDENTITY.contains(&key.as_str()) || extras.contains(&key.as_str()))
            && is_str(payload.get("request_id"), request_id)
    }) else {
        return Err(PyError::value("invalid scoped terminal fields"));
    };
    bounded(object.get("expected_run_id"), 128, false)?;
    bounded(object.get("expected_conversation_id"), 256, true)?;
    if !input {
        return Ok(());
    }
    let binding = bounded(object.get("terminal_binding_id"), 64, false)?;
    if binding.len() != 64
        || !binding
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
    {
        return Err(PyError::value("invalid terminal binding"));
    }
    let [request, run, conversation] = IDENTITY;
    if object.contains_key("key") {
        let key = object["key"].as_str();
        if !exact(
            object,
            &[request, run, conversation, "terminal_binding_id", "key"],
        ) || !key.is_some_and(|key| TERMINAL_KEYS.contains(&key))
        {
            return Err(PyError::value("unsupported terminal key"));
        }
        return Ok(());
    }
    let fields = [
        request,
        run,
        conversation,
        "terminal_binding_id",
        "text",
        "enter",
    ];
    let literal = object["text"].as_str().zip(object["enter"].as_bool());
    let valid = exact(object, &fields)
        && literal.is_some_and(|(text, enter)| {
            text.len() <= MAX_TERMINAL_TEXT
                && (enter || !text.is_empty())
                && !text
                    .chars()
                    .any(|c| ((c as u32) < 32 && c != '\n' && c != '\t') || c as u32 == 127)
        });
    if !valid {
        return Err(PyError::value("invalid literal terminal text"));
    }
    Ok(())
}

/// `Connector.validate(request)` for an operation of this module, after the
/// claim: speech and its configuration belong to the computer, the others
/// to one exact conversation of the last local snapshot.
pub fn validate(
    operation: &str,
    request: &Map<String, Value>,
    request_id: &str,
    context: &Context,
) -> Result<Validated, String> {
    let validated = |session: &str, payload: &Value| Validated {
        operation: operation.into(),
        session: session.into(),
        payload: Some(payload.clone()),
    };
    let null = Value::Null;
    let payload = request.get("payload");
    let computer = |message: &str| {
        if request.get("session").and_then(Value::as_str) == Some("") {
            Ok(())
        } else {
            Err(message.to_owned())
        }
    };
    if SPEECH_CONFIG.contains(&operation) {
        computer("speech configuration uses no session target")?;
        speech::validate_config(operation, payload.unwrap_or(&null), request_id).map_err(text)?;
        return Ok(validated("", payload.unwrap_or(&null)));
    }
    if SPEECH.contains(&operation) {
        computer("speech uses no session target")?;
        speech::validate_speech(operation, payload.unwrap_or(&null), request_id).map_err(text)?;
        return Ok(validated("", payload.unwrap_or(&null)));
    }
    let session = validate::valid_session(request.get("session"))
        .ok_or_else(|| validate::SESSION.to_owned())?;
    if request.contains_key("archive_id") {
        return Err("archive identity belongs only in inspect payload".into());
    }
    let object = payload.and_then(Value::as_object);
    let archive = object.and_then(|payload| payload.get("archive_id"));
    if let Some(archive) = archive {
        // Of these, only reads of files may name an archived conversation.
        if !READS.contains(&operation) {
            return Err("archived targets support inspection only".into());
        }
        let archive = validate::checked_uuid(Some(archive))?;
        if !context.admits(session, &archive) && !context.archive_supported {
            return Err("archive must match a fresh exact local snapshot entry".into());
        }
    } else if !context.sessions.contains(session) {
        return Err(validate::NONARCHIVED.into());
    }
    let Some(object) = object
        .filter(|payload| payload.get("request_id").and_then(Value::as_str) == Some(request_id))
    else {
        return Err("payload request_id must match request identity".into());
    };
    let run = object.get("expected_run_id").and_then(Value::as_str);
    if run.is_none_or(str::is_empty) {
        return Err("expected_run_id is required".into());
    }
    let payload = payload.expect("an object");
    match operation {
        "attachment_read" => files::validate_attachment(payload, request_id),
        "file_read" => files::validate_file(payload, request_id),
        _ if VOICE.contains(&operation) => speech::validate_voice(operation, payload, request_id),
        _ => validate_terminal(operation, payload, request_id),
    }
    .map_err(text)?;
    match json::python_default_len(payload, false) {
        Ok(size) if size > native::MAX_PAYLOAD => Err(native::REQUEST_TOO_LARGE.into()),
        Ok(_) => Ok(validated(session, payload)),
        Err(_) => Err("invalid native payload".into()),
    }
}

/// The capability gates of `_execute_claimed`.
fn gate(operation: &str, flags: Flags) -> Result<(), Failure> {
    let (supported, unavailable) = if TERMINAL.contains(&operation) {
        (flags.terminal, "native scoped Terminal ABI is unavailable")
    } else if VOICE.contains(&operation) {
        (flags.voice, "native voice message ABI is unavailable")
    } else if SPEECH_CONFIG.contains(&operation) {
        (
            flags.speech_config,
            "native speech profile management ABI is unavailable",
        )
    } else if SPEECH.contains(&operation) {
        (flags.speech, "native speech ABI is unavailable")
    } else if operation == "file_read" {
        (
            flags.file_read,
            "native computer file read ABI is unavailable",
        )
    } else {
        (
            flags.attachment_read,
            "native attachment read ABI is unavailable",
        )
    };
    if supported {
        Ok(())
    } else {
        Err(Failure::text(unavailable))
    }
}

/// The claim deadline of terminal input: `type(x) in (int, float)` and
/// finite. An integer beyond the double range raises OverflowError in
/// `math.isfinite`, which only the generic receipt reports.
fn deadline(value: Option<&Value>) -> Result<f64, Failure> {
    let Some(Value::Number(number)) = value else {
        return Err(Failure::text(EXPIRED));
    };
    let seconds = PyNumber::of(number).as_f64().ok_or(Failure::Generic)?;
    if seconds.is_finite() {
        Ok(seconds)
    } else {
        Err(Failure::text(EXPIRED))
    }
}

fn same(value: Option<&Value>, payload: &Value, key: &str) -> bool {
    py_eq(value.unwrap_or(&Value::Null), &payload[key])
}

/// `{"action": action, **payload}`, as native stdin.
fn with_action(payload: &Value, action: &str) -> Value {
    let mut native = payload.clone();
    native["action"] = json!(action);
    native
}

/// Work that Python moves to a thread (decoding and chunking files), off
/// the runtime thread; its error text is the receipt's.
async fn blocking<T: Send + 'static>(
    work: impl FnOnce() -> Result<T, String> + Send + 'static,
) -> Result<T, Failure> {
    match tokio::task::spawn_blocking(work).await {
        Ok(Ok(value)) => Ok(value),
        Ok(Err(message)) => Err(Failure::Connector(message)),
        Err(_) => Err(Failure::Generic),
    }
}

/// A finished request of this module: state, result and error.
type Answer = (&'static str, Value, Value);

impl<N: Native + 'static> Connector<N> {
    /// `_execute_claimed(request)` for an operation of this module: the
    /// receipt, which `execute` sanitizes and journals. A failure after the
    /// native mutation started is uncertain, before it failed.
    pub async fn media(&self, request: &Map<String, Value>, request_id: &str) -> Value {
        let mut started = false;
        let outcome = self.media_answer(request, request_id, &mut started).await;
        let failed = |failure: Failure| {
            let state = if started { "uncertain" } else { "failed" };
            receipt(state, Value::Null, Some(failure.message()))
        };
        match outcome {
            Ok((state, result, error)) => {
                let response = json!({"state": state, "result": result, "error": error});
                match json::python_default_len(&response, true) {
                    Ok(size) if size <= MAX_RESULT => response,
                    Ok(_) => failed(Failure::text(TOO_LARGE)),
                    // A number beyond the double range: Python stores
                    // "Infinity", which no relay accepts.
                    Err(_) => failed(Failure::Generic),
                }
            }
            Err(failure) => failed(failure),
        }
    }

    async fn media_answer(
        &self,
        request: &Map<String, Value>,
        request_id: &str,
        started: &mut bool,
    ) -> Result<Answer, Failure> {
        // Python validates on a thread: a recording or a range can be large.
        let validated = {
            let (request, id, context) = (request.clone(), request_id.to_owned(), self.context());
            blocking(move || validate::validate(&request, &id, &context)).await?
        };
        let operation = validated.operation.as_str();
        let session = validated.session.as_str();
        let payload = validated.payload.as_ref().expect("validated payload");
        gate(operation, self.flags.get())?;
        if operation == "terminal_input" {
            self.terminal_preflight(request, session, payload).await?;
        }
        *started = is_mutation(operation);
        if TERMINAL.contains(&operation) {
            return self
                .terminal(request, operation, session, payload, request_id)
                .await;
        }
        let result = match operation {
            "attachment_read" => self.read_attachment(session, payload, request_id).await?,
            "file_read" => self.read_file(session, payload, request_id).await?,
            _ if VOICE.contains(&operation) => self.voice(operation, session, payload).await?,
            _ if SPEECH_CONFIG.contains(&operation) => {
                self.speech_config(operation, payload).await?
            }
            _ => self.speech(operation, payload).await?,
        };
        Ok(("completed", result, Value::Null))
    }

    /// Terminal input runs only inside its claim window, `(now, now + 6]`,
    /// after a fresh snapshot shows the same binding still accepts it, and
    /// never once the deadline has passed meanwhile.
    async fn terminal_preflight(
        &self,
        request: &Map<String, Value>,
        session: &str,
        payload: &Value,
    ) -> Result<(), Failure> {
        let deadline = deadline(request.get("expires_at"))?;
        let now = self.wall();
        if deadline <= now || deadline > now + INPUT_WINDOW {
            return Err(Failure::text(EXPIRED));
        }
        let mut snapshot = Map::new();
        for key in IDENTITY {
            snapshot.insert(key.into(), payload[key].clone());
        }
        snapshot.insert("action".into(), json!("snapshot"));
        let call = Call::new(["terminal", session, "--json"])
            .payload(Value::Object(snapshot))
            .timeout(TERMINAL_TIMEOUT);
        let fresh = self.native(call).await?.json();
        let multiline = payload["text"]
            .as_str()
            .is_some_and(|text| text.contains('\n'));
        let accepts = fresh.as_object().is_some_and(|fresh| {
            is_str(fresh.get("name"), session)
                && same(fresh.get("run_id"), payload, "expected_run_id")
                && same(
                    fresh.get("conversation_id"),
                    payload,
                    "expected_conversation_id",
                )
                && same(
                    fresh.get("terminal_binding_id"),
                    payload,
                    "terminal_binding_id",
                )
                && fresh.get("input_supported") == Some(&json!(true))
                && (!multiline || fresh.get("multiline_supported") == Some(&json!(true)))
        });
        if !accepts {
            return Err(Failure::text(TERMINAL_CHANGED));
        }
        if deadline <= self.wall() {
            return Err(Failure::text(EXPIRED));
        }
        Ok(())
    }

    /// A screen, or input, through `hgs terminal <session> --json`. Input
    /// keeps the native status: submitted completes, failed and uncertain
    /// stay so with the native error.
    async fn terminal(
        &self,
        request: &Map<String, Value>,
        operation: &str,
        session: &str,
        payload: &Value,
        request_id: &str,
    ) -> Result<Answer, Failure> {
        let input = operation == "terminal_input";
        let mut native = with_action(payload, if input { "input" } else { "snapshot" });
        if input {
            native["expires_at"] = request["expires_at"].clone();
        }
        let call = Call::new(["terminal", session, "--json"])
            .payload(native)
            .timeout(TERMINAL_TIMEOUT);
        let result = self.native(call).await?.json();
        let statuses: &[&str] = if input {
            &["submitted", "failed", "uncertain"]
        } else {
            &["snapshot"]
        };
        let identity = result.as_object().filter(|result| {
            is_str(result.get("request_id"), request_id)
                && is_str(result.get("name"), session)
                && same(result.get("run_id"), payload, "expected_run_id")
                && same(
                    result.get("conversation_id"),
                    payload,
                    "expected_conversation_id",
                )
        });
        let status = match identity {
            // `status not in {...}` raises TypeError for a list or object.
            Some(result) => match result.get("status") {
                Some(status) => hashable(status).map_err(py)?.as_str(),
                None => None,
            },
            None => None,
        };
        let valid = status.is_some_and(|status| statuses.contains(&status))
            && (!input
                || same(
                    result.get("terminal_binding_id"),
                    payload,
                    "terminal_binding_id",
                ));
        let Some(status) = status.filter(|_| valid) else {
            return Err(Failure::text(TERMINAL_MISMATCH));
        };
        let (state, error) = match status {
            "failed" => ("failed", result.get("error").cloned()),
            "uncertain" => ("uncertain", result.get("error").cloned()),
            _ => ("completed", None),
        };
        Ok((state, result, error.unwrap_or(Value::Null)))
    }

    /// One range of a file of this exact conversation's receipts: the first
    /// range of a file checks the identity and reads it whole, later ranges
    /// of the same exact identity reuse that brief in-memory copy.
    async fn read_attachment(
        &self,
        session: &str,
        payload: &Value,
        request_id: &str,
    ) -> Result<Value, Failure> {
        let key = files::attachment_key(session, payload);
        let cached = self
            .state
            .borrow_mut()
            .reads
            .attachments
            .get(&key, self.clock());
        let entry = match cached {
            Some(entry) => entry,
            None => {
                let mut argv = vec!["inspect".to_owned(), session.to_owned()];
                if let Some(archive) = payload["archive_id"].as_str() {
                    argv.extend(["--archive".into(), archive.into()]);
                }
                argv.push("--skip-processes".into());
                let detail = self.native(Call::new(argv)).await?.json();
                files::check_identity(&detail, session, payload).map_err(Failure::Connector)?;
                let mut call = Call::new(files::attachment_argv(session, payload));
                call.max_output_bytes = Some(files::NATIVE_OUTPUT_BYTES);
                let raw = match self.native(call).await {
                    Ok(raw) => raw.json(),
                    Err(NativeError::Failed(native::FAILED)) => {
                        return Err(Failure::text(files::ATTACHMENT_MISSING))
                    }
                    Err(error) => return Err(error.into()),
                };
                let owned = payload.clone();
                let entry =
                    Arc::new(blocking(move || files::decode_attachment(&raw, &owned)).await?);
                self.state.borrow_mut().reads.attachments.put(
                    key,
                    Arc::clone(&entry),
                    self.clock(),
                );
                entry
            }
        };
        let (session, payload, id) = (session.to_owned(), payload.clone(), request_id.to_owned());
        blocking(move || files::attachment_chunk(&entry, &session, &payload, &id)).await
    }

    /// One range of an explicit file. The first range always captures the
    /// current file; later ranges use only the digest of that capture,
    /// never a path that was silently replaced.
    async fn read_file(
        &self,
        session: &str,
        payload: &Value,
        request_id: &str,
    ) -> Result<Value, Failure> {
        let cached = match payload["sha256"].as_str() {
            Some(digest) => {
                let key = files::file_key(session, payload, digest);
                self.state.borrow_mut().reads.files.get(&key, self.clock())
            }
            None => None,
        };
        let entry = match cached {
            Some(entry) => entry,
            None => {
                let mut call = Call::new(["file-read", session, "--json"])
                    .payload(files::file_native_payload(payload));
                call.max_output_bytes = Some(files::NATIVE_OUTPUT_BYTES);
                let raw = match self.native(call).await {
                    Ok(raw) => raw.json(),
                    Err(NativeError::Failed(native::FAILED)) => {
                        return Err(Failure::text(files::FILE_UNREADABLE))
                    }
                    Err(error) => return Err(error.into()),
                };
                let (owned, name) = (payload.clone(), session.to_owned());
                let entry =
                    Arc::new(blocking(move || files::decode_file(&raw, &name, &owned)).await?);
                let key = files::file_key(session, payload, &entry.sha256);
                self.state
                    .borrow_mut()
                    .reads
                    .files
                    .put(key, Arc::clone(&entry), self.clock());
                entry
            }
        };
        let (session, payload, id) = (session.to_owned(), payload.clone(), request_id.to_owned());
        blocking(move || files::file_chunk(&entry, &session, &payload, &id)).await
    }

    /// A voice message of this exact conversation, which native hgs owns.
    async fn voice(
        &self,
        operation: &str,
        session: &str,
        payload: &Value,
    ) -> Result<Value, Failure> {
        let call = Call::new(["voice", session, "--json"])
            .payload(with_action(payload, speech::action(operation)))
            .timeout(VOICE_TIMEOUT);
        let result = self.native(call).await?.json();
        if !speech::voice_result_valid(operation, &result, payload).map_err(py)? {
            return Err(Failure::text(VOICE_MISMATCH));
        }
        Ok(result)
    }

    /// A speech configuration read or change. Native output that fails the
    /// allowlist never becomes a receipt, and a failed call reports a fixed
    /// text: CLI or SSH stderr can carry provider configuration.
    async fn speech_config(&self, operation: &str, payload: &Value) -> Result<Value, Failure> {
        let call = Call::new(["speech", "--json"])
            .payload(with_action(payload, speech::action(operation)))
            .timeout(SPEECH_CONFIG_TIMEOUT)
            .failure_json();
        let result = match self.native(call).await {
            Ok(result) => result.json(),
            Err(NativeError::Stopping) => return Err(Failure::Stopped),
            Err(_) => return Err(Failure::text(speech::UNCONFIRMED)),
        };
        if !speech::config_result_valid(operation, &result, payload) {
            return Err(Failure::text(CONFIG_MISMATCH));
        }
        Ok(result)
    }

    /// A speech job of this computer. A transcript leaves only through a
    /// status read, never a submission or cancellation receipt.
    async fn speech(&self, operation: &str, payload: &Value) -> Result<Value, Failure> {
        let call = Call::new(["speech", "--json"])
            .payload(with_action(payload, speech::action(operation)))
            .timeout(SPEECH_TIMEOUT);
        let mut result = self.native(call).await?.json();
        if !speech::speech_result_valid(operation, &result, payload).map_err(py)? {
            return Err(Failure::text(SPEECH_MISMATCH));
        }
        if matches!(operation, "speech_submit" | "speech_cancel") {
            if let Some(result) = result.as_object_mut() {
                result.remove("text");
            }
        }
        Ok(result)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn this_module_runs_exactly_its_operations() {
        let mine: Vec<&str> = zerus_relay::protocol::OPERATIONS
            .iter()
            .copied()
            .filter(|operation| handles(operation))
            .collect();
        assert_eq!(mine.len(), 18, "{mine:?}");
        for operation in [
            "inspect",
            "send",
            "history",
            "launch",
            "set_mobile_delivery",
        ] {
            assert!(!handles(operation));
        }
    }

    #[test]
    fn deadlines_follow_python_number_rules() {
        assert_eq!(deadline(Some(&json!(5))), Ok(5.0));
        assert_eq!(deadline(Some(&json!(true))), Err(Failure::text(EXPIRED)));
        assert_eq!(deadline(Some(&json!("5"))), Err(Failure::text(EXPIRED)));
        assert_eq!(deadline(None), Err(Failure::text(EXPIRED)));
        let huge =
            zerus_relay::json::parse(format!("1{}", "0".repeat(400)).as_bytes(), 2, 2).unwrap();
        assert_eq!(deadline(Some(&huge)), Err(Failure::Generic));
        let infinite = zerus_relay::json::parse(b"1e400", 2, 2).unwrap();
        assert_eq!(deadline(Some(&infinite)), Err(Failure::text(EXPIRED)));
    }
}
