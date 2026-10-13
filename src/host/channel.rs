//! The supervisor channel (D10, D11): a session's `--run` supervisor keeps a
//! connection to the host, which then sends it requests (`SupervisorOp`) and
//! reads its answers. The host forwards pause and terminate requests over it
//! and asks it for the console evidence that `input_state` judges.
//!
//! The judgement mirrors the tmux backend's checks and messages
//! (`state/backend/tmux.rs`, `input_state`): the pane must show the claimed run
//! and session with bracketed paste on, the console input must be raw
//! (`ENABLE_LINE_INPUT` and `ENABLE_ECHO_INPUT` off, the counterparts of
//! `ICANON` and `ECHO`), the tracked agent must be alive, inside the session
//! job and attached to the console, and no other interactive program may be
//! attached: an editor or pager, or a shell started without a command. That
//! last rule replaces the foreground process group check and stays provisional
//! until the host prototype has met agent-launched helpers (D10).
use super::{
    link::{write_json, Link},
    protocol::{
        ConsoleEvidence, ErrorBody, ErrorCode, InputClaim, Kind, Request, Response, SupervisorOp,
    },
    session::lock,
};
use serde_json::Value;
use std::{
    collections::HashMap,
    sync::{
        atomic::{AtomicBool, AtomicU64, Ordering},
        mpsc, Arc, Condvar, Mutex,
    },
    time::{Duration, Instant},
};

/// The tmux backend's refusals, kept word for word.
pub(crate) const NOT_AGENT_INPUT: &str =
    "terminal is not in the expected agent input mode; open Terminal";
pub(crate) const NOT_RAW: &str = "agent does not own an interactive input terminal; open Terminal";
pub(crate) const NOT_ATTACHED: &str = "tracked agent is not attached to the selected terminal";
pub(crate) const MISSING_AGENT: &str = "missing agent process";

/// The host's end of one session's supervisor channel.
pub(crate) struct SupervisorChannel {
    link: Arc<dyn Link>,
    waiting: Mutex<HashMap<u64, mpsc::Sender<Result<Value, ErrorBody>>>>,
    next: AtomicU64,
    /// The supervisor has been told that it is registered; before that, a
    /// request of the host would reach it ahead of that answer.
    ready: Mutex<bool>,
    /// Signalled when the channel opens or closes.
    changed: Condvar,
    closed: AtomicBool,
}

impl SupervisorChannel {
    pub(crate) fn new(link: Arc<dyn Link>) -> Arc<Self> {
        Arc::new(Self {
            link,
            waiting: Mutex::new(HashMap::new()),
            next: AtomicU64::new(1),
            ready: Mutex::new(false),
            changed: Condvar::new(),
            closed: AtomicBool::new(false),
        })
    }

    /// The registration has been answered: requests may go out now.
    pub(crate) fn open(&self) {
        *lock(&self.ready) = true;
        self.changed.notify_all();
    }

    /// Whether requests may go out: registered, answered and not closed.
    pub(crate) fn is_open(&self) -> bool {
        *lock(&self.ready) && !self.is_closed()
    }

    /// Wait at most `timeout` for the channel to open. The supervisor may read
    /// its registration answer, and a client act on it, a moment before the
    /// host's connection thread opens the channel; a closed channel answers at
    /// once.
    pub(crate) fn wait_open(&self, timeout: Duration) -> bool {
        let deadline = Instant::now() + timeout;
        let mut ready = lock(&self.ready);
        while !*ready && !self.is_closed() {
            let left = deadline.saturating_duration_since(Instant::now());
            if left.is_zero() {
                break;
            }
            ready = self
                .changed
                .wait_timeout(ready, left)
                .unwrap_or_else(|poisoned| poisoned.into_inner())
                .0;
        }
        *ready && !self.is_closed()
    }

    fn mark_closed(&self) {
        self.closed.store(true, Ordering::SeqCst);
        let _ready = lock(&self.ready);
        self.changed.notify_all();
    }

    /// Read the supervisor's answers until the connection ends; runs on the
    /// connection's own thread. Requests still waiting then fail.
    pub(crate) fn serve(&self) {
        while let Ok(Some(frame)) = self.link.read_frame() {
            if frame.kind != Kind::Json {
                break;
            }
            let Ok(response) = frame.json::<Response>() else {
                break;
            };
            if let Some(waiting) = lock(&self.waiting).remove(&response.id) {
                let _ = waiting.send(response.result);
            }
        }
        self.mark_closed();
        lock(&self.waiting).clear();
    }

