//! The supervisor's end of the supervisor channel (D10, D11). A `--run`
//! supervisor that the session host started (`HGS_HOST_SESSION` and
//! `HGS_HOST_PANE` name its session and pane) registers the channel before it
//! starts its program and answers the host's requests on a thread of its own
//! until the connection ends:
//!
//! - `input_state`: the console evidence that the host judges together with
//!   its screen model (`channel::judge`): the input mode of `CONIN$`, the
//!   processes attached to the console by image name, and the agent the
//!   supervisor started last (`platform::forward_shutdown`), with whether it
//!   runs and is inside the session's job.
//! - `stop`: D11's pause, Ctrl+Break for the supervisor's process group, which
//!   the agent shares with every tool it started (`platform::break_process_group`).
//!   The supervisor's own handler (`platform::ShutdownGuard`) records it as
//!   SIGTERM and keeps running, so that it can record how the agent ended.
//!
//! Registration is best effort: a supervisor that cannot reach its host still
//! runs its program, and the host then refuses input checks and pauses for the
//! session, as for any session without a supervisor. The answers are built in
//! plain Rust, so their tests run on every platform; the console calls are
//! Windows only.
use super::{
    channel,
    link::Link,
    protocol::{
        ConsoleEvidence, ConsoleProcess, Done, ErrorBody, ErrorCode, FrameError, SupervisorOp,
        SupervisorRegistration,
    },
};
use serde_json::Value;
use std::io;

/// Console input mode flags of `GetConsoleMode`.
pub(crate) const ENABLE_PROCESSED_INPUT: u32 = 0x0001;
pub(crate) const ENABLE_LINE_INPUT: u32 = 0x0002;
pub(crate) const ENABLE_ECHO_INPUT: u32 = 0x0004;

/// The supervisor's agent as its evidence reports it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Agent {
    pub pid: u32,
    pub alive: bool,
    /// Inside the session's job (D9).
    pub in_job: bool,
}

/// The evidence of a console whose input mode is `mode`, with `processes`
/// attached, for the supervisor's `agent`.
pub(crate) fn evidence(
    mode: u32,
    processes: Vec<ConsoleProcess>,
    agent: Option<Agent>,
) -> ConsoleEvidence {
    ConsoleEvidence {
        line_input: mode & ENABLE_LINE_INPUT != 0,
        echo_input: mode & ENABLE_ECHO_INPUT != 0,
        processed_input: mode & ENABLE_PROCESSED_INPUT != 0,
        processes,
        agent_pid: agent.map(|agent| agent.pid),
        agent_alive: agent.is_some_and(|agent| agent.alive),
        agent_in_job: agent.is_some_and(|agent| agent.in_job),
    }
}

/// What a supervisor reads of its console and does to it.
pub(crate) trait Console: Sync {
    fn evidence(&self) -> io::Result<ConsoleEvidence>;
    /// Raise Ctrl+Break for the supervisor's process group.
    fn pause(&self) -> io::Result<()>;
}

/// The supervisor's answer to one request of the host.
pub(crate) fn answer(console: &dyn Console, op: SupervisorOp) -> Result<Value, ErrorBody> {
    let failed = |what: &str, error: &dyn std::fmt::Display| {
        ErrorBody::new(
            ErrorCode::Internal,
            format!("the supervisor cannot {what}: {error}"),
        )
    };
    match op {
        SupervisorOp::InputState => console
            .evidence()
            .map_err(|error| failed("read its console", &error))
            .and_then(|evidence| {
                serde_json::to_value(evidence).map_err(|error| failed("report its console", &error))
            }),
        SupervisorOp::Stop => console
            .pause()
            .map_err(|error| failed("raise Ctrl+Break", &error))
            .map(|()| serde_json::json!(Done {})),
        SupervisorOp::Unsupported => Err(ErrorBody::new(
            ErrorCode::Unsupported,
            "this supervisor does not know the request",
        )),
    }
}

/// Answer the host's requests on `link` until the connection ends.
pub(crate) fn serve(link: &dyn Link, console: &dyn Console) -> Result<(), FrameError> {
    channel::answer_requests(link, |op| answer(console, op))
}

