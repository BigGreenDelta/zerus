//! The session host process, `hgs.exe __host serve` (D1, D3-D6, D14).
//!
//! `serve` (Windows) first checks that it may serve (`gate`, D2): local
//! sessions are enabled and it runs from a staged image, or the test override
//! applies to an isolated state directory; otherwise it exits before it writes
//! anything. It then takes the host lock of the canonical state directory (a
//! second host exits at once), notes whether it was started inside a job (the
//! hello reports it, D4), joins its own kill-on-close job and fails closed if
//! it cannot (D3), turns off system error boxes for itself and every child,
//! binds the private pipe and serves one thread per connection. When its last
//! session has ended it lingers, decides to exit (from then on a create is
//! refused as `busy` and a new connection ends before its hello is answered),
//! closes its listener and exits once the connections it had accepted have
//! ended, or after `DRAIN` (D3), stopping the recovery worker it supervises
//! once a client asked (D14).
//!
//! `serve_connection` is the protocol of one connection and runs on every
//! platform: the hello (read within `HELLO_TIMEOUT`, the client authenticated
//! after it), negotiation, then requests until the client leaves. `attach`
//! turns the connection into a stream of terminal bytes and control messages
//! (D13), `supervisor_register` into the session's supervisor channel (D10).
use super::{
    link::{write_json, Link},
    protocol::{
        negotiate, Control, Done, ErrorBody, ErrorCode, Hello, HelloAnswer, HelloReply, Kind, Op,
        Request, Response, PROTOCOLS,
    },
    registry::Host,
    session::{Attachment, Launcher, Out, Session},
};
use std::{sync::Arc, thread, time::Duration};

/// How long a new connection may take to say hello.
pub(crate) const HELLO_TIMEOUT: Duration = Duration::from_secs(5);
/// `__host serve`'s exit code when another host holds the lock or the pipe:
/// that host is running or exiting, and a launcher tries again.
pub(crate) const EXIT_OTHER_HOST: i32 = 3;

/// Serve one connection until it ends. `identity` is the hello reply without
/// the negotiated protocol; `authenticate` names the client's process once its
/// hello is read, or gives the refusal to send.
pub(crate) fn serve_connection<L: Launcher>(
    host: &Arc<Host<L>>,
    link: Arc<dyn Link>,
    identity: &HelloReply,
    authenticate: impl FnOnce() -> Result<u32, ErrorBody>,
) {
    // Counted until this connection ends, so an exiting host waits for it.
    let Some((pid, _admitted)) = greet(&*link, identity, authenticate, || host.admit()) else {
        return;
    };
    loop {
        let Ok(Some(frame)) = link.read_frame() else {
            return;
        };
        if frame.kind != Kind::Json {
            return;
        }
        let request = match Request::<Op>::decode(&frame.payload) {
            Ok(request) => request,
            Err(bad) => match bad.response() {
                Some(response) if write_json(&*link, Kind::Json, &response).is_ok() => continue,
                _ => return,
            },
        };
        let id = request.id;
        let answer = |result: Result<serde_json::Value, ErrorBody>| {
            write_json(&*link, Kind::Json, &Response { id, result }).is_ok()
        };
        match request.op {
            Op::Attach(attach) => match host.attach(&attach, pid) {
                Ok((session, client, attached)) => {
                    if answer(Ok(serde_json::to_value(attached).unwrap_or_default())) {
                        relay(host, &link, &session, &client);
                    } else {
                        host.detached(&session, &client);
                    }
                    return;
                }
                Err(error) => {
                    if !answer(Err(error)) {
                        return;
                    }
                }
            },
            Op::SupervisorRegister(registration) => {
                match host.register_supervisor(&registration, pid, link.clone()) {
                    Ok(channel) => {
                        if answer(Ok(serde_json::json!(Done {}))) {
                            // Only now may the host's requests follow the answer.
                            channel.open();
                            channel.serve();
                        }
                        channel.close();
                        return;
                    }
                    Err(error) => {
                        if !answer(Err(error)) {
                            return;
                        }
                    }
                }
            }
            op => {
                if !answer(host.request(op)) {
                    return;
                }
            }
        }
    }
}