    pub(crate) fn is_closed(&self) -> bool {
        self.closed.load(Ordering::SeqCst)
    }

    /// End the channel: the supervisor reads the end of the connection.
    pub(crate) fn close(&self) {
        self.mark_closed();
        self.link.shutdown();
    }

    /// Ask the supervisor and wait at most `timeout` for its answer.
    pub(crate) fn call(&self, op: SupervisorOp, timeout: Duration) -> Result<Value, ErrorBody> {
        let gone = || {
            ErrorBody::new(
                ErrorCode::Refused,
                "the session's supervisor is no longer connected",
            )
        };
        if self.is_closed() {
            return Err(gone());
        }
        let id = self.next.fetch_add(1, Ordering::Relaxed);
        let (sender, answer) = mpsc::channel();
        lock(&self.waiting).insert(id, sender);
        if write_json(&*self.link, Kind::Json, &Request { id, op }).is_err() {
            lock(&self.waiting).remove(&id);
            return Err(gone());
        }
        match answer.recv_timeout(timeout) {
            Ok(result) => result,
            Err(mpsc::RecvTimeoutError::Timeout) => {
                lock(&self.waiting).remove(&id);
                Err(ErrorBody::new(
                    ErrorCode::Internal,
                    format!(
                        "the session's supervisor did not answer within {} s",
                        timeout.as_secs_f32()
                    ),
                ))
            }
            Err(mpsc::RecvTimeoutError::Disconnected) => Err(gone()),
        }
    }
}

/// The supervisor's side: answer the host's requests with `answer` until the
/// connection ends.
pub(crate) fn answer_requests(
    link: &dyn Link,
    mut answer: impl FnMut(SupervisorOp) -> Result<Value, ErrorBody>,
) -> Result<(), super::protocol::FrameError> {
    while let Some(frame) = link.read_frame()? {
        if frame.kind != Kind::Json {
            continue;
        }
        let response = match Request::<SupervisorOp>::decode(&frame.payload) {
            Ok(request) => Response {
                id: request.id,
                result: answer(request.op),
            },
            Err(bad) => match bad.response() {
                Some(response) => response,
                None => continue,
            },
        };
        write_json(link, Kind::Json, &response)?;
    }
    Ok(())
}

/// What the host knows of a pane for an input check.
pub(crate) struct PaneFacts<'a> {
    pub name: &'a str,
    pub run_marker: &'a str,
    pub dead: bool,
    pub bracketed_paste: bool,
}

/// The pane shows the claimed session and run, in normal mode (the host has
/// no other) with bracketed paste on.
pub(crate) fn check_pane(claim: &InputClaim, pane: &PaneFacts<'_>) -> Result<(), &'static str> {
    if pane.dead
        || pane.run_marker != claim.run_id
        || pane.name != claim.name
        || !pane.bracketed_paste
    {
        return Err(NOT_AGENT_INPUT);
    }
    Ok(())
}

/// Judge the supervisor's console evidence for `claim`. `root` is the pane's
/// root process (the supervisor), and `argv` reads another process's command
/// line, None when it cannot.
pub(crate) fn judge(
    claim: &InputClaim,
    root: u32,
    evidence: &ConsoleEvidence,
    argv: &dyn Fn(u32) -> Option<Vec<String>>,
) -> Result<(), &'static str> {
    let agent = claim.pid.ok_or(MISSING_AGENT)?;
    if evidence.line_input || evidence.echo_input {
        return Err(NOT_RAW);
    }
    if evidence.agent_pid != Some(agent)
        || !evidence.agent_alive
        || !evidence.agent_in_job
        || !evidence
            .processes
            .iter()
            .any(|process| process.pid == agent)
    {
        return Err(NOT_ATTACHED);
    }
    for process in &evidence.processes {
        if process.pid == agent || process.pid == root {
            continue;
        }
        let program = program_stem(&process.name);
        if EDITORS.contains(&program.as_str()) {
            return Err(NOT_AGENT_INPUT);
        }
        if SHELLS.contains(&program.as_str())
            && argv(process.pid).is_none_or(|argv| interactive_shell(&program, &argv))
        {
            return Err(NOT_AGENT_INPUT);
        }
    }
    Ok(())
}

