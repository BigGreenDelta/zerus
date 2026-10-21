//! The session host's client library (D3-D5, D13).
//!
//! `Connection` speaks protocol 1 over any `Link`: the hello, requests and
//! their answers, an attach stream and the supervisor registration. On Windows,
//! `connect` reaches a running host and never starts one, so a listing reports
//! no host as no live sessions, and only `ConnectError::NoHost` means that no
//! host runs; `connect_or_start` starts a host when none runs, outside this
//! process's job (D4), and retries while a host is starting or exiting (D3).
//! Both verify that the host that answers the hello is the process serving the
//! pipe; `connect_or_start` refuses a host that runs inside a job. Which
//! executable it starts is its caller's choice: the staged image (`gate`).
use super::{
    link::{write_json, Link},
    protocol::{
        Attach, Attached, Control, ErrorBody, ErrorCode, FrameError, Hello, HelloAnswer,
        HelloReply, Kind, Op, Request, Response, Role, Size, SupervisorRegistration, PROTOCOLS,
    },
};
use serde::de::DeserializeOwned;
use serde_json::Value;
use std::{fmt, io, sync::Arc, time::Duration};

/// How long a client waits for the answer to its hello.
const HELLO_ANSWER_TIMEOUT: Duration = Duration::from_secs(10);

/// Why there is no connection to a host.
#[derive(Debug)]
pub(crate) enum ClientError {
    /// No host runs (D3): the only case a listing may treat as no sessions.
    NoHost,
    /// The pipe could not be reached or verified.
    Connect(String),
    /// The host refused the hello.
    Refused(ErrorBody),
    /// The host closed the connection before it answered the hello.
    Closed,
    /// The host broke the protocol.
    Protocol(String),
    /// The host runs inside a job, so its sessions would end with that job (D4).
    InJob,
    /// No host could be started.
    Start(String),
}

impl fmt::Display for ClientError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            Self::NoHost => f.write_str("no session host runs"),
            Self::Closed => f.write_str("the session host closed the connection during the hello"),
            Self::Connect(message) | Self::Protocol(message) | Self::Start(message) => {
                f.write_str(message)
            }
            Self::Refused(error) => f.write_str(&error.message),
            Self::InJob => f.write_str(
                "the session host runs inside a job that does not let it break away, so its \
                 sessions would end with that job; start hgs outside that job",
            ),
        }
    }
}

impl std::error::Error for ClientError {}

fn lost(error: impl fmt::Display) -> ErrorBody {
    ErrorBody::new(
        ErrorCode::Internal,
        format!("the session host connection failed: {error}"),
    )
}

/// A connection after an accepted hello.
pub(crate) struct Connection {
    link: Arc<dyn Link>,
    host: HelloReply,
    next: u64,
}

impl Connection {
    /// Say hello as `role` and read the host's answer, waiting at most
    /// `HELLO_ANSWER_TIMEOUT` for it.
    pub(crate) fn hello(link: Arc<dyn Link>, role: Role) -> Result<Self, ClientError> {
        let early = |error: FrameError| match error {
            FrameError::Io(_) | FrameError::Truncated => ClientError::Closed,
            other => ClientError::Protocol(format!("invalid session host hello answer: {other}")),
        };
        let unusable = |error: io::Error| {
            ClientError::Connect(format!("the session host connection failed: {error}"))
        };
        link.set_read_timeout(Some(HELLO_ANSWER_TIMEOUT))
            .map_err(unusable)?;
        write_json(&*link, Kind::Json, &Hello::new(role)).map_err(early)?;
        let frame = link
            .read_frame()
            .map_err(early)?
            .ok_or(ClientError::Closed)?;
        link.set_read_timeout(None).map_err(unusable)?;
        if frame.kind != Kind::Json {
            return Err(ClientError::Protocol(
                "the session host answered the hello with stream data".into(),
            ));
        }
        match frame.json::<HelloAnswer>() {
            // The host chooses from the range this hello offered; any other
            // major is one this build cannot speak.
            Ok(HelloAnswer::Accepted(host))
                if !(PROTOCOLS.min..=PROTOCOLS.max).contains(&host.protocol) =>
            {
                Err(ClientError::Protocol(format!(
                    "the session host chose protocol {}, which hgs {} does not speak",
                    host.protocol,
                    env!("CARGO_PKG_VERSION")
                )))
            }
            Ok(HelloAnswer::Accepted(host)) => Ok(Self {
                link,
                host,
                next: 1,
            }),
            Ok(HelloAnswer::Refused(error)) => Err(ClientError::Refused(error)),
            Err(error) => Err(ClientError::Protocol(format!(
                "invalid session host hello answer: {error}"
            ))),
        }
    }

