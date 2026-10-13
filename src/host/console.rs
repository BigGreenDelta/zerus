//! Sessions on ConPTY (D6, D9, D11): the Windows `Launcher` and `Console`.
//!
//! A session's root process starts suspended in its own process group on a new
//! pseudoconsole, joins a session job of its own (no kill-on-close, no
//! breakaway) nested in the host's kill-on-close job, then the console is
//! released so that it ends with its last process, and only then the root
//! process runs. Everything it starts is in the session job.
//!
//! `SPAWN` serializes every child start of the host with `PseudoConsole::new`:
//! the inbox ConPTY starts its console host with inheritable handles while it
//! creates a pseudoconsole, and a process another thread started at that moment
//! with handle inheritance would keep the console's pipes open. The host's own
//! starts inherit nothing, but this keeps the rule simple for every caller.
use super::{
    channel::program_stem,
    conpty::{self, Api, Closing, PseudoConsole},
    job::{Job, Limits},
    protocol::Size,
    session::{lock, Console, LaunchPlan, Launched, Launcher},
    spawn::{self, Child, Command, Environment, Flags},
    worker,
};
use crate::platform;
use std::{
    collections::HashSet,
    io,
    os::windows::io::{AsHandle, FromRawHandle, OwnedHandle},
    path::Path,
    sync::Mutex,
    time::Duration,
};
use windows_sys::Win32::System::Threading::{OpenProcess, PROCESS_QUERY_LIMITED_INFORMATION};

/// Held while a pseudoconsole is created or a child process starts.
pub(crate) static SPAWN: Mutex<()> = Mutex::new(());

/// How long ending a session waits for a closing console.
const CLOSE_WAIT: Duration = Duration::from_secs(6);
/// How long ending a session waits for its job to empty.
const END_WAIT: Duration = Duration::from_secs(5);

fn console_size(size: Size) -> conpty::Size {
    conpty::Size {
        columns: size.cols,
        rows: size.rows,
    }
}

/// Starts sessions on pseudoconsoles of `api`.
pub(crate) struct ConPtyLauncher {
    api: Api,
}

impl ConPtyLauncher {
    pub(crate) fn new(api: Api) -> Self {
        Self { api }
    }
}

impl Launcher for ConPtyLauncher {
    fn launch(&self, plan: &LaunchPlan<'_>) -> io::Result<Launched> {
        let program = plan
            .argv
            .first()
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "no program"))?;
        let (mut console, pipes, child) = {
            let _no_other_start = lock(&SPAWN);
            let (mut console, pipes) = PseudoConsole::new(self.api, console_size(plan.size))?;
            let child = console.spawn(&Command {
                application: Path::new(program),
                argv: plan.argv,
                environment: plan.environment,
                directory: Some(Path::new(plan.cwd)),
                flags: Flags {
                    suspended: true,
                    new_process_group: true,
                    ..Flags::default()
                },
            })?;
            (console, pipes, child)
        };
        // The child is in the host's job already; this job nests inside it.
        let job = match Job::new(Limits::SESSION)
            .and_then(|job| job.assign(child.process.as_handle()).map(|()| job))
        {
            Ok(job) => job,
            Err(error) => {
                let _ = child.terminate(1);
                return Err(error);
            }
        };
        if let Err(error) = console.release().and_then(|_| child.resume()) {
            let _ = job.terminate(1);
            return Err(error);
        }
        Ok(Launched {
            pid: child.pid,
            output: Box::new(pipes.output),
            input: Box::new(pipes.input),
            console: Box::new(ConPtyConsole {
                console: Mutex::new(Some(console)),
                closing: Mutex::new(None),
                child,
                job,
            }),
        })
    }
}

/// A session's pseudoconsole, root process and job.
struct ConPtyConsole {
    /// None once closed.
    console: Mutex<Option<PseudoConsole>>,
    closing: Mutex<Option<Closing>>,
    child: Child,
    job: Job,
}