/// Editors and pagers that take over a console's input.
const EDITORS: &[&str] = &[
    "vi",
    "vim",
    "view",
    "nvim",
    "gvim",
    "nano",
    "pico",
    "emacs",
    "emacsclient",
    "micro",
    "hx",
    "helix",
    "kak",
    "joe",
    "ne",
    "mcedit",
    "edit",
    "less",
    "more",
    "most",
    "man",
];

/// Shells, interactive unless started with a command.
const SHELLS: &[&str] = &[
    "cmd",
    "powershell",
    "pwsh",
    "bash",
    "sh",
    "zsh",
    "fish",
    "dash",
    "ksh",
    "tcsh",
    "csh",
    "nu",
];

/// A process image's name as programs are compared on Windows: the file name
/// without a program extension, in lower case (`node.exe` is `node`).
pub(crate) fn program_stem(name: &str) -> String {
    let file = name
        .rsplit(['\\', '/'])
        .next()
        .unwrap_or(name)
        .to_lowercase();
    for extension in [".exe", ".com", ".bat", ".cmd"] {
        if let Some(stem) = file.strip_suffix(extension).filter(|stem| !stem.is_empty()) {
            return stem.to_owned();
        }
    }
    file
}

/// Whether shell `program` (a stem) with `argv` reads commands from the console.
pub(crate) fn interactive_shell(program: &str, argv: &[String]) -> bool {
    let arguments = argv.get(1..).unwrap_or_default();
    match program {
        // `/C` runs one command and exits; `/K` runs one and stays.
        "cmd" => !arguments
            .iter()
            .any(|argument| argument.to_ascii_lowercase().starts_with("/c")),
        "powershell" | "pwsh" => {
            // Options that take a value, by prefix (PowerShell accepts any
            // unambiguous prefix of a parameter name).
            const VALUED: &[&str] = &[
                "executionpolicy",
                "workingdirectory",
                "configurationname",
                "version",
                "windowstyle",
                "inputformat",
                "outputformat",
                "settingsfile",
                "custompipename",
            ];
            let mut command = false;
            let mut no_exit = false;
            let mut iterator = arguments.iter();
            while let Some(argument) = iterator.next() {
                let lower = argument.to_ascii_lowercase();
                let Some(option) = lower.strip_prefix('-').or_else(|| lower.strip_prefix('/'))
                else {
                    // A command (Windows PowerShell) or script file (pwsh).
                    command = true;
                    break;
                };
                let prefix = |full: &str, shortest: usize| {
                    option.len() >= shortest && full.starts_with(option)
                };
                if prefix("noexit", 3) {
                    no_exit = true;
                } else if prefix("command", 1)
                    || prefix("commandwithargs", 8)
                    || option == "cwa"
                    || prefix("file", 1)
                    || option == "e"
                    || option == "ec"
                    || prefix("encodedcommand", 3)
                {
                    command = true;
                    break;
                } else if VALUED.iter().any(|full| prefix(full, 2)) {
                    iterator.next();
                }
            }
            !command || no_exit
        }
        _ => {
            // POSIX shells: `-c` alone or in a cluster, or a script operand.
            let mut iterator = arguments.iter();
            while let Some(argument) = iterator.next() {
                if argument == "--" {
                    return iterator.next().is_none();
                }
                if let Some(long) = argument.strip_prefix("--") {
                    if long == "command" {
                        return false;
                    }
                    if matches!(long, "rcfile" | "init-file") {
                        iterator.next();
                    }
                } else if let Some(short) = argument
                    .strip_prefix('-')
                    .or_else(|| argument.strip_prefix('+'))
                {
                    if short.contains('c') && argument.starts_with('-') {
                        return false;
                    }
                    if short.ends_with('o') || short.ends_with('O') {
                        iterator.next();
                    }
                } else {
                    return false;
                }
            }
            true
        }
    }
}

#[cfg(test)]
mod tests {
    use super::super::{
        link::memory_pair,
        protocol::{ConsoleProcess, Done, Target},
    };
    use super::*;
    use std::thread;