/// Read and answer the hello; the client's process ID and its admission once
/// it is accepted. `admit` gives None when the host has decided to exit: the
/// connection then ends before the answer, as if the host were already gone,
/// and the client looks for the next one (D3).
fn greet<A>(
    link: &dyn Link,
    identity: &HelloReply,
    authenticate: impl FnOnce() -> Result<u32, ErrorBody>,
    admit: impl FnOnce() -> Option<A>,
) -> Option<(u32, A)> {
    link.set_read_timeout(Some(HELLO_TIMEOUT)).ok()?;
    let frame = link.read_frame().ok()??;
    let refuse = |body: ErrorBody| {
        let _ = write_json(link, Kind::Json, &HelloAnswer::Refused(body));
        None
    };
    if frame.kind != Kind::Json {
        return refuse(ErrorBody::new(
            ErrorCode::Invalid,
            "a session host connection starts with a hello",
        ));
    }
    let hello: Hello = match frame.json() {
        Ok(hello) => hello,
        Err(error) => {
            return refuse(ErrorBody::new(
                ErrorCode::Invalid,
                format!("invalid session host hello: {error}"),
            ))
        }
    };
    let pid = match authenticate() {
        Ok(pid) => pid,
        Err(refusal) => return refuse(refusal),
    };
    let protocol = match negotiate(hello.protocols, PROTOCOLS) {
        Ok(protocol) => protocol,
        Err(mismatch) => return refuse(mismatch.into()),
    };
    let reply = HelloReply {
        protocol,
        ..identity.clone()
    };
    let admitted = admit()?;
    write_json(link, Kind::Json, &HelloAnswer::Accepted(reply)).ok()?;
    link.set_read_timeout(None).ok()?;
    Some((pid, admitted))
}

/// An attached connection: the session's output to the client from a thread
/// of its own, the client's input and control messages to the session here.
fn relay<L: Launcher>(
    host: &Arc<Host<L>>,
    link: &Arc<dyn Link>,
    session: &Arc<Session>,
    client: &Arc<Attachment>,
) {
    let writer = {
        let (link, client) = (link.clone(), client.clone());
        thread::Builder::new()
            .name(format!("hgs-client-{}", client.id))
            .spawn(move || {
                while let Some(out) = client.next() {
                    let sent = match out {
                        Out::Stream(bytes) => link.write_frame(Kind::Stream, &bytes),
                        Out::Control(control) => write_json(&*link, Kind::Control, &control),
                    };
                    if sent.is_err() {
                        break;
                    }
                }
                // Wakes the reader below once the session ended or the client left.
                link.shutdown();
            })
    };
    let Ok(writer) = writer else {
        host.detached(session, client);
        return;
    };
    while let Ok(Some(frame)) = link.read_frame() {
        match frame.kind {
            Kind::Stream => session.client_input(client.id, frame.payload),
            Kind::Control => match frame.json::<Control>() {
                Ok(Control::Resize(size)) => session.client_resize(client.id, size),
                Ok(Control::Detach) | Err(_) => break,
                // Exit and notices go to clients, never from them.
                Ok(_) => {}
            },
            Kind::Json => break,
        }
    }
    host.detached(session, client);
    let _ = writer.join();
}

#[cfg(windows)]
pub(crate) use self::windows::{existing_state_dir, serve, state_dir, PipeLink};

#[cfg(windows)]
mod windows {
    use super::{
        super::{
            conpty::Api,
            console::{self, ConPtyLauncher},
            endpoint::{self, LockError},
            gate, job,
            log::{self, note},
            pipe::{BindError, PipeListener, PipeStream},
            protocol::{Frame, FrameError, Kind, PROTOCOLS},
            registry::{Host, LINGER},
            session::lock,
            staging::{self, Layout},
            token::{self, Identity},
            worker::{self, Worker},
        },
        *,
    };
    use crate::platform;
    use sha2::{Digest, Sha256};
    use std::{
        fs::File,
        io,
        path::{Path, PathBuf},
        sync::Mutex,
    };
    use windows_sys::Win32::System::Diagnostics::Debug::{
        SetErrorMode, SEM_FAILCRITICALERRORS, SEM_NOGPFAULTERRORBOX, SEM_NOOPENFILEERRORBOX,
    };

    /// `__host serve`'s exit code when it cannot serve.
    const FAILED: i32 = 1;
    /// How long an exiting host waits for the connections it accepted.
    const DRAIN: Duration = Duration::from_secs(2);

    /// A host pipe end as a `Link`.
    pub(crate) struct PipeLink {
        stream: PipeStream,
        writing: Mutex<()>,
    }

    impl PipeLink {
        pub(crate) fn new(stream: PipeStream) -> Self {
            Self {
                stream,
                writing: Mutex::new(()),
            }
        }

        pub(crate) fn stream(&self) -> &PipeStream {
            &self.stream
        }
    }

    impl Link for PipeLink {
        fn read_frame(&self) -> Result<Option<Frame>, FrameError> {
            super::super::protocol::read_frame(&mut &self.stream)
        }

        fn write_frame(&self, kind: Kind, payload: &[u8]) -> Result<(), FrameError> {
            let _one_writer = lock(&self.writing);
            super::super::protocol::write_frame(&mut &self.stream, kind, payload)
        }