/// The registration of supervisor `pid` for the session and pane that
/// `HGS_HOST_SESSION` and `HGS_HOST_PANE` name (`$N`, `%N`; empty counts as
/// unset). None outside a session of the host, where neither is set.
pub(crate) fn registration(
    pid: u32,
    session: Option<&str>,
    pane: Option<&str>,
) -> Option<Result<SupervisorRegistration, String>> {
    let (session, pane) = (
        session.filter(|value| !value.is_empty()),
        pane.filter(|value| !value.is_empty()),
    );
    if session.is_none() && pane.is_none() {
        return None;
    }
    fn id(value: Option<&str>, sigil: char) -> Option<&str> {
        value.filter(|value| {
            value.strip_prefix(sigil).is_some_and(|number| {
                !number.is_empty() && number.bytes().all(|byte| byte.is_ascii_digit())
            })
        })
    }
    Some(match (id(session, '$'), id(pane, '%')) {
        (Some(session), Some(pane)) => Ok(SupervisorRegistration {
            session: session.into(),
            pane: pane.into(),
            pid,
        }),
        _ => Err(
            "HGS_HOST_SESSION and HGS_HOST_PANE must name a session ($N) and pane (%N) of the \
             session host"
                .into(),
        ),
    })
}

#[cfg(windows)]
pub(crate) use self::windows::open;

#[cfg(windows)]
mod windows {
    use super::{
        super::{client, job, protocol::Role, server},
        *,
    };
    use crate::platform;
    use std::{
        os::windows::io::{AsRawHandle, FromRawHandle, OwnedHandle},
        ptr,
        sync::Arc,
        thread,
        time::Duration,
    };
    use windows_sys::Win32::{
        Foundation::{GENERIC_READ, GENERIC_WRITE, INVALID_HANDLE_VALUE},
        Storage::FileSystem::{CreateFileW, FILE_SHARE_READ, FILE_SHARE_WRITE, OPEN_EXISTING},
        System::Console::{GetConsoleMode, GetConsoleProcessList},
    };

    /// How long the host may take to answer the registration; it waits for a
    /// session that it is still creating.
    const REGISTRATION_TIMEOUT: Duration = Duration::from_secs(15);

    /// Open this supervisor's channel when the session host started it
    /// (`HGS_HOST_SESSION`, `HGS_HOST_PANE`), and answer the host on a thread of
    /// its own from now on. Without a channel the program still runs; the note
    /// goes to the pane.
    pub(crate) fn open() {
        let variable = |name: &str| std::env::var(name).ok();
        let (session, pane) = (variable("HGS_HOST_SESSION"), variable("HGS_HOST_PANE"));
        let Some(registration) =
            registration(std::process::id(), session.as_deref(), pane.as_deref())
        else {
            return;
        };
        let served = registration.and_then(register).and_then(|link| {
            thread::Builder::new()
                .name("hgs-supervisor-channel".into())
                .spawn(move || {
                    let _ = serve(&*link, &Live);
                })
                .map(drop)
                .map_err(|error| error.to_string())
        });
        if let Err(message) = served {
            eprintln!(
                "hgs: this session has no channel to its session host ({message}); its pauses \
                 and input checks are refused"
            );
        }
    }

    fn register(registration: SupervisorRegistration) -> Result<Arc<dyn Link>, String> {
        let state = server::existing_state_dir()
            .map_err(|error| error.to_string())?
            .ok_or("no session host runs")?;
        let connection =
            client::connect(&state, Role::Supervisor).map_err(|error| error.to_string())?;
        connection
            .set_read_timeout(Some(REGISTRATION_TIMEOUT))
            .map_err(|error| error.to_string())?;
        let link = connection
            .register_supervisor(registration)
            .map_err(|error| error.message)?;
        link.set_read_timeout(None)
            .map_err(|error| error.to_string())?;
        Ok(link)
    }

    /// This process's console, as a supervisor sees it.
    pub(super) struct Live;

    impl Console for Live {
        fn evidence(&self) -> io::Result<ConsoleEvidence> {
            let mode = input_mode()?;
            let attached = attached_processes()?;
            let names = platform::process_names()?;
            // A process that exited after the list was read is left out.
            let processes = attached
                .into_iter()
                .filter_map(|pid| {
                    Some(ConsoleProcess {
                        pid,
                        name: names.get(&pid)?.clone(),
                    })
                })
                .collect();
            let agent = platform::supervised_agent().map(|(pid, alive)| Agent {
                pid,
                alive,
                in_job: alive
                    && job::own_job_process_ids()
                        .ok()
                        .flatten()
                        .is_some_and(|members| members.contains(&pid)),
            });
            Ok(evidence(mode, processes, agent))
        }

