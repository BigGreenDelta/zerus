//! The session host's sessions and the requests that act on them (D3, D5, D9,
//! D10, D11, D13).
//!
//! Sessions and panes keep tmux's textual IDs, `$N` and `%N`, from counters that
//! never repeat while the host runs (D9). `create_session` is idempotent by
//! launch ID: a retried request returns the session the first one started,
//! also for a while after that session ended. Every request but `attach` and
//! `supervisor_register`, which turn their connection into a stream, is
//! answered here; the connection handling lives in `server.rs`.
//!
//! The host exits like a tmux server: when its last session has ended, it
//! lingers for `LINGER`, then decides to exit under the registry lock, so a
//! `create_session` that comes later is refused as `busy` and its client starts
//! a new host (D3).
use super::{
    channel::{self, PaneFacts, SupervisorChannel},
    link::Link,
    protocol::{
        Attach, Attached, Captured, ClientEntry, ClientList, ConsoleEvidence, CreateSession,
        Created, Done, EnvValue, ErrorBody, ErrorCode, HostStatus, InputClaim, InputReady, Listed,
        Op, PaneDetails, PaneEntry, Resolved, SessionEntry, Signal, SupervisorOp,
        SupervisorRegistration, Target, RUN_MARKER,
    },
    session::{self, lock, Attachment, LaunchPlan, Launcher, Session},
};
use serde::Serialize;
use serde_json::Value;
use std::{
    collections::{BTreeMap, HashMap, VecDeque},
    path::Path,
    sync::{Arc, Condvar, Mutex, PoisonError},
    thread,
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

/// How long an idle host waits for a new session before it exits.
pub(crate) const LINGER: Duration = Duration::from_secs(5);
/// How long the host waits for a supervisor's answer.
const INPUT_STATE_TIMEOUT: Duration = Duration::from_secs(3);
const STOP_TIMEOUT: Duration = Duration::from_secs(5);
/// D11: a terminate waits this long for the supervisor to record its exit.
const TERMINATE_GRACE: Duration = Duration::from_secs(10);
/// Ended launches that a retried create still finds.
const ENDED_LAUNCHES: usize = 256;

/// When an idle host exits (D3).
#[derive(Debug)]
pub(crate) struct Linger {
    period: Duration,
    idle_since: Option<Instant>,
}

/// What the host does next.
#[derive(Debug, PartialEq, Eq)]
pub(crate) enum Next {
    /// Sessions run: wait for a change.
    Serve,
    /// Idle: exit unless something changes within this time.
    Linger(Duration),
    Exit,
}

impl Linger {
    pub(crate) fn new(period: Duration) -> Self {
        Self {
            period,
            idle_since: None,
        }
    }

    pub(crate) fn next(&mut self, idle: bool, now: Instant) -> Next {
        if !idle {
            self.idle_since = None;
            return Next::Serve;
        }
        let since = *self.idle_since.get_or_insert(now);
        match self
            .period
            .checked_sub(now.saturating_duration_since(since))
        {
            Some(left) if !left.is_zero() => Next::Linger(left),
            _ => Next::Exit,
        }
    }
}

enum Launch {
    Running(u64),
    /// What the request that started it was told, with `existing` set.
    Ended(Created),
}

struct Registry {
    sessions: BTreeMap<u64, Arc<Session>>,
    launches: HashMap<String, Launch>,
    /// Ended launches, oldest first.
    ended: VecDeque<String>,
    next_session: u64,
    next_pane: u64,
    next_client: u64,
    /// Creates between their checks and their sessions' registration.
    starting: usize,
    /// Requests being answered.
    requests: usize,
    /// `stop_host` was accepted.
    stop: bool,
    /// The host has decided to exit: no session starts any more.
    exiting: bool,
    supervisors: HashMap<u64, Arc<SupervisorChannel>>,
}

/// The session host's state, shared by every connection.
pub(crate) struct Host<L: Launcher> {
    launcher: L,
    /// The host's own process ID (`pane_info`'s `server_pid`).
    pid: u32,
    /// `bundled` or `inbox` (D6).
    conpty: String,
    /// Reads another process's command line for input checks; None when it
    /// cannot.
    argv: fn(u32) -> Option<Vec<String>>,
    registry: Mutex<Registry>,
    changed: Condvar,
    /// Creates run one at a time, so names and launch IDs are checked once.
    creating: Mutex<()>,
}

fn error(code: ErrorCode, message: impl Into<String>) -> ErrorBody {
    ErrorBody::new(code, message)
}

fn payload(value: &impl Serialize) -> Result<Value, ErrorBody> {
    serde_json::to_value(value).map_err(|e| error(ErrorCode::Internal, e.to_string()))
}

/// A session name tmux would take: not empty, no control characters.
fn check_name(name: &str) -> Result<(), ErrorBody> {
    if name.is_empty() || name.chars().any(char::is_control) {
        return Err(error(
            ErrorCode::Invalid,
            format!("invalid session name {name:?}"),
        ));
    }
    Ok(())
}

fn unix_now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |time| time.as_secs())
}