impl Console for ConPtyConsole {
    fn resize(&self, size: Size) -> io::Result<()> {
        match lock(&self.console).as_ref() {
            Some(console) => console.resize(console_size(size)),
            None => Err(io::Error::other("the pseudoconsole is closed")),
        }
    }

    fn wait(&self) -> Option<u32> {
        loop {
            match self.child.wait(Duration::from_secs(3600)) {
                Ok(Some(code)) => return Some(code),
                Ok(None) => {}
                Err(_) => return None,
            }
        }
    }

    fn wait_exit(&self, timeout: Duration) -> bool {
        matches!(self.child.wait(timeout), Ok(Some(_)))
    }

    fn hang_up(&self) {
        let Some(console) = lock(&self.console).take() else {
            return;
        };
        // ClosePseudoConsole runs on a thread of its own, never the reader's.
        if let Ok(closing) = console.close() {
            *lock(&self.closing) = Some(closing);
        }
    }

    fn end(&self) {
        if let Some(closing) = lock(&self.closing).take() {
            closing.wait(CLOSE_WAIT);
        }
        let _ = self.job.terminate(1);
        let _ = self.job.wait_empty(END_WAIT);
    }

    fn current_command(&self) -> String {
        let root = self.child.pid;
        let members: HashSet<u32> = self
            .job
            .process_ids()
            .map(|ids| ids.into_iter().collect())
            .unwrap_or_default();
        let processes: Vec<(u32, u32, u64)> = platform::process_table()
            .map(|table| {
                table
                    .into_values()
                    .filter(|row| row.parent == root && members.contains(&row.pid))
                    .filter_map(|row| {
                        let identity = platform::process_identity(row.pid)?;
                        Some((row.pid, row.parent, identity.birth.parse().ok()?))
                    })
                    .collect()
            })
            .unwrap_or_default();
        let pid = super::session::foreground(root, &processes);
        platform::describe(pid)
            .map(|image| program_stem(&image.executable))
            .unwrap_or_default()
    }

    fn holds(&self, pid: u32) -> bool {
        if pid == self.child.pid {
            return true;
        }
        // SAFETY: plain call; a handle is returned only on success.
        let process = unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid) };
        if process.is_null() {
            return false;
        }
        // SAFETY: OpenProcess returned a handle that nothing else owns.
        let process = unsafe { OwnedHandle::from_raw_handle(process) };
        self.job.contains(process.as_handle()).unwrap_or(false)
    }
}

/// A hidden console process the host supervises (D14).
struct Hidden(Child);

impl worker::Process for Hidden {
    fn wait(&mut self, timeout: Duration) -> io::Result<Option<u32>> {
        self.0.wait(timeout)
    }

    fn kill(&mut self) {
        let _ = self.0.terminate(1);
    }
}