    /// The host's identity from its hello answer.
    pub(crate) fn host(&self) -> &HelloReply {
        &self.host
    }

    /// How long a request waits for its answer; None (the default) waits for
    /// ever. A request that timed out leaves the connection out of step.
    pub(crate) fn set_read_timeout(&self, timeout: Option<Duration>) -> io::Result<()> {
        self.link.set_read_timeout(timeout)
    }

    /// Send `op` and wait for its answer.
    pub(crate) fn request(&mut self, op: Op) -> Result<Value, ErrorBody> {
        let id = self.next;
        self.next += 1;
        write_json(&*self.link, Kind::Json, &Request { id, op }).map_err(lost)?;
        let frame = self
            .link
            .read_frame()
            .map_err(lost)?
            .ok_or_else(|| lost("the host closed the connection"))?;
        if frame.kind != Kind::Json {
            return Err(lost("the host answered with stream data"));
        }
        let response: Response = frame.json().map_err(lost)?;
        if response.id != id {
            return Err(lost(format!(
                "the answer to request {id} came as {}",
                response.id
            )));
        }
        response.result
    }

    /// `request` with the answer's payload as `T`.
    pub(crate) fn call<T: DeserializeOwned>(&mut self, op: Op) -> Result<T, ErrorBody> {
        Response {
            id: 0,
            result: self.request(op),
        }
        .payload()
    }

    /// Attach to a session; the connection then carries its stream.
    pub(crate) fn attach(mut self, attach: Attach) -> Result<(Attached, AttachStream), ErrorBody> {
        let attached = self.call(Op::Attach(attach))?;
        Ok((attached, AttachStream { link: self.link }))
    }

    /// Offer this connection as a session's supervisor channel (D10); answer
    /// the host's requests on the returned link with `channel::answer_requests`.
    pub(crate) fn register_supervisor(
        mut self,
        registration: SupervisorRegistration,
    ) -> Result<Arc<dyn Link>, ErrorBody> {
        self.request(Op::SupervisorRegister(registration))?;
        Ok(self.link)
    }
}

/// What an attached client receives.
#[derive(Debug, PartialEq, Eq)]
pub(crate) enum Event {
    Output(Vec<u8>),
    Control(Control),
}

/// An attached connection (D13). One thread may wait in `next` while another
/// sends.
#[derive(Clone)]
pub(crate) struct AttachStream {
    link: Arc<dyn Link>,
}

impl AttachStream {
    /// Bytes typed in the client's terminal.
    pub(crate) fn send(&self, bytes: &[u8]) -> Result<(), FrameError> {
        self.link.write_frame(Kind::Stream, bytes)
    }

    /// The client's terminal has a new size.
    pub(crate) fn resize(&self, size: Size) -> Result<(), FrameError> {
        write_json(&*self.link, Kind::Control, &Control::Resize(size))
    }

    /// Leave the session; it runs on. The host then closes the connection.
    pub(crate) fn detach(&self) -> Result<(), FrameError> {
        write_json(&*self.link, Kind::Control, &Control::Detach)
    }

    /// End this side of the connection: a thread waiting in `next` gets the
    /// end, and later sends fail.
    pub(crate) fn close(&self) {
        self.link.shutdown();
    }