    fn strings(words: &[&str]) -> Vec<String> {
        words.iter().map(|word| word.to_string()).collect()
    }

    #[test]
    fn requests_and_answers_cross_the_channel_in_both_directions() {
        let (host, supervisor) = memory_pair();
        let channel = SupervisorChannel::new(Arc::new(host));
        let reader = {
            let channel = channel.clone();
            thread::spawn(move || channel.serve())
        };
        let supervisor = thread::spawn(move || {
            let mut seen = Vec::new();
            answer_requests(&supervisor, |op| {
                seen.push(op.clone());
                match op {
                    SupervisorOp::Stop => Ok(serde_json::to_value(Done {}).unwrap()),
                    _ => Err(ErrorBody::new(ErrorCode::Unsupported, "no")),
                }
            })
            .unwrap();
            seen
        });
        assert_eq!(
            channel
                .call(SupervisorOp::Stop, Duration::from_secs(5))
                .unwrap(),
            serde_json::json!({})
        );
        let error = channel
            .call(SupervisorOp::InputState, Duration::from_secs(5))
            .unwrap_err();
        assert_eq!(error.code, ErrorCode::Unsupported);
        channel.close();
        reader.join().unwrap();
        assert!(channel.is_closed());
        assert_eq!(
            channel
                .call(SupervisorOp::Stop, Duration::from_secs(1))
                .unwrap_err()
                .code,
            ErrorCode::Refused
        );
        drop(channel);
        assert_eq!(
            supervisor.join().unwrap(),
            [SupervisorOp::Stop, SupervisorOp::InputState]
        );
    }

    #[test]
    fn a_silent_or_departed_supervisor_fails_the_call() {
        let (host, supervisor) = memory_pair();
        let channel = SupervisorChannel::new(Arc::new(host));
        let reader = {
            let channel = channel.clone();
            thread::spawn(move || channel.serve())
        };
        let error = channel
            .call(SupervisorOp::InputState, Duration::from_millis(100))
            .unwrap_err();
        assert_eq!(error.code, ErrorCode::Internal);
        // A request is waiting when the supervisor leaves.
        let waiting = {
            let channel = channel.clone();
            thread::spawn(move || channel.call(SupervisorOp::Stop, Duration::from_secs(20)))
        };
        thread::sleep(Duration::from_millis(100));
        drop(supervisor);
        assert_eq!(
            waiting.join().unwrap().unwrap_err().code,
            ErrorCode::Refused
        );
        reader.join().unwrap();
    }

    fn claim(pid: Option<u32>) -> InputClaim {
        InputClaim {
            target: Target::Pane("%1".into()),
            name: "claude/p/a".into(),
            run_id: "run-1".into(),
            agent: "claude".into(),
            pid,
        }
    }

    fn evidence(processes: &[(u32, &str)]) -> ConsoleEvidence {
        ConsoleEvidence {
            line_input: false,
            echo_input: false,
            processes: processes
                .iter()
                .map(|(pid, name)| ConsoleProcess {
                    pid: *pid,
                    name: (*name).into(),
                })
                .collect(),
            agent_pid: Some(20),
            agent_alive: true,
            agent_in_job: true,
        }
    }

    #[test]
    fn the_pane_must_show_the_claimed_run_with_bracketed_paste() {
        let facts = |name, run_marker, dead, bracketed_paste| PaneFacts {
            name,
            run_marker,
            dead,
            bracketed_paste,
        };
        let claim = claim(Some(20));
        assert_eq!(
            check_pane(&claim, &facts("claude/p/a", "run-1", false, true)),
            Ok(())
        );
        for wrong in [
            facts("claude/p/b", "run-1", false, true),
            facts("claude/p/a", "run-2", false, true),
            facts("claude/p/a", "run-1", true, true),
            facts("claude/p/a", "run-1", false, false),
        ] {
            assert_eq!(check_pane(&claim, &wrong), Err(NOT_AGENT_INPUT));
        }
    }