/// The number in a `c<N>` client ID.
fn client_number(id: &str) -> Option<u64> {
    id.strip_prefix('c')?.parse().ok()
}

fn client_id(number: u64) -> String {
    format!("c{number}")
}

impl<L: Launcher> Host<L> {
    pub(crate) fn new(
        launcher: L,
        pid: u32,
        conpty: &str,
        argv: fn(u32) -> Option<Vec<String>>,
    ) -> Arc<Self> {
        Arc::new(Self {
            launcher,
            pid,
            conpty: conpty.into(),
            argv,
            registry: Mutex::new(Registry {
                sessions: BTreeMap::new(),
                launches: HashMap::new(),
                ended: VecDeque::new(),
                next_session: 0,
                next_pane: 0,
                next_client: 0,
                starting: 0,
                requests: 0,
                stop: false,
                exiting: false,
                supervisors: HashMap::new(),
            }),
            changed: Condvar::new(),
            creating: Mutex::new(()),
        })
    }

    fn registry(&self) -> std::sync::MutexGuard<'_, Registry> {
        lock(&self.registry)
    }

    fn changed(&self) {
        self.changed.notify_all();
    }

    /// The session `target` names.
    pub(crate) fn find(&self, target: &Target) -> Result<Arc<Session>, ErrorBody> {
        let registry = self.registry();
        let mut sessions = registry.sessions.values();
        let found = match target {
            Target::Name(name) => sessions.find(|session| session.name() == *name),
            Target::Session(id) => sessions.find(|session| session.session_id() == *id),
            Target::Pane(id) => sessions.find(|session| session.pane_id() == *id),
        };
        found.cloned().ok_or_else(|| {
            let message = match target {
                Target::Name(name) | Target::Session(name) => format!("can't find session: {name}"),
                Target::Pane(pane) => format!("can't find pane: {pane}"),
            };
            error(ErrorCode::NotFound, message)
        })
    }

    /// Answer one request. `attach` and `supervisor_register` belong to
    /// `server.rs`, which turns their connections into streams.
    pub(crate) fn request(self: &Arc<Self>, op: Op) -> Result<Value, ErrorBody> {
        self.registry().requests += 1;
        let result = self.answer(op);
        self.registry().requests -= 1;
        self.changed();
        result
    }

    fn answer(self: &Arc<Self>, op: Op) -> Result<Value, ErrorBody> {
        match op {
            Op::CreateSession(request) => payload(&self.create(request)?),
            Op::List => payload(&self.list()),
            Op::ResolveTarget { target } => {
                let session = self.find(&target)?;
                payload(&Resolved {
                    session: session.session_id(),
                    name: session.name(),
                    pane: session.pane_id(),
                })
            }
            Op::PaneInfo { target } => payload(&self.pane_info(&*self.find(&target)?)),
            Op::Capture { target, styled } => payload(&Captured {
                text: self.find(&target)?.capture(styled),
            }),
            Op::SendKeys { target, keys } => {
                self.find(&target)?.send_keys(&keys)?;
                payload(&Done {})
            }
            Op::Paste {
                target,
                text,
                bracketed,
            } => {
                self.find(&target)?.paste(&text, bracketed)?;
                payload(&Done {})
            }
            Op::SetEnvironment {
                target,
                name,
                value,
            } => {
                if name.is_empty() || name.contains('\0') {
                    return Err(error(
                        ErrorCode::Invalid,
                        format!("invalid variable name {name:?}"),
                    ));
                }
                self.find(&target)?.set_variable(&name, value.as_deref());
                payload(&Done {})
            }
            Op::ShowEnvironment { target, name } => payload(&EnvValue {
                value: self.find(&target)?.variable(&name),
            }),
            Op::Rename { target, name } => {
                self.rename(&target, &name)?;
                payload(&Done {})
            }
            Op::Kill { target } => {
                self.find(&target)?.kill();
                payload(&Done {})
            }
            Op::Signal { target, signal } => {
                self.signal(&target, signal)?;
                payload(&Done {})
            }
            Op::InputState(claim) => payload(&self.input_state(&claim)?),
            Op::ListClients { target } => {
                let sessions = match target {
                    Some(target) => vec![self.find(&target)?],
                    None => self.registry().sessions.values().cloned().collect(),
                };
                payload(&ClientList {
                    clients: sessions.iter().flat_map(|s| clients_of(s)).collect(),
                })
            }
            Op::Notify { client, text } => {
                let number = client_number(&client);
                let delivered = number.is_some_and(|number| {
                    let sessions: Vec<_> = self.registry().sessions.values().cloned().collect();
                    sessions.iter().any(|session| session.notify(number, &text))
                });
                if !delivered {
                    return Err(error(
                        ErrorCode::NotFound,
                        format!("can't find client: {client}"),
                    ));
                }
                payload(&Done {})
            }
            Op::HostStatus => {
                let registry = self.registry();
                payload(&HostStatus {
                    sessions: registry.sessions.len() as u32,
                    clients: registry
                        .sessions
                        .values()
                        .map(|session| session.clients().len() as u32)
                        .sum(),
                    conpty: self.conpty.clone(),
                })
            }
            Op::StopHost => {
                let mut registry = self.registry();
                if !registry.sessions.is_empty() || registry.starting > 0 {
                    return Err(error(
                        ErrorCode::Busy,
                        "sessions are running; the session host keeps running",
                    ));
                }
                registry.stop = true;
                payload(&Done {})
            }
            Op::Attach(_) | Op::SupervisorRegister(_) => Err(error(
                ErrorCode::Invalid,
                "this request changes the connection and needs one of its own",
            )),
            Op::Unsupported => Err(error(
                ErrorCode::Unsupported,
                "this session host does not know the request",
            )),
        }
    }

    /// Start a session, or report the one this launch ID started.
    pub(crate) fn create(self: &Arc<Self>, request: CreateSession) -> Result<Created, ErrorBody> {
        let _one_at_a_time = lock(&self.creating);
        let invalid = |message: &str| Err(error(ErrorCode::Invalid, message));
        if request.launch_id.is_empty() {
            return invalid("a session needs a launch ID");
        }
        check_name(&request.name)?;
        let Some(program) = request.argv.first() else {
            return invalid("a session needs a command");
        };
        if !Path::new(program).is_absolute() {
            return invalid("the session's program must be an absolute path");
        }
        if !Path::new(&request.cwd).is_absolute() {
            return invalid("the session's directory must be an absolute path");
        }
        if request.size.cols == 0 || request.size.rows == 0 {
            return invalid("a session needs at least one column and one row");
        }
        let (id, pane) = {
            let mut registry = self.registry();
            match registry.launches.get(&request.launch_id) {
                Some(Launch::Running(id)) => {
                    let session = &registry.sessions[id];
                    return Ok(Created {
                        session: session.session_id(),
                        pane: session.pane_id(),
                        name: session.name(),
                        pid: session.pid,
                        existing: true,
                    });
                }
                Some(Launch::Ended(created)) => return Ok(created.clone()),
                None => {}
            }
            if registry.exiting {
                return Err(error(
                    ErrorCode::Busy,
                    "the session host is exiting; start it again",
                ));
            }
            if registry
                .sessions
                .values()
                .any(|session| session.name() == request.name)
            {
                return Err(error(
                    ErrorCode::Exists,
                    format!("duplicate session: {}", request.name),
                ));
            }
            let ids = (registry.next_session, registry.next_pane);
            registry.next_session += 1;
            registry.next_pane += 1;
            registry.starting += 1;
            ids
        };
        let started = self.launch(&request, id, pane);
        let mut registry = self.registry();
        registry.starting -= 1;
        let created = started.map(|session| {
            registry
                .launches
                .insert(request.launch_id.clone(), Launch::Running(id));
            registry.sessions.insert(id, session.clone());
            Created {
                session: session.session_id(),
                pane: session.pane_id(),
                name: request.name.clone(),
                pid: session.pid,
                existing: false,
            }
        });
        drop(registry);
        self.changed();
        created
    }

    fn launch(
        self: &Arc<Self>,
        request: &CreateSession,
        id: u64,
        pane: u64,
    ) -> Result<Arc<Session>, ErrorBody> {
        let failed = |message: String| error(ErrorCode::Internal, message);
        let environment =
            session::environment(&request.env, &format!("${id}"), &format!("%{pane}"))
                .map_err(|e| error(ErrorCode::Invalid, e.to_string()))?;
        let launched = self
            .launcher
            .launch(&LaunchPlan {
                argv: &request.argv,
                cwd: &request.cwd,
                environment: &environment,
                size: request.size,
            })
            .map_err(|e| failed(format!("cannot start the session: {e}")))?;
        let session = Session::start(
            id,
            pane,
            request.launch_id.clone(),
            request.name.clone(),
            unix_now(),
            request.size,
            launched,
        )
        .map_err(|e| failed(format!("cannot start the session: {e}")))?;
        let host = self.clone();
        let watched = session.clone();
        thread::Builder::new()
            .name(format!("hgs-pane-{pane}-wait"))
            .spawn(move || {
                let code = watched.watch();
                host.ended(&watched, code);
            })
            .map_err(|e| {
                session.kill();
                failed(format!("cannot watch the session: {e}"))
            })?;
        Ok(session)
    }

    /// A session's root process exited and its console and job are gone.
    fn ended(&self, session: &Arc<Session>, code: Option<i32>) {
        let mut registry = self.registry();
        registry.sessions.remove(&session.id);
        registry.launches.insert(
            session.launch_id.clone(),
            Launch::Ended(Created {
                session: session.session_id(),
                pane: session.pane_id(),
                name: session.name(),
                pid: session.pid,
                existing: true,
            }),
        );
        registry.ended.push_back(session.launch_id.clone());
        while registry.ended.len() > ENDED_LAUNCHES {
            if let Some(old) = registry.ended.pop_front() {
                if matches!(registry.launches.get(&old), Some(Launch::Ended(_))) {
                    registry.launches.remove(&old);
                }
            }
        }
        let channel = registry.supervisors.remove(&session.id);
        drop(registry);
        if let Some(channel) = channel {
            channel.close();
        }
        session.finish(code);
        self.changed();
    }

    fn list(&self) -> Listed {
        let sessions: Vec<_> = self.registry().sessions.values().cloned().collect();
        let mut listed = Listed::default();
        for session in &sessions {
            let name = session.name();
            let clients = clients_of(session);
            listed.sessions.push(SessionEntry {
                id: session.session_id(),
                name: name.clone(),
                attached: clients.len() as u64,
                created: session.created,
                launch_id: session.launch_id.clone(),
            });
            listed.panes.push(PaneEntry {
                session: name,
                session_id: session.session_id(),
                pane: session.pane_id(),
                pid: session.pid,
                dead: session.dead(),
                run_marker: session.variable(RUN_MARKER).unwrap_or_default(),
                // Another process's current directory is not readable yet
                // (platform::current_directories); "" means unknown.
                current_path: String::new(),
            });
            listed.clients.extend(clients);
        }
        listed
    }

    fn pane_info(&self, session: &Session) -> PaneDetails {
        let info = session.info();
        let cells = |value: usize| u16::try_from(value).unwrap_or(u16::MAX);
        PaneDetails {
            session_id: session.session_id(),
            session_name: session.name(),
            pane: session.pane_id(),
            server_pid: self.pid,
            pane_pid: session.pid,
            width: cells(info.width),
            height: cells(info.height),
            cursor_x: cells(info.cursor_x),
            cursor_y: cells(info.cursor_y),
            in_mode: false,
            current_command: session.current_command(),
            dead: session.dead(),
            run_marker: session.variable(RUN_MARKER).unwrap_or_default(),
            bracketed_paste: info.bracketed_paste,
            tty: format!("conpty:{}", session.pane_id()),
        }
    }

    fn rename(&self, target: &Target, name: &str) -> Result<(), ErrorBody> {
        check_name(name)?;
        let session = self.find(target)?;
        let registry = self.registry();
        if registry
            .sessions
            .values()
            .any(|other| other.id != session.id && other.name() == name)
        {
            return Err(error(
                ErrorCode::Exists,
                format!("duplicate session: {name}"),
            ));
        }
        session.set_name(name);
        Ok(())
    }

    fn supervisor(&self, session: &Session) -> Result<Arc<SupervisorChannel>, ErrorBody> {
        self.registry()
            .supervisors
            .get(&session.id)
            .filter(|channel| !channel.is_closed())
            .cloned()
            .ok_or_else(|| {
                error(
                    ErrorCode::Refused,
                    format!(
                        "session {} has no supervisor connected to the session host",
                        session.name()
                    ),
                )
            })
    }

    /// D11 through the supervisor channel.
    fn signal(&self, target: &Target, signal: Signal) -> Result<(), ErrorBody> {
        let session = self.find(target)?;
        let channel = self.supervisor(&session)?;
        match signal {
            Signal::Pause => channel.call(SupervisorOp::Stop, STOP_TIMEOUT).map(drop),
            Signal::Terminate => {
                let terminate = move || {
                    let _ = channel.call(SupervisorOp::Stop, STOP_TIMEOUT);
                    if !session.wait_exit(TERMINATE_GRACE) {
                        session.kill();
                    }
                };
                thread::Builder::new()
                    .name("hgs-terminate".into())
                    .spawn(terminate)
                    .map(drop)
                    .map_err(|e| error(ErrorCode::Internal, e.to_string()))
            }
        }
    }

    fn input_state(&self, claim: &InputClaim) -> Result<InputReady, ErrorBody> {
        let refused = |message: &str| error(ErrorCode::Refused, message);
        let session = self.find(&claim.target)?;
        let run_marker = session.variable(RUN_MARKER).unwrap_or_default();
        let name = session.name();
        channel::check_pane(
            claim,
            &PaneFacts {
                name: &name,
                run_marker: &run_marker,
                dead: session.dead(),
                bracketed_paste: session.info().bracketed_paste,
            },
        )
        .map_err(refused)?;
        let channel = self.supervisor(&session)?;
        let evidence: ConsoleEvidence =
            serde_json::from_value(channel.call(SupervisorOp::InputState, INPUT_STATE_TIMEOUT)?)
                .map_err(|e| {
                    error(
                        ErrorCode::Internal,
                        format!("invalid console evidence from the supervisor: {e}"),
                    )
                })?;
        channel::judge(claim, session.pid, &evidence, &self.argv).map_err(refused)?;
        let info = session.info();
        let cells = |value: usize| u16::try_from(value).unwrap_or(u16::MAX);
        Ok(InputReady {
            cursor_x: cells(info.cursor_x),
            cursor_y: cells(info.cursor_y),
        })
    }

    /// Attach a client running as `client_pid` (D13). The connection then
    /// carries `Attachment`'s output and the client's input.
    pub(crate) fn attach(
        &self,
        request: &Attach,
        client_pid: u32,
    ) -> Result<(Arc<Session>, Arc<Attachment>, Attached), ErrorBody> {
        if request.size.cols == 0 || request.size.rows == 0 {
            return Err(error(
                ErrorCode::Invalid,
                "a client needs at least one column and one row",
            ));
        }
        let session = self.find(&request.target)?;
        if let Some(run_id) = &request.run_id {
            if session.variable(RUN_MARKER).as_ref() != Some(run_id) {
                return Err(error(
                    ErrorCode::Refused,
                    format!("session {} runs another run now", session.name()),
                ));
            }
        }
        let number = {
            let mut registry = self.registry();
            registry.next_client += 1;
            registry.next_client
        };
        let client = Attachment::new(number, client_pid, request.size);
        session.attach(client.clone())?;
        let attached = Attached {
            client: client_id(number),
            session: session.session_id(),
            name: session.name(),
            pane: session.pane_id(),
        };
        self.changed();
        Ok((session, client, attached))
    }

    /// A client left its session.
    pub(crate) fn detached(&self, session: &Session, client: &Attachment) {
        session.detach(client.id);
        self.changed();
    }

    /// Accept `link` as the supervisor channel of a session (D10), from a
    /// client running as `client_pid`: the session's root process or one of
    /// its descendants in the session's job.
    pub(crate) fn register_supervisor(
        &self,
        registration: &SupervisorRegistration,
        client_pid: u32,
        link: Arc<dyn Link>,
    ) -> Result<Arc<SupervisorChannel>, ErrorBody> {
        let session = self.find(&Target::Session(registration.session.clone()))?;
        if session.pane_id() != registration.pane {
            return Err(error(
                ErrorCode::NotFound,
                format!("can't find pane: {}", registration.pane),
            ));
        }
        if registration.pid != client_pid || !session.holds(client_pid) {
            return Err(error(
                ErrorCode::Refused,
                "only a process of the session may supervise it",
            ));
        }
        let mut registry = self.registry();
        if registry
            .supervisors
            .get(&session.id)
            .is_some_and(|channel| !channel.is_closed())
        {
            return Err(error(
                ErrorCode::Exists,
                format!("session {} has a supervisor already", session.name()),
            ));
        }
        let channel = SupervisorChannel::new(link);
        registry.supervisors.insert(session.id, channel.clone());
        Ok(channel)
    }

    /// Block until the host should exit: no session has run for `linger`, or
    /// `stop_host` was accepted while none ran. From then on no session starts.
    pub(crate) fn wait_for_exit(&self, linger: Duration) {
        let mut linger = Linger::new(linger);
        let mut registry = self.registry();
        loop {
            let idle = registry.sessions.is_empty() && registry.starting == 0;
            if std::mem::take(&mut registry.stop) && idle {
                registry.exiting = true;
                return;
            }
            registry = match linger.next(idle, Instant::now()) {
                Next::Exit => {
                    registry.exiting = true;
                    return;
                }
                Next::Linger(left) => {
                    self.changed
                        .wait_timeout(registry, left)
                        .unwrap_or_else(PoisonError::into_inner)
                        .0
                }
                Next::Serve => self
                    .changed
                    .wait(registry)
                    .unwrap_or_else(PoisonError::into_inner),
            };
        }
    }

    /// Wait at most `timeout` for requests being answered to finish.
    pub(crate) fn drain(&self, timeout: Duration) {
        let deadline = Instant::now() + timeout;
        let mut registry = self.registry();
        while registry.requests > 0 {
            let left = deadline.saturating_duration_since(Instant::now());
            if left.is_zero() {
                return;
            }
            registry = self
                .changed
                .wait_timeout(registry, left)
                .unwrap_or_else(PoisonError::into_inner)
                .0;
        }
    }

    pub(crate) fn exiting(&self) -> bool {
        self.registry().exiting
    }
}

fn clients_of(session: &Session) -> Vec<ClientEntry> {
    let name = session.name();
    session
        .clients()
        .into_iter()
        .map(|(number, pid)| ClientEntry {
            id: client_id(number),
            session: name.clone(),
            pid,
        })
        .collect()
}