        fn pause(&self) -> io::Result<()> {
            platform::break_process_group()
        }
    }

    /// The input mode of this process's console (`CONIN$`, whatever standard
    /// input is).
    pub(super) fn input_mode() -> io::Result<u32> {
        let name: Vec<u16> = "CONIN$\0".encode_utf16().collect();
        // SAFETY: `name` is a NUL-terminated wide string; no security
        // attributes and no template file.
        let handle = unsafe {
            CreateFileW(
                name.as_ptr(),
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                ptr::null(),
                OPEN_EXISTING,
                0,
                ptr::null_mut(),
            )
        };
        if handle == INVALID_HANDLE_VALUE {
            return Err(io::Error::last_os_error());
        }
        // SAFETY: CreateFileW opened this handle and nothing else owns it.
        let handle = unsafe { OwnedHandle::from_raw_handle(handle) };
        let mut mode = 0;
        // SAFETY: the handle is the console's input buffer, opened for reading,
        // and `mode` is writable.
        if unsafe { GetConsoleMode(handle.as_raw_handle(), &mut mode) } == 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(mode)
    }

    /// The IDs of the processes attached to this process's console.
    fn attached_processes() -> io::Result<Vec<u32>> {
        let mut list = vec![0u32; 16];
        loop {
            let capacity = u32::try_from(list.len()).map_err(io::Error::other)?;
            // SAFETY: `list` is writable for `capacity` process IDs.
            let count = unsafe { GetConsoleProcessList(list.as_mut_ptr(), capacity) } as usize;
            if count == 0 {
                return Err(io::Error::last_os_error());
            }
            if count <= list.len() {
                list.truncate(count);
                return Ok(list);
            }
            // More processes than room: the count is how many there are now.
            list.resize(count + 8, 0);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{
        atomic::{AtomicUsize, Ordering},
        Mutex,
    };

    struct Fake {
        evidence: Mutex<io::Result<ConsoleEvidence>>,
        pauses: AtomicUsize,
        pause_fails: bool,
    }

    impl Console for Fake {
        fn evidence(&self) -> io::Result<ConsoleEvidence> {
            match &*self.evidence.lock().unwrap() {
                Ok(evidence) => Ok(evidence.clone()),
                Err(error) => Err(io::Error::new(error.kind(), error.to_string())),
            }
        }

        fn pause(&self) -> io::Result<()> {
            self.pauses.fetch_add(1, Ordering::SeqCst);
            if self.pause_fails {
                return Err(io::Error::other("no console"));
            }
            Ok(())
        }
    }

    fn process(pid: u32, name: &str) -> ConsoleProcess {
        ConsoleProcess {
            pid,
            name: name.into(),
        }
    }

    #[test]
    fn evidence_reports_the_mode_flags_and_the_agent() {
        // The console of an idle composer (S2): no line, echo or processed input,
        // with ENABLE_VIRTUAL_TERMINAL_INPUT (0x200) and extended flags set.
        let raw = evidence(
            0x0200 | 0x0080,
            vec![process(10, "hgs.exe"), process(20, "node.exe")],
            Some(Agent {
                pid: 20,
                alive: true,
                in_job: true,
            }),
        );
        assert_eq!(
            raw,
            ConsoleEvidence {
                line_input: false,
                echo_input: false,
                processed_input: false,
                processes: vec![process(10, "hgs.exe"), process(20, "node.exe")],
                agent_pid: Some(20),
                agent_alive: true,
                agent_in_job: true,
            }
        );
        // The default console mode: everything on.
        let cooked = evidence(0x01f7, Vec::new(), None);
        assert!(cooked.line_input && cooked.echo_input && cooked.processed_input);
        assert_eq!(
            (cooked.agent_pid, cooked.agent_alive, cooked.agent_in_job),
            (None, false, false)
        );
        let echo = evidence(ENABLE_ECHO_INPUT, Vec::new(), None);
        assert!(echo.echo_input && !echo.line_input && !echo.processed_input);
        // A crashed agent: still named, no longer alive (S2).
        let crashed = evidence(
            0,
            vec![process(10, "hgs.exe")],
            Some(Agent {
                pid: 20,
                alive: false,
                in_job: false,
            }),
        );
        assert_eq!((crashed.agent_pid, crashed.agent_alive), (Some(20), false));
    }

    #[test]
    fn requests_get_the_evidence_a_pause_or_a_refusal() {
        let raw = evidence(0, vec![process(10, "hgs.exe")], None);
        let console = Fake {
            evidence: Mutex::new(Ok(raw.clone())),
            pauses: AtomicUsize::new(0),
            pause_fails: false,
        };
        assert_eq!(
            answer(&console, SupervisorOp::InputState).unwrap(),
            serde_json::to_value(&raw).unwrap()
        );
        assert_eq!(
            answer(&console, SupervisorOp::Stop).unwrap(),
            serde_json::json!({})
        );
        assert_eq!(console.pauses.load(Ordering::SeqCst), 1);
        let unknown = answer(&console, SupervisorOp::Unsupported).unwrap_err();
        assert_eq!(unknown.code, ErrorCode::Unsupported);

        let broken = Fake {
            evidence: Mutex::new(Err(io::Error::other("no console"))),
            pauses: AtomicUsize::new(0),
            pause_fails: true,
        };
        let error = answer(&broken, SupervisorOp::InputState).unwrap_err();
        assert_eq!(error.code, ErrorCode::Internal);
        assert!(error.message.contains("read its console"), "{error}");
        let error = answer(&broken, SupervisorOp::Stop).unwrap_err();
        assert_eq!(error.code, ErrorCode::Internal);
        assert!(error.message.contains("Ctrl+Break"), "{error}");
    }

    #[test]
    fn only_a_session_of_the_host_registers() {
        assert_eq!(registration(7, None, None), None);
        assert_eq!(registration(7, Some(""), Some("")), None);
        assert_eq!(
            registration(7, Some("$12"), Some("%3")),
            Some(Ok(SupervisorRegistration {
                session: "$12".into(),
                pane: "%3".into(),
                pid: 7,
            }))
        );
        for (session, pane) in [
            (Some("$1"), None),
            (None, Some("%1")),
            (Some("1"), Some("%1")),
            (Some("$1"), Some("%")),
            (Some("$x"), Some("%1")),
            (Some("%1"), Some("$1")),
        ] {
            assert!(
                matches!(registration(7, session, pane), Some(Err(_))),
                "{session:?} {pane:?}"
            );
        }
    }
}

/// The supervisor's console calls on a real pseudoconsole, in the shape the
/// host gives a session: the test binary is the root process of a ConPTY
/// session of an in-process host (a group of its own in a session job), and
/// starts the test binary again as its raw-input agent.
#[cfg(all(windows, test))]
mod windows_tests {
    use super::{
        super::{
            channel::{self, NOT_ATTACHED},
            client::Connection,
            conpty::Api,
            console::ConPtyLauncher,
            job::{Job, Limits},
            link::memory_pair,
            protocol::{
                CreateSession, Created, HelloReply, InputClaim, Integrity, Listed, Op, Role, Size,
                Target,
            },
            registry::Host,
            server::serve_connection,
            spawn::{self, Command, Environment, Flags},
        },
        windows::{input_mode, Live},
        *,
    };
    use crate::platform::{self, ExitInfo};
    use std::{
        env, fs,
        os::windows::io::{AsRawHandle, FromRawHandle, OwnedHandle},
        ptr,
        sync::Arc,
        thread,
        time::{Duration, Instant},
    };
    use windows_sys::Win32::{
        Foundation::{GENERIC_READ, GENERIC_WRITE, INVALID_HANDLE_VALUE},
        Storage::FileSystem::{CreateFileW, FILE_SHARE_READ, FILE_SHARE_WRITE, OPEN_EXISTING},
        System::Console::{GetConsoleMode, SetConsoleMode, ENABLE_VIRTUAL_TERMINAL_INPUT},
    };

    /// Where the supervisor reports, when the parent test runs it.
    const REPORT: &str = "HGS_TEST_SUPERVISOR_REPORT";
    /// Set for the agent the supervisor starts.
    const AGENT: &str = "HGS_TEST_SUPERVISOR_AGENT";
    const LIMIT: Duration = Duration::from_secs(60);
    const MODULE: &str = "host::supervisor::windows_tests::";

    fn until(what: &str, mut done: impl FnMut() -> bool) {
        let deadline = Instant::now() + LIMIT;
        while !done() {
            assert!(Instant::now() < deadline, "timed out waiting for {what}");
            thread::sleep(Duration::from_millis(50));
        }
    }

    /// The test binary running one ignored test of this module.
    fn test_argv(test: &str) -> Vec<String> {
        vec![
            env::current_exe().unwrap().display().to_string(),
            format!("{MODULE}{test}"),
            "--exact".into(),
            "--ignored".into(),
            "--test-threads=1".into(),
        ]
    }

    /// The agent: raw console input as at an idle composer (S2), then nothing
    /// until it is ended, by a kill or by Ctrl+Break's default handler.
    #[test]
    #[ignore = "run by supervisor_child"]
    fn agent_child() {
        if env::var_os(AGENT).is_none() {
            return;
        }
        let name: Vec<u16> = "CONIN$\0".encode_utf16().collect();
        // SAFETY: `name` is a NUL-terminated wide string; no security
        // attributes and no template file.
        let handle = unsafe {
            CreateFileW(
                name.as_ptr(),
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                ptr::null(),
                OPEN_EXISTING,
                0,
                ptr::null_mut(),
            )
        };
        assert!(handle != INVALID_HANDLE_VALUE, "no console input");
        // SAFETY: CreateFileW opened this handle and nothing else owns it.
        let handle = unsafe { OwnedHandle::from_raw_handle(handle) };
        let mut mode = 0;
        // SAFETY: the handle is the console's input buffer and `mode` is writable.
        assert!(unsafe { GetConsoleMode(handle.as_raw_handle(), &mut mode) } != 0);
        let raw = (mode & !(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT))
            | ENABLE_VIRTUAL_TERMINAL_INPUT;
        // SAFETY: the handle is the console's input buffer, opened for writing.
        assert!(unsafe { SetConsoleMode(handle.as_raw_handle(), raw) } != 0);
        thread::sleep(LIMIT);
    }

    /// The supervisor: run by the session that `supervised_session_child` starts.
    #[test]
    #[ignore = "run by supervised_session_child"]
    fn supervisor_child() {
        let Some(report) = env::var_os(REPORT) else {
            return;
        };
        let note = |line: &str| {
            let mut text = fs::read_to_string(&report).unwrap_or_default();
            text.push_str(line);
            text.push('\n');
            fs::write(&report, text).unwrap();
        };
        let _stops = platform::ShutdownGuard::install().unwrap();
        let agent = || {
            let argv = test_argv("agent_child");
            let child = std::process::Command::new(&argv[0])
                .args(&argv[1..])
                .env(AGENT, "1")
                .spawn()
                .unwrap();
            platform::forward_shutdown(&child);
            child
        };
        let root = std::process::id();
        let judged = |pid: u32| {
            let claim = InputClaim {
                target: Target::Pane("%0".into()),
                name: "test/supervisor".into(),
                run_id: "run-1".into(),
                agent: "fake_agent".into(),
                pid: Some(pid),
            };
            channel::judge(&claim, root, &Live.evidence().unwrap(), &|_| None)
        };

        // An idle agent: raw input, alive in the session job, alone with us.
        let mut first = agent();
        until("the agent's raw input", || judged(first.id()).is_ok());
        let evidence = Live.evidence().unwrap();
        assert!(
            evidence.agent_alive && evidence.agent_in_job,
            "{evidence:?}"
        );
        assert!(!evidence.processed_input, "{evidence:?}");
        let attached: Vec<u32> = evidence.processes.iter().map(|p| p.pid).collect();
        assert!(attached.contains(&root) && attached.contains(&first.id()));
        note("accepted");

        // Killed: its raw mode lingers while the supervisor runs on (S2), and
        // the check fails on the agent alone.
        first.kill().unwrap();
        first.wait().unwrap();
        let evidence = Live.evidence().unwrap();
        assert!(!evidence.line_input && !evidence.echo_input, "{evidence:?}");
        assert!(!evidence.agent_alive, "{evidence:?}");
        assert_eq!(judged(first.id()), Err(NOT_ATTACHED));
        assert_eq!(input_mode().unwrap() & ENABLE_LINE_INPUT, 0);
        note("refused");

        // D11's pause: Ctrl+Break for the group ends the agent; the supervisor
        // records it as SIGTERM and runs on.
        let mut second = agent();
        until("the second agent", || judged(second.id()).is_ok());
        assert_eq!(platform::shutdown_signal(), 0);
        Live.pause().unwrap();
        let deadline = Instant::now() + LIMIT;
        let status = loop {
            if let Some(status) = second.try_wait().unwrap() {
                break status;
            }
            assert!(
                Instant::now() < deadline,
                "Ctrl+Break did not end the agent"
            );
            thread::sleep(Duration::from_millis(20));
        };
        // The supervisor's own handler runs on a thread of its own.
        until("the recorded pause", || platform::shutdown_signal() != 0);
        note(&format!(
            "paused {} {}",
            ExitInfo::of(status).exit_code(),
            platform::shutdown_signal()
        ));
        note("done");
    }

    /// An in-process host whose session runs `supervisor_child`; run by the
    /// parent test in a process of its own inside a kill-on-close job, which
    /// ends whatever it leaves.
    #[test]
    #[ignore = "run by a_supervisor_reports_its_console_and_pauses_its_group"]
    fn supervised_session_child() {
        if env::var_os(REPORT).is_none() {
            return;
        }
        let api = Api::get().unwrap();
        let pid = std::process::id();
        let host = Host::new(
            ConPtyLauncher::new(api),
            pid,
            api.source().name(),
            |_| None,
            || true,
        );
        let identity = HelloReply {
            pid,
            created: String::new(),
            image: String::new(),
            image_hash: String::new(),
            version: String::new(),
            protocol: 1,
            capabilities: Vec::new(),
            integrity: Integrity::from_rid(Integrity::MEDIUM),
            windows_session: 0,
            in_job: false,
        };
        let (client, server) = memory_pair();
        {
            let host = host.clone();
            thread::spawn(move || {
                serve_connection(&host, Arc::new(server), &identity, move || Ok(pid))
            });
        }
        let mut connection = Connection::hello(Arc::new(client), Role::Cli).unwrap();
        let work = tempfile::tempdir().unwrap();
        let environment: Vec<String> = env::vars_os()
            .filter_map(|(name, value)| Some(format!("{}={}", name.to_str()?, value.to_str()?)))
            .collect();
        connection
            .call::<Created>(Op::CreateSession(CreateSession {
                launch_id: format!("hgs-supervisor-test-{pid}"),
                name: "test/supervisor".into(),
                cwd: work.path().display().to_string(),
                env: environment,
                argv: test_argv("supervisor_child"),
                size: Size {
                    cols: 120,
                    rows: 30,
                },
            }))
            .unwrap();
        let deadline = Instant::now() + LIMIT * 3;
        while !connection
            .call::<Listed>(Op::List)
            .unwrap()
            .sessions
            .is_empty()
        {
            assert!(Instant::now() < deadline, "the supervisor did not end");
            thread::sleep(Duration::from_millis(100));
        }
    }

    #[test]
    fn a_supervisor_reports_its_console_and_pauses_its_group() {
        let dir = tempfile::tempdir().unwrap();
        let report = dir.path().join("report");
        let mut environment = Environment::current().unwrap();
        environment.set(REPORT, report.to_str().unwrap()).unwrap();
        let argv = test_argv("supervised_session_child");
        let exe = env::current_exe().unwrap();
        // Ends whatever the child leaves behind if this test fails half way.
        let cleanup = Job::new(Limits {
            kill_on_close: true,
            ..Limits::default()
        })
        .unwrap();
        let child = spawn::spawn(&Command {
            application: &exe,
            argv: &argv,
            environment: &environment,
            directory: Some(dir.path()),
            flags: Flags {
                suspended: true,
                no_window: true,
                ..Flags::default()
            },
        })
        .unwrap();
        cleanup.adopt_suspended(&child).unwrap();
        let code = child.wait(LIMIT * 4).unwrap();
        let text = fs::read_to_string(&report).unwrap_or_default();
        assert_eq!(code, Some(0), "{text}");
        // STATUS_CONTROL_C_EXIT reads as SIGINT (130); the supervisor records
        // the pause request as SIGTERM (15).
        assert_eq!(
            text.lines().collect::<Vec<_>>(),
            ["accepted", "refused", "paused 130 15", "done"]
        );
    }
}