/// Start `program` with `arguments` on a hidden console of its own, with this
/// process's environment. It runs in the host's job.
pub(crate) fn spawn_hidden(
    program: &Path,
    arguments: &[String],
) -> io::Result<Box<dyn worker::Process>> {
    let environment = Environment::current()?;
    let mut argv = vec![program.display().to_string()];
    argv.extend_from_slice(arguments);
    let _no_other_start = lock(&SPAWN);
    let child = spawn::spawn(&Command {
        application: program,
        argv: &argv,
        environment: &environment,
        directory: None,
        flags: Flags {
            no_window: true,
            ..Flags::default()
        },
    })?;
    Ok(Box::new(Hidden(child)))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::host::{
        client::Connection,
        link::memory_pair,
        protocol::{
            Captured, CreateSession, Created, Done, HelloReply, Integrity, Key, Listed, Op,
            PaneDetails, Role, Target,
        },
        registry::Host,
        server::serve_connection,
    };
    use std::{env, fs, path::PathBuf, sync::Arc, thread, time::Instant};

    /// Where `conpty_session_child` reports, when the parent test runs it.
    const REPORT: &str = "HGS_TEST_CONPTY_SESSION_REPORT";
    const LIMIT: Duration = Duration::from_secs(60);

    fn system32(program: &str) -> PathBuf {
        PathBuf::from(env::var_os("SystemRoot").expect("SystemRoot is set"))
            .join("System32")
            .join(program)
    }

    fn until(what: &str, mut done: impl FnMut() -> bool) {
        let deadline = Instant::now() + LIMIT;
        while !done() {
            assert!(Instant::now() < deadline, "timed out waiting for {what}");
            thread::sleep(Duration::from_millis(50));
        }
    }

    /// Run by `a_cmd_session_runs_on_conpty_through_the_host` in a test process
    /// of its own inside a kill-on-close job, which ends whatever it leaves.
    #[test]
    #[ignore = "run by a_cmd_session_runs_on_conpty_through_the_host"]
    fn conpty_session_child() {
        let Some(report) = env::var_os(REPORT) else {
            return;
        };
        let note = |line: &str| {
            let mut text = fs::read_to_string(&report).unwrap_or_default();
            text.push_str(line);
            text.push('\n');
            fs::write(&report, text).unwrap();
        };
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
        let marker = format!("hgs-conpty-{pid}");
        let work = tempfile::tempdir().unwrap();
        let environment: Vec<String> = env::vars_os()
            .filter_map(|(name, value)| Some(format!("{}={}", name.to_str()?, value.to_str()?)))
            .collect();
        let created: Created = connection
            .call(Op::CreateSession(CreateSession {
                launch_id: marker.clone(),
                name: "cmd/conpty".into(),
                cwd: work.path().display().to_string(),
                env: environment,
                argv: vec![
                    system32("cmd.exe").display().to_string(),
                    "/d".into(),
                    "/q".into(),
                    "/k".into(),
                    format!("prompt {marker}$G"),
                ],
                size: crate::host::protocol::Size {
                    cols: 100,
                    rows: 30,
                },
            }))
            .unwrap();
        let target = || Target::Pane(created.pane.clone());
        let capture = |connection: &mut Connection| {
            connection
                .call::<Captured>(Op::Capture {
                    target: target(),
                    styled: false,
                })
                .unwrap()
                .text
        };
        until("the prompt", || {
            capture(&mut connection).contains(&format!("{marker}>"))
        });
        note("prompt");
        connection
            .call::<Done>(Op::SendKeys {
                target: target(),
                keys: vec![
                    Key::Literal(format!("echo {marker}-echo")),
                    Key::Named("Enter".into()),
                ],
            })
            .unwrap();
        until("the echo", || {
            capture(&mut connection)
                .lines()
                .any(|line| line == format!("{marker}-echo"))
        });
        let details: PaneDetails = connection.call(Op::PaneInfo { target: target() }).unwrap();
        assert_eq!((details.width, details.height), (100, 30));
        assert_eq!(details.pane_pid, created.pid);
        assert_eq!(details.current_command, "cmd");
        note("keys");
        connection
            .call::<Done>(Op::Kill { target: target() })
            .unwrap();
        until("the end", || {
            connection
                .call::<Listed>(Op::List)
                .unwrap()
                .sessions
                .is_empty()
        });
        note("killed");
        note("done");
    }

    #[test]
    fn a_cmd_session_runs_on_conpty_through_the_host() {
        let dir = tempfile::tempdir().unwrap();
        let report = dir.path().join("report");
        let mut environment = Environment::current().unwrap();
        environment.set(REPORT, report.to_str().unwrap()).unwrap();
        let exe = env::current_exe().unwrap();
        let argv: Vec<String> = [
            exe.to_string_lossy().into_owned(),
            "host::console::tests::conpty_session_child".into(),
            "--exact".into(),
            "--ignored".into(),
            "--test-threads=1".into(),
        ]
        .into();
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
        let code = child.wait(Duration::from_secs(180)).unwrap();
        let text = fs::read_to_string(&report).unwrap_or_default();
        assert_eq!(code, Some(0), "{text}");
        assert_eq!(
            text.lines().collect::<Vec<_>>(),
            ["prompt", "keys", "killed", "done"]
        );
    }
}