        fn set_read_timeout(&self, timeout: Option<Duration>) -> io::Result<()> {
            self.stream.set_read_timeout(timeout)
        }

        fn shutdown(&self) {
            self.stream.shutdown();
        }
    }

    /// The canonical state directory of this user, created when missing.
    pub(crate) fn state_dir() -> io::Result<PathBuf> {
        let root = crate::state::host_state_root();
        platform::private_dir(&root)?;
        platform::canonical(&root)
    }

    /// The canonical state directory of this user; None when it does not
    /// exist, as then no host runs for it.
    pub(crate) fn existing_state_dir() -> io::Result<Option<PathBuf>> {
        match platform::canonical(crate::state::host_state_root()) {
            Ok(state) => Ok(Some(state)),
            Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(None),
            Err(error) => Err(error),
        }
    }

    /// What `admission` found: this host's executable and its SHA-256, and
    /// whether it runs under the test override.
    struct Admitted {
        image: PathBuf,
        hash: String,
        unstaged: bool,
    }

    /// Whether this process may serve (`gate`, D2). Writes nothing, except the
    /// state directory that `HGS_STATE_DIR` names when the test override is
    /// asked for, whose canonical form decides whether it is isolated.
    fn admission() -> Result<Admitted, (i32, String)> {
        let failed = |error: io::Error| (FAILED, format!("cannot check the session host: {error}"));
        let image = std::env::current_exe().map_err(failed)?;
        let hash = image_hash(&image).map_err(failed)?;
        // The state directory is created only when the override is asked for.
        let unstaged = gate::override_requested(std::env::var_os(gate::ALLOW_UNSTAGED).as_deref())
            && gate::explicit_state_dir()
            && gate::unstaged_allowed(&state_dir().map_err(failed)?);
        let staged = Layout::current()
            .and_then(|layout| platform::canonical(layout.images).ok())
            .zip(platform::canonical(&image).ok())
            .is_some_and(|(images, exe)| gate::staged(&exe, &images, &hash));
        gate::admit(staging::enabled(), staged, unstaged, &image)
            .map_err(|refusal| (refusal.exit_code(), refusal.to_string()))?;
        Ok(Admitted {
            image,
            hash,
            unstaged,
        })
    }

    /// The enablement check of a host under the test override.
    fn always() -> bool {
        true
    }

    fn image_hash(image: &Path) -> io::Result<String> {
        let mut hash = Sha256::new();
        io::copy(&mut File::open(image)?, &mut hash)?;
        Ok(hash
            .finalize()
            .iter()
            .fold(String::new(), |mut text, byte| {
                text.push_str(&format!("{byte:02x}"));
                text
            }))
    }