    /// The next output or control message; None once the host closed the
    /// connection (after `Control::Exit`, a detach, or the host's death).
    pub(crate) fn next(&self) -> Result<Option<Event>, FrameError> {
        loop {
            let Some(frame) = self.link.read_frame()? else {
                return Ok(None);
            };
            match frame.kind {
                Kind::Stream => return Ok(Some(Event::Output(frame.payload))),
                Kind::Control => match frame.json::<Control>() {
                    Ok(Control::Unknown) => {}
                    Ok(control) => return Ok(Some(Event::Control(control))),
                    Err(error) => return Err(std::io::Error::from(error).into()),
                },
                Kind::Json => {}
            }
        }
    }
}

#[cfg(windows)]
pub(crate) use self::windows::{connect, connect_or_start};

#[cfg(windows)]
mod windows {
    use super::{
        super::{
            endpoint, gate,
            log::LOG_FILE,
            pipe::{self, ConnectError},
            server::{PipeLink, EXIT_OTHER_HOST},
            spawn::{spawn_detached, Child, Command, Environment, Flags, LaunchError},
            token,
        },
        *,
    };
    use std::{
        path::{Path, PathBuf},
        thread,
        time::Instant,
    };
    use windows_sys::Win32::Foundation::{ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND};

    /// How long one connect waits while every pipe instance is busy.
    const BUSY_WAIT: Duration = Duration::from_secs(5);
    /// How long `connect_or_start` keeps trying.
    const START_DEADLINE: Duration = Duration::from_secs(20);

    /// What one attempt found.
    enum Attempt {
        Connected(Connection),
        /// Nothing serves the pipe (D3: the only "no host").
        Absent,
        /// The host closed the connection before its hello answer: it is
        /// exiting, and gone soon.
        Closing,
        /// Every pipe instance stayed busy (an exiting host keeps its connected
        /// instances while it stops listening).
        Busy,
        Failed(ClientError),
    }

    fn attempt(state_dir: &Path, role: Role, deadline: Instant) -> Attempt {
        let secret = match endpoint::existing_secret(state_dir) {
            Ok(Some(secret)) => secret,
            Ok(None) => return Attempt::Absent,
            Err(error) => return Attempt::Failed(ClientError::Connect(error.to_string())),
        };
        let sid = match token::user_sid() {
            Ok(sid) => sid,
            Err(error) => return Attempt::Failed(ClientError::Connect(error.to_string())),
        };
        let name = endpoint::pipe_name(&sid, state_dir, &secret);
        let stream = match pipe::connect(&name, deadline) {
            Ok(stream) => stream,
            Err(ConnectError::NoHost) => return Attempt::Absent,
            Err(ConnectError::Busy) => return Attempt::Busy,
            Err(error) => return Attempt::Failed(ClientError::Connect(error.to_string())),
        };
        let server = match stream.server_pid() {
            Ok(pid) => pid,
            Err(error) => return Attempt::Failed(ClientError::Connect(error.to_string())),
        };
        match Connection::hello(Arc::new(PipeLink::new(stream)), role) {
            Err(ClientError::Closed) => Attempt::Closing,
            Err(error) => Attempt::Failed(error),
            Ok(connection) if connection.host().pid != server => {
                Attempt::Failed(ClientError::Connect(format!(
                    "the session host answered as process {} but process {server} serves its pipe",
                    connection.host().pid
                )))
            }
            Ok(connection) => Attempt::Connected(connection),
        }
    }