    #[test]
    fn console_evidence_is_judged_like_the_tmux_checks() {
        let argv = |pid: u32| match pid {
            30 => Some(strings(&["bash.exe", "-lc", "npm test"])),
            31 => Some(strings(&["cmd.exe", "/d", "/c", "dir"])),
            32 => Some(strings(&["cmd.exe"])),
            _ => None,
        };
        let root = 10;
        let attached = [(10, "hgs.exe"), (20, "node.exe")];
        assert_eq!(
            judge(&claim(Some(20)), root, &evidence(&attached), &argv),
            Ok(())
        );
        assert_eq!(
            judge(&claim(None), root, &evidence(&attached), &argv),
            Err(MISSING_AGENT)
        );
        let mut cooked = evidence(&attached);
        cooked.echo_input = true;
        assert_eq!(judge(&claim(Some(20)), root, &cooked, &argv), Err(NOT_RAW));
        let mut crashed = evidence(&[(10, "hgs.exe")]);
        assert_eq!(
            judge(&claim(Some(20)), root, &crashed, &argv),
            Err(NOT_ATTACHED)
        );
        crashed = evidence(&attached);
        crashed.agent_alive = false;
        assert_eq!(
            judge(&claim(Some(20)), root, &crashed, &argv),
            Err(NOT_ATTACHED)
        );
        assert_eq!(
            judge(&claim(Some(21)), root, &evidence(&attached), &argv),
            Err(NOT_ATTACHED)
        );
        // Tools and helpers the agent started with a command are fine.
        let helpers = [
            (10, "hgs.exe"),
            (20, "node.exe"),
            (30, "bash.exe"),
            (31, "cmd.exe"),
            (33, "node.exe"),
            (34, "git.exe"),
        ];
        assert_eq!(
            judge(&claim(Some(20)), root, &evidence(&helpers), &argv),
            Ok(())
        );
        // An editor, a pager, an interactive shell or an unreadable shell is not.
        for other in [
            (40, "vim.exe"),
            (41, "NVIM.EXE"),
            (42, "less.exe"),
            (43, "more.com"),
            (32, "cmd.exe"),
            (44, "pwsh.exe"),
        ] {
            let processes = [(10, "hgs.exe"), (20, "node.exe"), other];
            assert_eq!(
                judge(&claim(Some(20)), root, &evidence(&processes), &argv),
                Err(NOT_AGENT_INPUT),
                "{other:?}"
            );
        }
    }

    #[test]
    fn shells_count_as_interactive_without_a_command() {
        let cases: &[(&str, &[&str], bool)] = &[
            ("cmd", &["cmd.exe"], true),
            ("cmd", &["cmd.exe", "/d", "/q", "/k", "prompt x"], true),
            ("cmd", &["cmd.exe", "/d", "/c", "dir"], false),
            ("cmd", &["cmd.exe", "/Cdir"], false),
            ("powershell", &["powershell.exe", "-NoLogo"], true),
            (
                "powershell",
                &["powershell.exe", "-NoProfile", "-Command", "x"],
                false,
            ),
            ("powershell", &["powershell.exe", "-c", "x"], false),
            (
                "powershell",
                &["powershell.exe", "-NoExit", "-Command", "x"],
                true,
            ),
            ("pwsh", &["pwsh.exe", "-File", "a.ps1"], false),
            ("pwsh", &["pwsh.exe", "a.ps1"], false),
            ("pwsh", &["pwsh.exe", "-ExecutionPolicy", "Bypass"], true),
            ("pwsh", &["pwsh.exe", "-ec", "ZQBjAGgAbwA="], false),
            ("bash", &["bash.exe"], true),
            ("bash", &["bash.exe", "--login", "-i"], true),
            ("bash", &["bash.exe", "-lc", "make"], false),
            ("bash", &["bash.exe", "script.sh"], false),
            ("bash", &["bash.exe", "-o", "vi"], true),
            ("bash", &["bash.exe", "--rcfile", "x", "-i"], true),
            ("sh", &["sh", "--", "script"], false),
            ("fish", &["fish", "--command", "ls"], false),
        ];
        for (program, argv, interactive) in cases {
            assert_eq!(
                interactive_shell(program, &strings(argv)),
                *interactive,
                "{argv:?}"
            );
        }
        assert_eq!(program_stem(r"C:\Windows\System32\CMD.EXE"), "cmd");
        assert_eq!(program_stem("node"), "node");
        assert_eq!(program_stem(".exe"), ".exe");
    }
}