    /// `hgs.exe __host serve`. Runs before `platform::prepare_environment`, so
    /// this process is in no job of hgs's own when it looks (D4).
    pub(crate) fn serve() -> i32 {
        // SAFETY: plain call; the mode is process-wide and every child the host
        // starts inherits it, so none of them shows a system error box.
        unsafe {
            SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX)
        };
        // Its launcher reports the exit code; the message is for a person who
        // started it by hand.
        let admitted = match admission() {
            Ok(admitted) => admitted,
            Err((code, message)) => {
                eprintln!("hgs: {message}");
                return code;
            }
        };
        let state = match state_dir() {
            Ok(state) => state,
            Err(error) => {
                eprintln!("hgs: cannot use the state directory: {error}");
                return FAILED;
            }
        };
        // Appends: a host that finds another one holding the lock notes that.
        let _ = log::open(&state, false);
        let _lock = match endpoint::lock(&state) {
            Ok(lock) => {
                let _ = log::open(&state, true);
                lock
            }
            Err(LockError::Held) => {
                note("another session host holds the lock; exiting");
                return EXIT_OTHER_HOST;
            }
            Err(error) => {
                note(format!("start failed: {error}"));
                return FAILED;
            }
        };
        let placement = match job::join_host_job() {
            Ok(host_job) => {
                let placement = Placement {
                    in_job: host_job.was_in_job,
                    desktop_job: host_job.desktop_job,
                };
                // The job's handle stays open until this process exits; closing
                // it earlier would end the host with it. Its kill-on-close then
                // ends whatever is left (D3).
                std::mem::forget(host_job);
                placement
            }
            Err(error) => {
                note(format!(
                    "start failed: cannot join the session host's own job: {error}"
                ));
                return FAILED;
            }
        };
        match run(&state, placement, admitted) {
            Ok(()) => {
                note("exited");
                0
            }
            Err((code, message)) => {
                note(format!("start failed: {message}"));
                code
            }
        }
    }

    /// Where the host stood before it joined its own job (D4).
    #[derive(Clone, Copy)]
    struct Placement {
        in_job: bool,
        desktop_job: bool,
    }

    fn run(state: &Path, placement: Placement, admitted: Admitted) -> Result<(), (i32, String)> {
        let Placement {
            in_job,
            desktop_job,
        } = placement;
        let failed = |message: String| (FAILED, message);
        let api = Api::get().map_err(|e| failed(format!("no pseudoconsole API: {e}")))?;
        let identity = Identity::current().map_err(|e| failed(e.to_string()))?;
        let sid = identity.user.text().map_err(|e| failed(e.to_string()))?;
        let name = endpoint::resolve(&sid, state).map_err(|e| failed(e.to_string()))?;
        let listener = Arc::new(PipeListener::bind(&name, &identity).map_err(|e| match e {
            BindError::InUse => (EXIT_OTHER_HOST, e.to_string()),
            BindError::Io(_) => failed(e.to_string()),
        })?);
        let pid = std::process::id();
        let reply = HelloReply {
            pid,
            created: platform::process_start_time(pid).unwrap_or_default(),
            image: admitted.image.display().to_string(),
            image_hash: admitted.hash,
            version: env!("CARGO_PKG_VERSION").into(),
            protocol: PROTOCOLS.max,
            capabilities: vec![
                "attach".into(),
                "supervisor-channel".into(),
                "recovery-worker".into(),
                "launch-status".into(),
            ],
            integrity: identity.integrity.clone(),
            windows_session: token::windows_session().map_err(|e| failed(e.to_string()))?,
            in_job,
            desktop_job,
        };
        note(format!(
            "started: image {} conpty {}{} in_job {in_job} desktop_job {desktop_job}{}",
            reply.image,
            api.source().name(),
            api.fallback()
                .map(|why| format!(" ({why})"))
                .unwrap_or_default(),
            if admitted.unstaged {
                format!(" ({}: an isolated test host)", gate::ALLOW_UNSTAGED)
            } else {
                String::new()
            }
        ));
        // A test host needs no marker; any other host stops creating sessions
        // once `hgs session-host uninstall` removed it.
        let enabled: fn() -> bool = if admitted.unstaged {
            always
        } else {
            staging::enabled
        };
        let host = Host::new(
            ConPtyLauncher::new(api),
            pid,
            api.source().name(),
            argv_of,
            enabled,
        );
        host.complete_launch_reports_in(crate::state::launches::folder_in(state));
        host.supervise_recovery_with(|| {
            let cli = worker::stable_cli()?;
            Worker::start("hgs-recovery-worker", move || {
                console::spawn_hidden(&cli, &["recovery".into(), "worker".into()])
            })
            .map_err(|error| error.to_string())
        });
        let accepting = {
            let (host, listener) = (host.clone(), listener.clone());
            thread::Builder::new()
                .name("hgs-host-accept".into())
                .spawn(move || accept(&host, &listener, &identity, &reply))
                .map_err(|e| failed(e.to_string()))?
        };
        host.wait_for_exit(LINGER);
        // Clients that lose this race see no host, or a connection that ends
        // before their hello is answered, and start a new host (D3). Those
        // accepted before the decision may still send their request.
        listener.close();
        let _ = accepting.join();
        host.drain(DRAIN);
        host.stop_workers();
        Ok(())
    }

    /// The command line of another process, for input checks.
    fn argv_of(pid: u32) -> Option<Vec<String>> {
        platform::describe(pid).and_then(|image| image.argv)
    }

    fn accept<L: Launcher>(
        host: &Arc<Host<L>>,
        listener: &PipeListener,
        identity: &Identity,
        reply: &HelloReply,
    ) {
        loop {
            match listener.accept() {
                Ok(stream) => {
                    let (host, identity, reply) = (host.clone(), identity.clone(), reply.clone());
                    let served = thread::Builder::new()
                        .name("hgs-host-connection".into())
                        .spawn(move || {
                            let link = Arc::new(PipeLink::new(stream));
                            let check = link.clone();
                            serve_connection(&host, link, &reply, move || {
                                check
                                    .stream()
                                    .authenticate(&identity)
                                    .map(|client| client.pid)
                                    .map_err(|error| error.refusal())
                            });
                        });
                    if let Err(error) = served {
                        note(format!("cannot serve a connection: {error}"));
                    }
                }
                Err(_) if listener.is_closed() => return,
                Err(error) => {
                    note(format!("accept failed: {error}"));
                    thread::sleep(Duration::from_millis(100));
                }
            }
        }
    }
}