    /// Connect to the host of the canonical `state_dir`; never starts one. A
    /// host that is exiting is waited for until it is gone.
    pub(crate) fn connect(state_dir: &Path, role: Role) -> Result<Connection, ClientError> {
        let deadline = Instant::now() + BUSY_WAIT;
        loop {
            match attempt(state_dir, role, deadline) {
                Attempt::Connected(connection) => return Ok(connection),
                Attempt::Absent => return Err(ClientError::NoHost),
                Attempt::Closing if Instant::now() < deadline => {
                    thread::sleep(Duration::from_millis(50))
                }
                Attempt::Closing => return Err(ClientError::Closed),
                Attempt::Busy => return Err(ClientError::Connect(ConnectError::Busy.to_string())),
                Attempt::Failed(error) => return Err(error),
            }
        }
    }

    /// Why `start` started nothing.
    enum StartError {
        /// The image is gone: a later install retired it (D2).
        Missing(ClientError),
        Failed(ClientError),
    }

    /// Start `image` as the host of `state_dir` (D4): outside this process's
    /// job, hidden, in its own process group, in the state directory.
    fn start(state_dir: &Path, image: &Path) -> Result<Child, StartError> {
        let failed = |error: &dyn fmt::Display| {
            StartError::Failed(ClientError::Start(format!(
                "cannot start the session host: {error}"
            )))
        };
        let mut environment = Environment::current_with_optional_hint_controls()
            .map_err(|e| failed(&e))?;
        let state = state_dir
            .to_str()
            .ok_or_else(|| failed(&"the state directory is not Unicode"))?;
        environment
            .set("HGS_STATE_DIR", state)
            .map_err(|e| failed(&e))?;
        let argv = vec![
            image.display().to_string(),
            "__host".to_owned(),
            "serve".to_owned(),
        ];
        spawn_detached(&Command {
            application: image,
            argv: &argv,
            environment: &environment,
            directory: Some(state_dir),
            flags: Flags::default(),
        })
        .map_err(|error| match &error {
            LaunchError::Spawn(io)
                if [ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND]
                    .map(|code| Some(code as i32))
                    .contains(&io.raw_os_error()) =>
            {
                StartError::Missing(ClientError::Start(format!(
                    "cannot start the session host {}: {error}",
                    image.display()
                )))
            }
            _ => failed(&error),
        })
    }

    /// Connect to the host of the canonical `state_dir`, starting the
    /// executable `image` gives as the host when none runs. `image` is asked
    /// again before each start, so a launcher that read the pointer of an image
    /// a later install retired starts the new one (D2). A host that is still
    /// starting, or one that is exiting and holds the pipe or the lock, is
    /// waited for. A host that refuses to serve (`gate`) is reported.
    pub(crate) fn connect_or_start(
        state_dir: &Path,
        role: Role,
        image: &dyn Fn() -> Result<PathBuf, ClientError>,
    ) -> Result<Connection, ClientError> {
        let deadline = Instant::now() + START_DEADLINE;
        let mut started: Option<Child> = None;
        // The image whose start found it missing.
        let mut missing: Option<PathBuf> = None;
        loop {
            let absent = match attempt(state_dir, role, deadline.min(Instant::now() + BUSY_WAIT)) {
                // A host that shares the desktop shell's job ends no earlier
                // than the desktop does (D4).
                Attempt::Connected(connection)
                    if connection.host().in_job && !connection.host().desktop_job =>
                {
                    return Err(ClientError::InJob)
                }
                Attempt::Connected(connection) => return Ok(connection),
                Attempt::Failed(error) => return Err(error),
                // An exiting host goes away first; its successor starts then.
                Attempt::Closing | Attempt::Busy => false,
                Attempt::Absent => true,
            };
            if Instant::now() >= deadline {
                return Err(ClientError::Start(
                    "the session host did not start in time".into(),
                ));
            }
            if !absent {
                thread::sleep(Duration::from_millis(50));
                continue;
            }
            // Start a host unless the one started last is still coming up. A
            // host that found another one holding the lock or the pipe exits
            // with EXIT_OTHER_HOST: that one is exiting, so try again.
            let running = match started.as_ref().map(|child| child.wait(Duration::ZERO)) {
                Some(Ok(None)) => true,
                Some(Ok(Some(code))) if code as i32 != EXIT_OTHER_HOST => {
                    if let Some(refusal) = gate::launch_refusal(code as i32) {
                        return Err(ClientError::Start(format!(
                            "the session host refused to start: {refusal}"
                        )));
                    }
                    return Err(ClientError::Start(format!(
                        "the session host exited with {code:#x} while starting; see {} in {}",
                        LOG_FILE,
                        state_dir.display()
                    )));
                }
                Some(Ok(Some(_))) => {
                    thread::sleep(Duration::from_millis(200));
                    false
                }
                Some(Err(_)) | None => false,
            };
            if !running {
                let image = image()?;
                match start(state_dir, &image) {
                    Ok(child) => started = Some(child),
                    // Read the pointer again, once per image that went missing.
                    Err(StartError::Missing(_)) if missing.as_ref() != Some(&image) => {
                        missing = Some(image);
                        continue;
                    }
                    Err(StartError::Missing(error) | StartError::Failed(error)) => {
                        return Err(error)
                    }
                }
            }
            thread::sleep(Duration::from_millis(50));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::super::{link::memory_pair, protocol::Integrity};
    use super::*;
    use std::thread;

    fn identity() -> HelloReply {
        HelloReply {
            pid: 7,
            created: "1".into(),
            image: "hgs".into(),
            image_hash: "00".into(),
            version: "1".into(),
            protocol: 1,
            capabilities: Vec::new(),
            integrity: Integrity::from_rid(Integrity::MEDIUM),
            windows_session: 1,
            in_job: false,
            desktop_job: false,
        }
    }

    #[test]
    fn a_refused_or_broken_hello_is_reported() {
        let (client, host) = memory_pair();
        let server = thread::spawn(move || {
            host.read_frame().unwrap().unwrap();
            write_json(
                &host,
                Kind::Json,
                &HelloAnswer::Refused(ErrorBody::new(ErrorCode::Refused, "elevated")),
            )
            .unwrap();
        });
        let error = Connection::hello(Arc::new(client), Role::Cli)
            .err()
            .unwrap();
        assert!(matches!(&error, ClientError::Refused(body) if body.message == "elevated"));
        server.join().unwrap();

        let (client, host) = memory_pair();
        drop(host);
        assert!(matches!(
            Connection::hello(Arc::new(client), Role::Cli),
            Err(ClientError::Closed)
        ));

        // Answers must match their requests.
        let (client, host) = memory_pair();
        let server = thread::spawn(move || {
            host.read_frame().unwrap().unwrap();
            write_json(&host, Kind::Json, &HelloAnswer::Accepted(identity())).unwrap();
            host.read_frame().unwrap().unwrap();
            write_json(&host, Kind::Json, &Response::ok(9, &serde_json::json!({}))).unwrap();
            host.read_frame().unwrap();
        });
        let mut connection = Connection::hello(Arc::new(client), Role::Cli).unwrap();
        assert_eq!(connection.host().pid, 7);
        let error = connection.request(Op::List).unwrap_err();
        assert_eq!(error.code, ErrorCode::Internal);
        assert!(error.message.contains("came as 9"), "{}", error.message);
        drop(connection);
        server.join().unwrap();
        assert_eq!(ClientError::NoHost.to_string(), "no session host runs");
    }

    #[test]
    fn a_protocol_outside_the_offered_range_is_refused() {
        let (client, host) = memory_pair();
        let server = thread::spawn(move || {
            host.read_frame().unwrap().unwrap();
            let chosen = HelloReply {
                protocol: PROTOCOLS.max + 1,
                ..identity()
            };
            write_json(&host, Kind::Json, &HelloAnswer::Accepted(chosen)).unwrap();
        });
        let error = Connection::hello(Arc::new(client), Role::Cli)
            .err()
            .unwrap();
        assert!(
            matches!(&error, ClientError::Protocol(message) if message.contains("does not speak")),
            "{error}"
        );
        server.join().unwrap();
    }
}
