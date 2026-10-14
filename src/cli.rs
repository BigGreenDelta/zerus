//! Public hgs CLI. tmux remains the terminal supervisor; durable agent identity belongs to state.
use crate::{
    config::{self, Config},
    platform::{self, TabGuard},
    state::{
        self,
        backend::{DryRun, LaunchSpec, PaneField, Target, TerminalBackend},
    },
};
use serde_json::{json, Value};
#[cfg(unix)]
use std::process::Command;
use std::{
    env, fmt, fs,
    path::{Path, PathBuf},
    process::Stdio,
    time::Instant,
};

#[derive(Debug)]
pub struct Error {
    pub code: i32,
    pub message: String,
}
impl Error {
    pub fn new(code: i32, message: impl Into<String>) -> Self {
        Self {
            code,
            message: message.into(),
        }
    }
}
impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.message.fmt(f)
    }
}
impl std::error::Error for Error {}
impl From<std::io::Error> for Error {
    fn from(error: std::io::Error) -> Self {
        Self::new(1, error.to_string())
    }
}
pub type Result<T> = std::result::Result<T, Error>;

const USAGE: &str = r#"usage: hgs [@host] <cmd> [project] [-c [ID]] [-n tag] [-d] [--rc] [--dry-run] [-- args...]
       hgs mobile-peers --json        guarded direct-peer discovery (mobile-peer ABI 1)
       hgs mobile-peer --json         guarded direct-peer native request from JSON stdin
       hgs [@host] voice <session> --json  send and inspect persistent voice messages (JSON stdin)
       hgs [@host] speech --json      host speech providers and durable jobs (JSON stdin)
       Speech profile management v1
       hgs [@host] a <session>        attach by full name (see hgs ls)
       hgs [@host] a <session> --existing [--run-id ID]   attach only to this live run
       hgs [@host] ls                 sessions here and on peers
       hgs ls --json --local          this box as JSON (for the tray; no ssh)
       hgs [@host] status [--json] [--local]   version, sessions and background services here and on peers
       hgs [@host] version            this hgs version (also --version)
       hgs [@host] inspect <session> [--archive <id>] [--after <cursor>]   activity as JSON
       hgs [@host] reference <session> --json [--archive ID] [--agent ID]   copy a session reference
       hgs inspect 'zerus://session?...'   resolve and inspect a referenced conversation
       hgs [@host] processes <session> [--archive <id>]   observed shell commands as JSON
       hgs [@host] processes <session> --output ID | --stop ID --run RUN --conversation ID [--generation ID]
       hgs [@host] send <session> --json   submit message/attachments from JSON stdin
       hgs [@host] recovery action --scoped-json exact waiting-job control
       hgs [@host] history <session> --json bounded exact public history
       hgs [@host] terminal <session> --json scoped terminal screen/input
       hgs [@host] AGENT DIRECTORY --new -n TAG -d --launch-id UUID
       hgs [@host] session-action <session> --json scoped native lifecycle action
       hgs [@host] compact-context <session> --json compact the idle agent context using its native command
       hgs [@host] clear-context <session> --json   clear the idle agent context using its native command
       hgs [@host] interrupt <session> --json   interrupt the current turn, keep the session
       hgs [@host] attachment <session> --request ID --index N --conversation ID   read a sent attachment
       hgs @host download --path REMOTE_PATH --output LOCAL_FILE   download a file through configured SSH
       hgs [@host] answer <session> --json   answer an exact pending question from JSON stdin
       hgs [@host] read <session> --json   mark the current reply/attention read (JSON stdin)
       hgs [@host] fork <session> [-n name] [-d] [--archive ID]  branch into a new conversation
       hgs [@host] effort <session> --json   change this agent's effort from JSON stdin
       hgs [@host] settings <session> --json model and effort for this session (JSON stdin)
       hgs [@host] recovery get | set | action   automatic recovery policy (JSON stdin for writes)
       hgs [@host] search --query <text> [--limit N]   search saved session contents
       hgs machine ls | set <alias> --json <profile> | remove <alias> | reset <alias>
       hgs machine resolve <alias>          effective SSH settings (no connection)
       hgs swarm get | preview PEER | join PEER | sync | worker   shared project catalog
       hgs swarm assign-launch --json   assign an exact launch to its selected project
       hgs presence set --json        desktop idle/lock time from JSON stdin (written by the tray)
       hgs swarm preference get --json   shared phone notification timing
       hgs swarm preference set mobile_delivery VALUE --json   immediate|away|away_5|away_10|away_15|away_30
       hgs machine check <alias> | ssh <alias> [--directory PATH] | setup <alias> [--source <checkout>]
       hgs [@host] account ls | add ID --provider AGENT --label NAME | rm ID | restore ID | login ID
       hgs [@host] account rename ID --label NAME | inspect ID [--refresh]
       hgs [@host] account default [ID]   show or select the default account for new sessions
       hgs [@host] account set-key native-dsh --json   save API key from private JSON stdin
       hgs [@host] account install codex|claude|kimi|dsh   install an agent in this terminal
       hgs [@host] account inspect --session SESSION [--refresh]
       hgs account copy ID --from MACHINE --to MACHINE --as NEW_ID [--label NAME]
       hgs [@host] worktrees --path PATH [--refresh] [--json]   existing checkouts as JSON
       hgs [@host] git-status --path PATH --json   read-only checkout/publication status
       hgs [@host] worktrees create --path REPO --branch NAME --destination PATH [--base HEAD] [--json]
       hgs [@host] worktrees review --path PATH [--worktree WORKTREE] [--json]   cleanup verdicts and fingerprints
       hgs [@host] worktrees remove --path WORKTREE --common-dir DIR --fingerprint HASH [--delete-branch] [--json]
       Mobile worktree ABI: worktrees-v1 (catalog/create/verified project placement)
       hgs [@host] dirs [--hidden] [path]   directories as JSON (default: home)
       hgs [@host] dirs --create <path> [--request-id UUID]   create one folder (existing parent)
       hgs [@host] kill <session> [--archive <id>]
       hgs [@host] rename <session> <new-full-name> [--archive <id>]
       hgs [@host] archive <session>          move a stopped conversation to Archive
       hgs [@host] pause <session>|--all       save and gracefully close idle agents
       hgs [@host] resume <session>|--all [-d] [--archive <id>]   restore exact conversations
       hgs [@host] resume <session> --fresh [-d] [--expected-run-id ID]   same settings, new conversation
       hgs notify [--bell] <session> [text]   event for the tray (no tray = no-op)
       hgs project ls [--json] | add <name> <dir> | set <name> <dir> | rm <name>
       hgs clip                       PNG from this box's clipboard to stdout (rc 1: no image)
       hgs paste <session> <pane> [client]   the tmux C-v binding; not for hand use
  cmd      claude | codex | kimi (know --resume) | opencode (native API) | sh (login shell) | any command
  project  name from ~/.config/hgs/projects, a path, or omitted = current dir
  -c       resume the dir's last conversation (claude/kimi -c, codex resume --last)
  -c ID    resume this exact conversation UUID; --resume=ID is also accepted
  -n tag   separate session <cmd>/<project>/<tag>
  --account ID   use a named native agent profile for this session
  --model PROVIDER/MODEL   OpenCode native model for a new session
  --new    always create a separate session; generate a name unless -n is given
  -d       start detached (do not attach)      --fresh  explicitly replace a saved conversation
  --rc     claude --remote-control (phone)     --bare   ignore a project run_<cmd>.sh launcher
  --dry-run  print the tmux/ssh command
~/.config/hgs/config sets HGS_SELF (names that mean "this box") and HGS_PEERS
(the ssh aliases that hgs ls polls and @host hops to); environment variables win.
The terminal tab is renamed while attached (HGS_TAB=0 disables this); HGS_TAB_COLORS
sets per-box colours in Konsole. Configuration uses NAME=value assignments.
Ctrl+V in a session attached from a peer pulls the image from that box's clipboard.
--client <peer> identifies the attaching box; @host hops set it automatically.
Saved names resume their exact conversation; --fresh explicitly replaces the binding.
Launch options apply only when a session is CREATED; existing sessions attach as is.
Claude/Codex/Kimi install tracking hooks; Codex hooks need trust in /hooks.
pause refuses untracked/busy sessions. Saved sessions survive reboot; a opens them again.
Archive holds completed conversations; only explicit resume --archive restores them.
terminate stops a tracked session and preserves it in Archive; kill explicitly forgets it.
pause/resume --all covers tracked terminal agents; OpenCode uses exact per-session controls.
resume --all excludes Archive. kill --archive forgets only that archived entry.
rename changes the session label while preserving its agent/project, process and conversation.
"#;

/// Fails where this computer runs no local sessions (Windows for now). Forwarded
/// `@host` commands never reach it.
fn local_sessions() -> Result<()> {
    if platform::local_sessions() {
        Ok(())
    } else {
        Err(Error::new(2, platform::LOCAL_SESSIONS_UNSUPPORTED))
    }
}

/// Fails unless this process may run what the processes of a session run:
/// `--run`, the session's supervisor, and `__state`, which its tracking hooks
/// call. That is wherever local sessions run, and on Windows also inside a
/// session of an isolated test host (spec D2). A control console refuses both
/// with exit code 2, as in Phase 1.
fn session_process() -> Result<()> {
    state::session_operations().map_err(|error| Error::new(2, error))
}
#[cfg(unix)]
fn mobile_peers(config: &Config, args: &[String]) -> Result<i32> {
    crate::mobile_peers::dispatch(config, args)
}
#[cfg(not(unix))]
fn mobile_peers(_: &Config, _: &[String]) -> Result<i32> {
    Err(Error::new(
        2,
        "Mobile peers are not supported on Windows yet; use a Linux or macOS machine as the mobile gateway.",
    ))
}
fn state_command(verb: &str, args: &[String]) -> Result<i32> {
    let mut command = vec![verb.to_owned()];
    command.extend_from_slice(args);
    state::dispatch(&command).map_err(|e| Error::new(1, e))
}
fn has(args: &[String], arg: &str) -> bool {
    args.iter().any(|a| a == arg)
}
fn require<'a>(args: &'a [String], index: usize, message: &str) -> Result<&'a str> {
    args.get(index)
        .filter(|s| !s.is_empty())
        .map(String::as_str)
        .ok_or_else(|| Error::new(1, message))
}
fn sanitize(name: &str) -> String {
    name.replace([':', '.'], "_")
}
/// A Windows folder's name after either separator (spec D15).
#[cfg(windows)]
fn basename(path: &str) -> String {
    let trimmed = path.trim_end_matches(['/', '\\']);
    if trimmed.is_empty() {
        return "/".into();
    }
    trimmed.rsplit(['/', '\\']).next().unwrap_or(path).into()
}
#[cfg(unix)]
fn basename(path: &str) -> String {
    if path.trim_end_matches('/').is_empty() {
        return "/".into();
    }
    path.trim_end_matches('/')
        .rsplit('/')
        .next()
        .unwrap_or(path)
        .into()
}
/// The name a session takes from `directory`: on Windows in the letter case of
/// the folder on disk, so `cd C:\work\myproj` names it after `MyProj` (spec D15).
#[cfg(windows)]
fn folder_name(directory: &str) -> String {
    let canonical = platform::canonical(directory)
        .ok()
        .map(|path| basename(&path.to_string_lossy()));
    on_disk_name(basename(directory), canonical)
}
#[cfg(unix)]
fn folder_name(directory: &str) -> String {
    basename(directory)
}
/// The `canonical` folder name when it is the `logical` one in another letter
/// case; a junction, link or short name with another name keeps its own.
/// Plain Rust, so its tests run on every platform.
#[cfg_attr(unix, allow(dead_code))]
fn on_disk_name(logical: String, canonical: Option<String>) -> String {
    let fold = |name: &str| {
        name.chars()
            .flat_map(char::to_lowercase)
            .collect::<String>()
    };
    match canonical {
        Some(canonical) if fold(&canonical) == fold(&logical) => canonical,
        _ => logical,
    }
}
fn session_exists(name: &str) -> bool {
    terminal().session_exists(name)
}
/// This computer's session terminal (tmux on Unix).
fn terminal() -> &'static dyn TerminalBackend {
    state::backend::current()
}
fn binding_exists(config: &Config, name: &str) -> Result<bool> {
    if !config.state.is_dir() {
        return Ok(false);
    }
    Ok(state_command("exists", &[name.into()])? == 0)
}
/// On Windows only the stable CLI, whichever hgs.exe runs now: records, hooks
/// and recipes never name a portable folder that an update replaces (spec D8).
#[cfg(windows)]
fn executable_path() -> Result<PathBuf> {
    platform::stable_executable().map_err(|error| Error::new(1, error))
}
#[cfg(unix)]
fn executable_path() -> Result<PathBuf> {
    if let Some(path) = env::var_os("HGS_EXECUTABLE").filter(|s| !s.is_empty()) {
        let path = PathBuf::from(path);
        if path.is_absolute() && platform::executable(&path) {
            return Ok(path);
        }
    }
    // Preserve the invoked stable install path, including a symlink, in durable recipes.
    let invoked = env::args().next().unwrap_or_else(|| "hgs".into());
    let path = PathBuf::from(&invoked);
    if path.is_absolute() {
        return Ok(path);
    }
    if invoked.contains('/') {
        return Ok(env::current_dir()?.join(path));
    }
    if let Some(path) = env::split_paths(&env::var_os("PATH").unwrap_or_default())
        .map(|directory| directory.join(&invoked))
        .find(|path| platform::executable(path))
    {
        return Ok(if path.is_absolute() {
            path
        } else {
            env::current_dir()?.join(path)
        });
    }
    Ok(env::current_exe()?)
}
fn nested_guard(dry: bool) -> Result<()> {
    if !dry && terminal().inside() {
        let name = terminal().enclosing_session();
        return Err(Error::new(
            2,
            format!("already inside tmux session {name}; open a new tab"),
        ));
    }
    Ok(())
}

#[cfg(test)]
pub(crate) fn usage() -> &'static str {
    USAGE
}

pub fn dispatch(args: Vec<String>) -> Result<i32> {
    // Private: the screen model behind the differential tmux capture test.
    if args.first().is_some_and(|a| a == "__screen-capture") {
        return crate::screen::cli(&args[1..]).map_err(|e| Error::new(2, e));
    }
    // A Windows `--run` supervisor belongs to its session's job (spec D9).
    #[cfg(windows)]
    if args.first().is_some_and(|a| a == "--run") {
        platform::prepare_supervisor();
    } else {
        platform::prepare_environment();
    }
    #[cfg(unix)]
    platform::prepare_environment();
    // Existing hooks call this private stable ABI, including hooks installed before migration.
    if args.first().is_some_and(|a| a == "__state") {
        session_process()?;
        return state::dispatch(&args[1..]).map_err(|e| Error::new(1, e));
    }
    let config = Config::load()?;
    // The existing swarm namespace fails closed on unknown operations in older
    // installations, so transport helpers cannot fall through to agent launch.
    if args.first().is_some_and(|a| a == "swarm")
        && args.get(1).is_some_and(|a| {
            matches!(a.as_str(), "mobile-peers" | "mobile-peer" | "__mobile-peer-local" | "__mobile-peer-identity" | "__mobile-peer-transport")
        })
    {
        return mobile_peers(&config, &args[1..]);
    }
    if args.first().is_some_and(|a| {
        matches!(a.as_str(), "mobile-peers" | "mobile-peer" | "__mobile-peer-local" | "__mobile-peer-identity" | "__mobile-peer-transport")
    }) {
        return mobile_peers(&config, &args);
    }
    if args.first().is_some_and(|a| a == "--run") {
        // A control console (local sessions off) still refuses it as in Phase 1;
        // an isolated test host's sessions may run it (spec D2).
        #[cfg(windows)]
        session_process()?;
        let start = Instant::now();
        let result = run(&args[1..]);
        // A launch the desktop waits for learns how its session ended.
        state::launches::finished(
            start.elapsed(),
            result
                .as_ref()
                .copied()
                .map_err(|error| (error.code, error.message.as_str())),
        );
        return result;
    }
    let mut target = String::new();
    let mut dry = false;
    let mut client = "local".to_owned();
    let mut index = 0;
    while let Some(arg) = args.get(index) {
        if let Some(host) = arg.strip_prefix('@') {
            target = host.into();
            index += 1;
        } else if arg == "--dry-run" {
            dry = true;
            index += 1;
        } else if arg == "--client" {
            client = require(&args, index + 1, "--client needs a peer name")?.into();
            index += 2;
        } else {
            break;
        }
    }
    let Some(command) = args.get(index) else {
        eprint!("{USAGE}");
        return Ok(1);
    };
    let args = &args[index + 1..];
    match command.as_str() {
        "-h" | "--help" | "help" => {
            // Capability probes for a selected computer must inspect that
            // computer's installed CLI, including retained older versions.
            if !target.is_empty() && !config.is_self(&target) {
                return remote(&config, &target, "--help", args, dry);
            }
            print!("{USAGE}");
            #[cfg(windows)]
            print!("{}", crate::host::admin::HELP);
            return Ok(0);
        }
        "--version" | "version" => return crate::status::version(&config, &target, args),
        // Read-only on every machine; never routed as an agent launch.
        "status" => return crate::status::run(&config, &target, args, dry),
        _ => {}
    }
    if args.first().is_some_and(|value| value.starts_with("zerus://")) {
        let local_only = args.len() == 2 && args[1] == "--local";
        if command != "inspect" || (args.len() != 1 && !local_only) || dry {
            return Err(Error::new(1, "Use hgs inspect '<session reference>' for read-only session references"));
        }
        let reference = crate::session_reference::Reference::parse(&args[0])?;
        let local_id = crate::swarm::local_node_id(&config).map_err(|e| Error::new(1,e.to_string()))?;
        if local_only && local_id != reference.machine {
            return Err(Error::new(1,"Session reference belongs to another computer; the selected SSH alias does not match"));
        }
        if target.is_empty() && local_id != reference.machine {
            if config.peers.contains(&reference.host) && !config.is_self(&reference.host) {
                return remote(&config, &reference.host, command, args, false);
            }
            return Err(Error::new(1, "The referenced computer is not configured here; choose its configured @host"));
        }
        if !target.is_empty() && !config.is_self(&target) {
            if local_only { return Err(Error::new(1,"A local session reference cannot route to another computer")); }
            return remote(&config,&target,command,args,false);
        }
        local_sessions()?;
        println!("{}", crate::session_reference::inspect(&config,&reference)?);
        return Ok(0);
    }
    if command == "native-ui" {
        let name = require(args, 0, "usage: hgs native-ui <session> [--json]")?;
        if args.get(1).is_some_and(|v| v == "--json") && !dry {
            println!(
                "{}",
                serde_json::json!({"url":crate::native_ui::address(&config,&target,name)?})
            );
            return Ok(0);
        }
        return crate::native_ui::open(&config, &target, name, dry);
    }
    if command == "account"
        && args.first().is_some_and(|v| v == "login")
        && args.get(1).is_some_and(|v| v == "native-dsh")
    {
        return crate::native_ui::open(&config, &target, "@account", dry);
    }
    if command == "a" && args.first().is_some_and(|v| v.starts_with("dsh/")) {
        return crate::native_ui::open(&config, &target, &args[0], dry);
    }
    if command == "download" {
        return crate::download::dispatch(&config, &target, args, dry);
    }
    // Windows only, before local sessions are enabled, and never forwarded: it
    // enables them on this computer (spec D2). On Unix the word stays a program.
    #[cfg(windows)]
    if command == "session-host" {
        if !target.is_empty() && !config.is_self(&target) {
            return Err(Error::new(
                2,
                "hgs session-host manages the computer it runs on; run it on that computer",
            ));
        }
        return crate::host::admin::dispatch(&config, args, dry);
    }
    if !target.is_empty() && !config.is_self(&target) {
        return remote(&config, &target, command, args, dry);
    }
    // Commands that only read or change this machine's configuration, or reach
    // remote machines, also run on a build without local sessions.
    if !matches!(command.as_str(), "ls" | "machine" | "presence" | "swarm" | "project" | "account" | "speech") {
        local_sessions()?;
    }
    match command.as_str() {
        "speech" => crate::speech::dispatch(&config, args, dry),
        "__dsh-ui-url" => {
            let url = state::dsh::native_ui_url(require(args, 0, "Choose a native session")?)
                .map_err(|e| Error::new(1, e))?;
            println!("{}", json!({"url":url}));
            Ok(0)
        }
        "ls" => list(&config, args, dry),
        "reference" => {
            if dry { return Err(Error::new(1,"Session references do not support --dry-run")); }
            crate::session_reference::create(&config,args)
        }
        "a" => attach_command(&config, args, &client, dry),
        "kill" => kill(&config, args, dry),
        "rename" => rename(&config, args, dry),
        "archive" | "terminate" => {
            let mut args = args.to_vec();
            if dry {
                args.push("--dry-run".into());
            }
            state_command(command, &args)
        }
        "inspect" | "processes" | "worktrees" | "git-status" | "dirs" | "send" | "send-now" | "session-action" | "terminal" | "history" | "interrupt" | "clear-context" | "compact-context" | "answer" | "effort" | "settings" | "search" | "read"
        | "attachment" | "recovery" | "voice" => {
            if command == "dirs" && has(args, "--create") && dry {
                return Err(Error::new(1, "folder creation does not support --dry-run"));
            }
            if command == "worktrees" && args.first().is_some_and(|s| s == "create" || s == "remove") && dry {
                return Err(Error::new(1, "worktree creation and removal do not support --dry-run"));
            }
            if (matches!(command.as_str(), "processes" | "send" | "send-now" | "session-action" | "terminal" | "interrupt" | "clear-context" | "compact-context" | "answer" | "effort" | "settings" | "recovery" | "read" | "voice") || (command=="attachment" && has(args,"--stage"))) && dry {
                return Err(Error::new(1, "message input does not support --dry-run"));
            }
            state_command(command, args)
        }
        "pause" | "resume" => pause_resume(&config, command, args, &client, dry),
        "fork" => fork_session(&config, args, &client, dry),
        #[cfg(unix)]
        "notify" => notify(&config, args, dry),
        #[cfg(unix)]
        "clip" => platform::clip(),
        #[cfg(unix)]
        "paste" => platform::paste(
            &config,
            require(
                args,
                0,
                "usage: hgs paste <session> <pane> [client]  (the tmux C-v binding)",
            )?,
            require(
                args,
                1,
                "usage: hgs paste <session> <pane> [client]  (the tmux C-v binding)",
            )?,
            args.get(2).map(String::as_str).unwrap_or(""),
        ),
        "machine" => crate::machines::dispatch(&config, args, dry),
        "presence" => crate::presence::dispatch(&config, args, dry),
        "swarm" => crate::swarm::dispatch(&config, args, dry),
        "account" => crate::accounts::dispatch(&config, args, dry),
        "project" => project(&config, args, dry),
        _ => launch(&config, command, args, &client, dry),
    }
}

/// A session's root process: `--run N RESUME... BASE...`. Tracked agents run
/// under `state`'s supervisor; anything else first tries the N resume words,
/// then runs the base command in place of this process on Unix and as its
/// child on Windows, which has no exec (spec D8).
fn run(args: &[String]) -> Result<i32> {
    let n: usize = require(args, 0, "--run needs a resume-argument count")?
        .parse()
        .map_err(|_| Error::new(1, "invalid --run resume-argument count"))?;
    if n >= args.len() - 1 {
        return Err(Error::new(
            1,
            "--run needs a base command after the resume arguments",
        ));
    }
    if env::var("HGS_SESSION").is_ok_and(|s| !s.is_empty())
        && env::var("HGS_TRACKING").as_deref() != Ok("0")
    {
        return state_command("run", args);
    }
    let resume = &args[1..n + 1];
    let base = &args[n + 1..];
    // The agent gets Ctrl+C; a stop request or a closing console waits for it.
    // The session host pauses the agent and reads its console through the
    // supervisor channel (spec D10, D11).
    #[cfg(windows)]
    let _stops = platform::ShutdownGuard::install()?;
    #[cfg(windows)]
    crate::host::supervisor::open();
    if n != 0 {
        let start = Instant::now();
        let rc = run_resume(resume)?;
        let seconds: u64 = env::var("HGS_FALLBACK_SECS")
            .unwrap_or_else(|_| "5".into())
            .parse()
            .map_err(|_| Error::new(1, "HGS_FALLBACK_SECS must be a nonnegative integer"))?;
        if resume_stands(
            rc,
            start.elapsed().as_secs(),
            seconds,
            platform::shutdown_signal() != 0,
        ) {
            return Ok(rc);
        }
        eprintln!(
            "hgs: nothing to resume (exit {rc} after {}s) — starting fresh",
            start.elapsed().as_secs()
        );
    }
    run_base(base)
}

/// Whether a resume attempt that exited with `rc` after `elapsed` seconds is
/// final, rather than "nothing to resume" (a quick ordinary failure, after
/// which the base command starts fresh): it succeeded, ran for at least
/// `limit` seconds, crashed or was signalled (128 and above; Windows reads
/// crash statuses as signals, `platform::ntstatus_exit`), or a stop request
/// reached the supervisor (`stopped`, Windows only: there a paused agent may
/// exit with any code).
fn resume_stands(rc: i32, elapsed: u64, limit: u64, stopped: bool) -> bool {
    rc == 0 || elapsed >= limit || rc >= 128 || stopped
}

#[cfg(unix)]
fn run_resume(argv: &[String]) -> Result<i32> {
    platform::wait_interactive(Command::new(&argv[0]).args(&argv[1..]))
}

#[cfg(unix)]
fn run_base(base: &[String]) -> Result<i32> {
    // The untracked runner has no durable process state and can replace itself directly.
    use std::os::unix::process::CommandExt;
    let error = Command::new(&base[0]).args(&base[1..]).exec();
    Err(Error::new(
        if error.kind() == std::io::ErrorKind::NotFound {
            127
        } else {
            126
        },
        format!("{}: {error}", base[0]),
    ))
}

#[cfg(windows)]
fn run_resume(argv: &[String]) -> Result<i32> {
    run_child(argv)
}

#[cfg(windows)]
fn run_base(base: &[String]) -> Result<i32> {
    run_child(base)
}

/// Run `argv` on this console as the pane's program, which shares this
/// process group, and wait for it; its exit code, a crash read as 128 plus a
/// signal (`platform::code`).
#[cfg(windows)]
fn run_child(argv: &[String]) -> Result<i32> {
    let mut child = platform::agent_command(argv)
        .and_then(|mut command| command.spawn())
        .map_err(|error| {
            Error::new(
                if error.kind() == std::io::ErrorKind::NotFound {
                    127
                } else {
                    126
                },
                format!("{}: {error}", argv[0]),
            )
        })?;
    platform::forward_shutdown(&child);
    let status = child.wait()?;
    platform::shutdown_child_exited();
    Ok(platform::code(status))
}

fn peer_session(command: &str, args: &[String]) -> String {
    if command == "fork" {
        if let Some(source) = args.first() {
            let parts: Vec<_> = source.splitn(3, '/').collect();
            if parts.len() >= 2 {
                let tag = args
                    .windows(2)
                    .find(|pair| pair[0] == "-n")
                    .map(|pair| pair[1].clone())
                    .unwrap_or_else(|| {
                        format!("{}-fork", parts.get(2).copied().unwrap_or("session"))
                    });
                return format!("{}/{}/{tag}", parts[0], parts[1]);
            }
        }
    }
    if matches!(command, "a" | "resume") {
        return args.first().cloned().unwrap_or_default();
    }
    let mut project = String::new();
    let mut tag = String::new();
    let mut i = 0;
    while let Some(arg) = args.get(i) {
        match arg.as_str() {
            "--" => break,
            "-n" => {
                tag = args.get(i + 1).cloned().unwrap_or_default();
                i += 1;
            }
            "--launch-id" => i += 1,
            option if option.starts_with('-') => {}
            name if project.is_empty() => project = name.into(),
            _ => {}
        }
        i += 1;
    }
    if matches!(project.as_str(), "." | "..") {
        project.clear();
    } else if project.contains('/') || project.starts_with('~') {
        project = basename(&project);
    }
    format!(
        "{}{}{}",
        basename(command),
        if project.is_empty() {
            String::new()
        } else {
            format!("/{}", sanitize(&project))
        },
        if tag.is_empty() {
            String::new()
        } else {
            format!("/{tag}")
        }
    )
}
fn peer_json(config: &Config, peer: &str) -> Value {
    let output = platform::ssh_output(
        platform::ssh()
            .args(crate::machines::ssh_options(config, peer))
            .args([
                "-o",
                "BatchMode=yes",
                "-o",
                "ConnectTimeout=3",
                peer,
                "~/.local/bin/hgs ls --local --json",
            ])
            .stderr(Stdio::piped()),
    );
    let (error, detail) = match output {
        Ok(out) if out.status.success() && !out.stdout.is_empty() => {
            match serde_json::from_slice::<Value>(&out.stdout) {
                Ok(value) if value.is_object() => return value,
                _ => (
                    "bad_response",
                    "Invalid response from hgs on the remote machine".to_string(),
                ),
            }
        }
        Ok(out) => {
            let detail: String = String::from_utf8_lossy(&out.stderr)
                .chars()
                .filter(|c| !c.is_control() || *c == '\n' || *c == '\t')
                .take(2048)
                .collect();
            (
                "offline",
                if detail.trim().is_empty() {
                    format!("SSH command failed ({})", out.status)
                } else {
                    detail.trim().to_owned()
                },
            )
        }
        Err(err) => ("offline", format!("Could not start SSH: {err}")),
    };
    // Preserve the legacy object key order used by simple shell consumers.
    let mut value = serde_json::Map::new();
    value.insert("host".into(), json!(peer));
    value.insert("ok".into(), json!(false));
    value.insert("error".into(), json!(error));
    value.insert("error_detail".into(), json!(detail));
    value.insert("projects".into(), json!({}));
    value.insert("sessions".into(), json!([]));
    Value::Object(value)
}
fn remote(
    config: &Config,
    target: &str,
    command: &str,
    args: &[String],
    global_dry: bool,
) -> Result<i32> {
    if target.starts_with('-') || target.chars().any(char::is_whitespace) {
        return Err(Error::new(1, "invalid SSH host alias"));
    }
    let mut args = args.to_vec();
    if command == "inspect" && args.first().is_some_and(|arg| arg.starts_with("zerus://session?")) && args.len()==1 {
        args.push("--local".into());
    }
    if global_dry {
        args.push("--dry-run".into());
    }
    let mut flags: Vec<String>;
    let mut link = None;
    let mut tab = None;
    let mut ssh = platform::ssh();
    let remote_args: Vec<String> = match command {
        "ls" => {
            flags = vec![
                "-o".into(),
                "BatchMode=yes".into(),
                "-o".into(),
                "ConnectTimeout=3".into(),
            ];
            let mut all = vec!["ls".into(), "--local".into()];
            all.extend(args.clone());
            all
        }
        "account" if args.first().is_some_and(|s| s != "login" && s != "install") => {
            flags = vec![
                "-o".into(),
                "BatchMode=yes".into(),
                "-o".into(),
                "ConnectTimeout=5".into(),
            ];
            let mut all = vec![command.into()];
            all.extend(args.clone());
            all
        }
        "kill" | "terminate" | "pause" | "inspect" | "reference" | "processes" | "worktrees" | "git-status" | "dirs" | "project" | "archive" | "rename"
        | "send" | "send-now" | "session-action" | "terminal" | "history" | "interrupt" | "clear-context" | "compact-context" | "answer" | "effort" | "settings" | "search" | "dsh" | "opencode" | "attachment" | "recovery" | "swarm" | "read" | "speech" | "voice" | "--help" => {
            flags = vec![
                "-o".into(),
                "BatchMode=yes".into(),
                "-o".into(),
                "ConnectTimeout=3".into(),
            ];
            let mut all = vec![command.into()];
            all.extend(args.clone());
            all
        }
        _ => {
            if has(&args, "-d") || (command == "resume" && has(&args, "--all")) {
                flags = vec!["-o".into(), "BatchMode=yes".into()];
            } else {
                let directory = platform::link_marker_dir()?;
                let marker = directory.path().join("up");
                flags = vec![
                    "-t".into(),
                    "-o".into(),
                    "PermitLocalCommand=yes".into(),
                    "-o".into(),
                    format!(
                        "LocalCommand={}",
                        platform::create_file_command(&mut ssh, &marker)
                    ),
                ];
                link = Some(directory);
                tab = Some(format!("{target}:{}", peer_session(command, &args)));
            }
            let mut all = vec!["--client".into(), config.host().into(), command.into()];
            all.extend(args.clone());
            all
        }
    };
    flags.extend(crate::machines::ssh_options(config, target));
    let remote = format!("~/.local/bin/hgs {}", platform::join(&remote_args));
    if global_dry || has(&args, "--dry-run") {
        if let Some(title) = tab {
            if config.tab {
                println!("tab {title} color={}", platform::tab_color(config, target));
            }
        }
        println!("ssh {} {target} {remote}", platform::join(&flags));
        return Ok(0);
    }
    if command == "ls" && has(&args, "--json") {
        println!("{}", peer_json(config, target));
        return Ok(0);
    }
    if command == "ls" {
        println!("{target}:");
    }
    let _tab = tab
        .as_ref()
        .map(|title| TabGuard::begin(config, title, &platform::tab_color(config, target)));
    flags.push(target.into());
    flags.push(remote);
    let rc = platform::wait_interactive(ssh.args(flags))?;
    if rc == 255 {
        if link
            .as_ref()
            .is_some_and(|dir| dir.path().join("up").exists())
        {
            platform::restore_terminal();
            return Err(Error::new(
                3,
                format!("link to {target} dropped; the session is still there, rerun to reattach"),
            ));
        }
        return Err(Error::new(3, format!("{target} unreachable")));
    }
    Ok(rc)
}

pub(crate) fn local_json(config: &Config) -> Result<Value> {
    if !platform::local_sessions() {
        // A control console: no tmux, session state or local projects. Absent
        // `local_sessions` means true, so Unix listings stay as they were.
        let mut raw = json!({"host":config.host(),"ok":true,"hgs_version":env!("CARGO_PKG_VERSION"),"projects":{},"sessions":[],"peers":config.peers,"metrics":crate::metrics::snapshot(&config.state),"local_sessions":false});
        if let Some(presence) = crate::presence::listing(&config.state, crate::presence::now()) {
            raw["desktop_presence"] = presence;
        }
        raw["preferences"] = crate::swarm::preferences(config);
        #[cfg(windows)]
        {
            raw["local_host"] = crate::host::admin::local_host(&config.state);
        }
        return Ok(raw);
    }
    let mut projects = serde_json::Map::new();
    for project in config.projects()? {
        projects.insert(project.name, json!(project.dir));
    }
    // Before the terminal lists: a launch's session that this listing does not
    // show existed before it began and has ended.
    let launches = state::launches::Evidence::read();
    // A listing that fails is an error: no sessions would show live ones as stopped.
    let listing = terminal().listing().map_err(|e| Error::new(1, e))?;
    let sessions: Vec<Value> = listing.sessions.iter().map(|row| {
        let name = row.name.as_str();
        let mut parts = name.splitn(3, '/'); let cmd = parts.next().unwrap_or("");
        let project = parts.next().filter(|s| !s.is_empty()); let tag = parts.next().filter(|s| !s.is_empty());
        let ttys: Vec<&str> = listing.clients.iter().filter_map(|client| if client.session == name { Some(client.terminal.as_str()) } else { None }).collect();
        json!({"name":name,"cmd":cmd,"project":project,"tag":tag,"attached":row.attached,"clients":ttys,"created":row.created,"launch_id":row.launch_id})
    }).collect();
    let mut raw = json!({"host":config.host(),"ok":true,"hgs_version":env!("CARGO_PKG_VERSION"),"projects":projects,"sessions":sessions,"peers":config.peers,"metrics":crate::metrics::snapshot(&config.state)});
    // Phone alerts wait while the owner is active here; see src/presence.rs.
    if let Some(presence) = crate::presence::listing(&config.state, crate::presence::now()) {
        raw["desktop_presence"] = presence;
    }
    raw["preferences"] = crate::swarm::preferences(config);
    // Windows: the session host as found, never started (spec D2). Unix
    // listings stay as they were.
    #[cfg(windows)]
    {
        raw["local_host"] = crate::host::admin::local_host(&config.state);
    }
    let mut merged = state::merge_snapshot(raw).map_err(|e| Error::new(1, e))?;
    let ended = launches.ended(merged["sessions"].as_array().map_or(&[], Vec::as_slice));
    if !ended.is_empty() {
        merged["ended_launches"] = json!(ended);
    }
    Ok(merged)
}
fn local_list(config: &Config) -> Result<()> {
    if !platform::local_sessions() {
        println!("  (control console: sessions run on remote machines)");
        return Ok(());
    }
    match terminal().attached_counts().map_err(|e| Error::new(1, e))? {
        Some(rows) => {
            for (name, count) in rows {
                println!("  {name:<40} {count} client(s)");
            }
        }
        None => println!("  (no sessions)"),
    }
    if config.state.is_dir() {
        state_command("list", &[])?;
    }
    Ok(())
}
fn list(config: &Config, args: &[String], global_dry: bool) -> Result<i32> {
    let dry = global_dry || has(args, "--dry-run");
    if dry && !platform::local_sessions() {
        return Ok(0); // this machine's listing runs no command
    }
    if has(args, "--json") {
        if dry {
            println!("{}", terminal().describe(&DryRun::ListJson));
            return Ok(0);
        }
        let local = local_json(config)?;
        if has(args, "--local") {
            println!("{local}");
        } else {
            let mut fleet = vec![local];
            fleet.extend(config.peers.iter().map(|peer| peer_json(config, peer)));
            println!("{}", json!(fleet));
        }
        return Ok(0);
    }
    if dry {
        println!("{}", terminal().describe(&DryRun::List));
        return Ok(0);
    }
    if args.first().is_some_and(|a| a == "--local") {
        local_list(config)?;
        return Ok(0);
    }
    println!("{}:", config.host());
    local_list(config)?;
    for peer in &config.peers {
        println!("{peer}:");
        let mut flags = crate::machines::ssh_options(config, peer);
        flags.extend(
            [
                "-o",
                "BatchMode=yes",
                "-o",
                "ConnectTimeout=3",
                peer,
                "~/.local/bin/hgs ls --local",
            ]
            .into_iter()
            .map(str::to_owned),
        );
        match platform::ssh_capture(&flags) {
            Some(out) => println!("{}", out.trim_end_matches('\n')),
            None => println!("  offline"),
        }
    }
    Ok(0)
}

fn attach_command(config: &Config, args: &[String], client: &str, mut dry: bool) -> Result<i32> {
    const HELP: &str = "usage: hgs a <session> [--existing [--run-id ID]]";
    let session = require(args, 0, HELP)?;
    if state::opencode::exists(session) {return Err(Error::new(1,"OpenCode uses Activity; this adapter has no tmux Terminal"));}
    if state::dsh::exists(session) {
        return crate::native_ui::open(config, "", session, dry || has(args, "--dry-run"));
    }
    let mut existing = false;
    let mut run_id = None;
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--existing" => existing = true,
            "--dry-run" => dry = true,
            "--run-id" if run_id.is_none() => {
                run_id = Some(require(args, i + 1, "--run-id needs an identity")?);
                i += 1;
            }
            _ => return Err(Error::new(1, HELP)),
        }
        i += 1;
    }
    if run_id.is_some() && !existing {
        return Err(Error::new(1, "--run-id requires --existing"));
    }
    if !existing {
        return attach(config, session, client, dry);
    }
    if dry {
        println!(
            "{}",
            terminal().describe(&DryRun::AttachExisting { session, run_id })
        );
        return Ok(0);
    }
    nested_guard(false)?;
    // Resolve exact name to an immutable tmux session ID before checking the run.
    // If it disappears later, attach fails instead of creating or selecting a new one.
    const ENDED: &str = "session is no longer running; resume it explicitly";
    let target = terminal()
        .probe(
            Target::Name(session),
            &[PaneField::SessionId, PaneField::RunMarker],
        )
        .ok_or_else(|| Error::new(1, ENDED))?;
    let fields = target.fields();
    // A running tmux server expands an unknown target to empty fields instead of failing.
    if fields.first().is_some_and(|id| id.is_empty()) {
        return Err(Error::new(1, ENDED));
    }
    if fields.len() != 2
        || !fields[0]
            .strip_prefix('$')
            .is_some_and(|id| !id.is_empty() && id.chars().all(|ch| ch.is_ascii_digit()))
    {
        return Err(Error::new(1, "invalid tmux session identity"));
    }
    if run_id.is_some_and(|expected| fields[1] != expected) {
        return Err(Error::new(1, crate::state::backend::RUN_CHANGED));
    }
    attach_target(config, session, client, Target::Session(fields[0]), run_id)
}

fn attach(config: &Config, session: &str, client: &str, dry: bool) -> Result<i32> {
    if dry {
        println!(
            "{}",
            terminal().describe(&DryRun::Attach { session, client })
        );
        return Ok(0);
    }
    nested_guard(false)?;
    terminal().attachable()?;
    if !session_exists(session) && config.state.is_dir() {
        env::set_var("HGS_CLIENT", client);
        let rc = state_command("resume", &[session.into()])?;
        if rc != 0 {
            return Ok(rc);
        }
    }
    attach_target(config, session, client, Target::Name(session), None)
}

fn attach_target(
    config: &Config,
    session: &str,
    client: &str,
    target: Target,
    run_id: Option<&str>,
) -> Result<i32> {
    terminal().set_client(target, client);
    let _tab = TabGuard::begin(config, session, &platform::tab_color(config, "local"));
    let exit = terminal().attach(target, run_id)?;
    report_startup_failure(session);
    Ok(exit.code)
}

fn report_startup_failure(session: &str) -> bool {
    if let Ok(Some(message)) = state::startup_failure(session) {
        eprintln!("hgs: {message}");
        true
    } else {
        false
    }
}

fn attach_requested(
    config: &Config,
    session: &str,
    client: &str,
    agent: &str,
    id: &str,
) -> Result<i32> {
    // Capture the tmux identity before rechecking the conversation. If the name
    // is reused afterwards, attach must fail instead of selecting its new owner.
    let target = terminal()
        .probe(Target::Name(session), &[PaneField::SessionId])
        .filter(|info| {
            info.value().strip_prefix('$').is_some_and(|value| {
                !value.is_empty() && value.chars().all(|ch| ch.is_ascii_digit())
            })
        })
        .ok_or_else(|| {
            Error::new(
                1,
                "requested session ended before attach; repeat the command",
            )
        })?;
    state::check_requested_session(session, agent, id).map_err(|error| Error::new(1, error))?;
    attach_target(
        config,
        session,
        client,
        Target::Session(target.value()),
        None,
    )
}
fn kill(config: &Config, args: &[String], mut dry: bool) -> Result<i32> {
    let session = require(args, 0, "usage: hgs kill <session> [--archive <id>]")?;
    let mut archive = None;
    let mut index = 1;
    while index < args.len() {
        match args[index].as_str() {
            "--dry-run" => dry = true,
            "--archive" if archive.is_none() => {
                archive = Some(require(args, index + 1, "--archive needs an archive ID")?);
                index += 1;
            }
            option => return Err(Error::new(1, format!("unknown kill option: {option}"))),
        }
        index += 1;
    }
    // Resolve archive-only operations before touching tmux. The same name can
    // belong to a live session and several independent archived conversations.
    if let Some(id) = archive {
        let mut args = vec![session.into(), "--archive".into(), id.into()];
        if dry {
            args.push("--dry-run".into());
        }
        return state_command("forget", &args);
    }
    if dry {
        println!("{}", terminal().describe(&DryRun::Kill { session }));
        return Ok(0);
    }
    if session_exists(session) {
        let rc = terminal().kill(session)?;
        if rc != 0 {
            return Ok(rc);
        }
    } else if !binding_exists(config, session)? {
        return Err(Error::new(1, format!("no such session: {session}")));
    }
    if config.state.is_dir() {
        state_command("forget", &[session.into()])
    } else {
        Ok(0)
    }
}

fn rename(config: &Config, args: &[String], mut dry: bool) -> Result<i32> {
    const USAGE: &str = "usage: hgs rename <session> <new-full-name> [--archive ID] [--dry-run]";
    let old = require(args, 0, USAGE)?;
    let new = require(args, 1, USAGE)?;
    let mut archive = None;
    let mut index = 2;
    while index < args.len() {
        match args[index].as_str() {
            "--dry-run" => dry = true,
            "--archive" if archive.is_none() => {
                archive = Some(require(args, index + 1, "--archive needs an archive ID")?);
                index += 1;
            }
            option => return Err(Error::new(1, format!("unknown rename option: {option}"))),
        }
        index += 1;
    }
    let mut state_args = vec![old.to_owned(), new.to_owned()];
    if let Some(id) = archive {
        state_args.extend(["--archive".into(), id.into()]);
    }
    if dry {
        state_args.push("--dry-run".into());
    }
    let result = state_command("rename", &state_args)?;
    if result == 0 && !dry && archive.is_none() && config.tab {
        if let Err(error) = terminal().refresh_titles(new) {
            eprintln!("hgs: session renamed; could not update a terminal title: {error}");
        }
    }
    Ok(result)
}
fn fork_session(config: &Config, args: &[String], client: &str, mut dry: bool) -> Result<i32> {
    let name = require(
        args,
        0,
        "usage: hgs fork <session> [-n name] [-d] [--archive ID]",
    )?;
    let (mut tag, mut archive, mut expected_run, mut expected_conversation) =
        (None, None, None, None);
    let mut detached = false;
    let mut index = 1;
    while index < args.len() {
        match args[index].as_str() {
            "--dry-run" => dry = true,
            "-d" => detached = true,
            "-n" | "--archive" | "--expected-run-id" | "--expected-conversation-id" => {
                let value = require(args, index + 1, "missing fork option value")?;
                let slot = match args[index].as_str() {
                    "-n" => &mut tag,
                    "--archive" => &mut archive,
                    "--expected-run-id" => &mut expected_run,
                    _ => &mut expected_conversation,
                };
                if slot.is_some() {
                    return Err(Error::new(1, "duplicate fork option"));
                }
                *slot = Some(value);
                index += 1;
            }
            option => return Err(Error::new(1, format!("unknown fork option: {option}"))),
        }
        index += 1;
    }
    if !detached && !dry && terminal().inside() {
        return Err(Error::new(
            2,
            "already inside tmux; use fork -d or open a new tab",
        ));
    }
    env::set_var("HGS_CLIENT", client);
    let target = state::fork_session(name, tag, archive, expected_run, expected_conversation, dry)
        .map_err(|error| Error::new(1, error))?;
    if detached || dry {
        Ok(0)
    } else {
        attach(config, &target, client, false)
    }
}

fn pause_resume(
    config: &Config,
    command: &str,
    args: &[String],
    client: &str,
    mut dry: bool,
) -> Result<i32> {
    let session = require(
        args,
        0,
        &format!("usage: hgs {command} <session>|--all [-d] [--archive <id> | --fresh]"),
    )?;
    let mut detached = session == "--all";
    if session.starts_with('-') && session != "--all" {
        return Err(Error::new(
            1,
            format!("unknown {command} option: {session}"),
        ));
    }
    let mut archive = None;
    let mut fresh = false;
    let mut expected_run = None;
    let mut index = 1;
    while index < args.len() {
        match args[index].as_str() {
            "--dry-run" => dry = true,
            "-d" if command == "resume" => detached = true,
            "-d" => return Err(Error::new(1, "-d is only valid with resume")),
            "--archive" if command == "resume" && session != "--all" && archive.is_none() => {
                archive = Some(require(args, index + 1, "--archive needs an archive ID")?);
                index += 1;
            }
            "--fresh" if command == "resume" && session != "--all" => fresh = true,
            "--expected-run-id" if command == "resume" && expected_run.is_none() => {
                expected_run = Some(require(args, index + 1, "--expected-run-id needs a run ID")?);
                index += 1;
            }
            option => return Err(Error::new(1, format!("unknown {command} option: {option}"))),
        }
        index += 1;
    }
    if fresh && archive.is_some() {
        return Err(Error::new(1, "--fresh cannot restore an archive"));
    }
    if expected_run.is_some() && !fresh {
        return Err(Error::new(1, "--expected-run-id is only valid with --fresh"));
    }
    if command == "resume" && !detached && !dry && terminal().inside() {
        return Err(Error::new(
            2,
            "already inside tmux; use resume -d or open a new tab",
        ));
    }
    let mut state_args = vec![session.into()];
    if let Some(id) = archive {
        state_args.extend(["--archive".into(), id.into()]);
    }
    if let Some(run) = expected_run {
        state_args.extend(["--expected-run-id".into(), run.into()]);
    }
    if dry {
        state_args.push("--dry-run".into());
    }
    env::set_var("HGS_CLIENT", client);
    let rc = state_command(if fresh { "fresh" } else { command }, &state_args)?;
    if rc != 0 || dry || command != "resume" || detached || state::dsh::exists(session) || state::opencode::exists(session) {
        return Ok(rc);
    }
    attach(config, session, client, false)
}

#[cfg(unix)]
fn notify(config: &Config, args: &[String], dry: bool) -> Result<i32> {
    use std::{
        io::Write,
        time::{SystemTime, UNIX_EPOCH},
    };
    let bell = args.first().is_some_and(|s| s == "--bell");
    let args = if bell { &args[1..] } else { args };
    let session = require(args, 0, "usage: hgs notify [--bell] <session> [text...]")?;
    let mut text = args[1..].join(" ");
    if text.is_empty() {
        text = if bell {
            "rang the bell"
        } else {
            "waiting for you"
        }
        .into();
    }
    let path = env::var("HGS_EVENTS")
        .ok()
        .filter(|p| !p.is_empty())
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            if platform::is_macos() {
                PathBuf::from(format!(
                    "{}/Library/Application Support/hgs/events",
                    config.home
                ))
            } else {
                PathBuf::from(
                    env::var("XDG_RUNTIME_DIR")
                        .unwrap_or_else(|_| format!("/tmp/hgs-{}", unsafe { libc::getuid() })),
                )
                .join("hgs/events")
            }
        });
    if dry {
        println!("notify {} {session}", path.display());
        return Ok(0);
    }
    // Never create or spool events when the tray is absent.
    if !path.is_file() {
        return Ok(0);
    }
    let mut file = match fs::OpenOptions::new().append(true).open(&path) {
        Ok(file) => file,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(0),
        Err(e) => return Err(e.into()),
    };
    let line = format!(
        "{}\n",
        json!({"session":session,"host":config.host(),"text":text,"bell":bell,"ts":SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_secs()})
    );
    // O_APPEND plus a single write preserves records across simultaneous short notifications.
    file.write_all(line.as_bytes())?;
    Ok(0)
}
fn project(config: &Config, args: &[String], dry: bool) -> Result<i32> {
    let subcommand = args.first().map(String::as_str).unwrap_or("ls");
    let args = if args.is_empty() { args } else { &args[1..] };
    match subcommand {
        "ls" => {
            let projects = config.projects()?;
            if has(args, "--json") {
                println!("{}", json!(projects.iter().map(|p| json!({"name":p.name,"dir":p.dir,"src":p.src,"exists":Path::new(&p.dir).is_dir()})).collect::<Vec<_>>()));
            } else {
                for p in projects {
                    println!(
                        "{} {:<24} {:<8} {}",
                        if Path::new(&p.dir).is_dir() { " " } else { "!" },
                        p.name,
                        p.src,
                        p.dir
                    );
                }
            }
        }
        "add" | "set" => {
            let usage = format!("usage: hgs project {subcommand} <name> <dir>");
            let name = require(args, 0, &usage)?;
            let directory = require(args, 1, &usage)?;
            config::check_name(name)?;
            let expanded = config.expand_home(directory);
            config::check_directory(&expanded)?;
            let path = absolute_directory(&expanded)
                .map_err(|_| Error::new(1, format!("no such directory: {directory}")))?;
            let directory = path.to_string_lossy();
            config::check_directory(&directory)?;
            if subcommand == "add" {
                if let Some(p) = config.projects()?.into_iter().find(|p| p.name == name) {
                    return Err(Error::new(1, format!("project '{name}' already exists ({}); use 'hgs project set' to override it", p.src)));
                }
            }
            if dry {
                println!(
                    "project {subcommand} {name}={directory} -> {}/projects.local",
                    config.dir.display()
                );
            } else {
                config.write_project(name, Some(&directory), subcommand == "add")?;
            }
        }
        "rm" => {
            let name = require(args, 0, "usage: hgs project rm <name>")?;
            match config
                .projects()?
                .into_iter()
                .find(|p| p.name == name)
                .map(|p| p.src)
            {
                Some(source) if source == "local" => {}
                Some(_) => return Err(Error::new(1, format!("unknown project '{name}'"))),
                None => return Err(Error::new(1, format!("unknown project '{name}'"))),
            }
            if dry {
                println!(
                    "project rm {name} <- {}/projects.local",
                    config.dir.display()
                );
            } else {
                config.write_project(name, None, false)?;
            }
        }
        _ => {
            return Err(Error::new(
                1,
                "usage: hgs project ls [--json] | add <name> <dir> | set <name> <dir> | rm <name>",
            ))
        }
    }
    Ok(0)
}
fn logical_cwd() -> std::io::Result<PathBuf> {
    let actual = env::current_dir()?;
    Ok(env::var("PWD")
        .ok()
        .filter(|p| {
            Path::new(p).is_absolute() && platform::canonical(p).ok().as_ref() == Some(&actual)
        })
        .map(PathBuf::from)
        .unwrap_or(actual))
}
fn absolute_directory(path: &str) -> std::io::Result<PathBuf> {
    // Match `cd -L; pwd`: a project symlink's visible name remains the session name.
    // Canonicalizing here would rename ~/projects/site to its target's basename.
    let path = Path::new(path);
    let absolute = if path.is_absolute() {
        path.to_path_buf()
    } else {
        logical_cwd()?.join(path)
    };
    let mut directory = PathBuf::new();
    for component in absolute.components() {
        match component {
            std::path::Component::CurDir => {}
            std::path::Component::ParentDir => {
                directory.pop();
            }
            component => directory.push(component.as_os_str()),
        }
    }
    if !directory.is_dir() {
        return Err(std::io::Error::new(
            std::io::ErrorKind::NotADirectory,
            "not a directory",
        ));
    }
    Ok(directory)
}

#[derive(Default)]
struct LaunchOptions {
    account: Option<String>,
    model: Option<String>,
    launch_id: Option<String>,
    project: String,
    tag: String,
    fresh: bool,
    new_only: bool,
    resume: bool,
    resume_id: Option<String>,
    remote_control: bool,
    dry: bool,
    detached: bool,
    bare: bool,
    extra: Vec<String>,
}
fn conversation_id(value: &str) -> Option<String> {
    let (prefix, value) = match value.strip_prefix("session_") {
        Some(value) => ("session_", value),
        None => ("", value),
    };
    uuid::Uuid::parse_str(value)
        .ok()
        .map(|id| format!("{prefix}{id}"))
}
impl LaunchOptions {
    fn parse(args: &[String], dry: bool) -> Result<Self> {
        let mut options = Self {
            dry,
            ..Self::default()
        };
        let mut i = 0;
        while let Some(arg) = args.get(i) {
            match arg.as_str() {
                "-n" => {
                    options.tag = require(args, i + 1, "-n needs a tag")?.into();
                    i += 1;
                }
                "--model" => { options.model=Some(require(args,i+1,"--model needs provider/model")?.into()); i+=1; }
                "--account" => {
                    options.account =
                        Some(require(args, i + 1, "--account needs a profile ID")?.into());
                    i += 1;
                }
                "--launch-id" => {
                    let id =
                        uuid::Uuid::parse_str(require(args, i + 1, "--launch-id needs a UUID")?)
                            .map_err(|_| Error::new(1, "--launch-id needs a UUID"))?;
                    if id.is_nil() || options.launch_id.replace(id.to_string()).is_some() {
                        return Err(Error::new(1, "provide one non-empty launch UUID"));
                    }
                    i += 1;
                }
                "-d" => options.detached = true,
                "-c" | "--resume" | "--continue" => {
                    options.resume = true;
                    if let Some(id) = args.get(i + 1).and_then(|value| conversation_id(value)) {
                        if options.resume_id.replace(id).is_some() {
                            return Err(Error::new(1, "only one conversation ID can be requested"));
                        }
                        i += 1;
                    }
                }
                option if option.starts_with("--resume=") || option.starts_with("--continue=") => {
                    let value = option.split_once('=').unwrap().1;
                    let id = conversation_id(value)
                        .ok_or_else(|| Error::new(1, "--resume=ID needs a conversation UUID"))?;
                    if options.resume_id.replace(id).is_some() {
                        return Err(Error::new(1, "only one conversation ID can be requested"));
                    }
                    options.resume = true;
                }
                "--fresh" => options.fresh = true,
                "--new" => options.new_only = true,
                "--bare" => options.bare = true,
                "--rc" => options.remote_control = true,
                "--dry-run" => options.dry = true,
                "--" => {
                    options.extra = args[i + 1..].to_vec();
                    break;
                }
                option if option.starts_with('-') => {
                    return Err(Error::new(
                        1,
                        format!("unknown option {option} (agent flags go after --)"),
                    ))
                }
                name if options.project.is_empty() => options.project = name.into(),
                name => {
                    return Err(Error::new(
                        1,
                        format!("unexpected argument {name} (agent flags go after --)"),
                    ))
                }
            }
            i += 1;
        }
        nested_guard(options.dry)?;
        if options.launch_id.is_some() && !options.new_only {
            return Err(Error::new(1, "--launch-id requires --new"));
        }
        if options.resume_id.is_some() && options.fresh {
            return Err(Error::new(
                1,
                "an exact conversation ID cannot be combined with --fresh",
            ));
        }
        if options.new_only {
            if options.resume || options.fresh {
                return Err(Error::new(
                    1,
                    "--new cannot be combined with --resume or --fresh",
                ));
            }
            if options.tag.is_empty() {
                options.tag = format!("work-{}", &uuid::Uuid::new_v4().simple().to_string()[..8]);
            }
            // Same tags as rename and fork; the GUI validates with SessionTag::problem.
            if options.tag.trim() != options.tag
                || options.tag.contains(['/', ':', '.'])
                || options.tag.chars().any(char::is_control)
            {
                return Err(Error::new(
                    1,
                    "new session name cannot contain '/', ':', '.', control characters or surrounding whitespace",
                ));
            }
        }
        Ok(options)
    }
}
fn resolve_project(
    config: &Config,
    command: &str,
    options: &LaunchOptions,
) -> Result<(String, String)> {
    let project = &options.project;
    if project.is_empty() {
        let directory = logical_cwd()?.to_string_lossy().into_owned();
        return Ok((directory.clone(), folder_name(&directory)));
    }
    // A Windows folder may also be `C:\work`, `work\site` or `C:work`.
    let windows_path = cfg!(windows) && (project.contains('\\') || project.get(1..2) == Some(":"));
    if project.contains('/')
        || project.starts_with('~')
        || matches!(project.as_str(), "." | "..")
        || windows_path
    {
        let directory = absolute_directory(&config.expand_home(project))
            .map_err(|_| Error::new(1, format!("no such directory: {project}")))?
            .to_string_lossy()
            .into_owned();
        return Ok((directory.clone(), folder_name(&directory)));
    }
    let projects = config.projects()?;
    if let Some(found) = projects.iter().find(|p| p.name == *project) {
        let directory = config.expand_home(&found.dir);
        if !Path::new(&directory).is_dir() {
            return Err(Error::new(
                1,
                format!("project '{project}' points to missing dir {directory}"),
            ));
        }
        return Ok((directory, project.clone()));
    }
    let session = format!(
        "{}/{}{}",
        basename(command),
        sanitize(project),
        if options.tag.is_empty() {
            String::new()
        } else {
            format!("/{}", options.tag)
        }
    );
    if session_exists(&session) {
        return Ok((String::new(), project.clone()));
    }
    Err(Error::new(
        1,
        format!(
            "unknown project '{project}' and no session {}/{}; known: {}",
            basename(command),
            sanitize(project),
            projects
                .iter()
                .map(|p| p.name.as_str())
                .collect::<Vec<_>>()
                .join(" ")
        ),
    ))
}
fn launch(
    config: &Config,
    command: &str,
    args: &[String],
    client: &str,
    global_dry: bool,
) -> Result<i32> {
    let mut options = LaunchOptions::parse(args, global_dry)?;
    let (directory, project) = resolve_project(config, command, &options)?;
    let command_name = basename(command);
    if command_name=="opencode" {
        if options.account.as_deref().is_some_and(|id|id!="native-opencode") {return Err(Error::new(1,"Choose native-opencode; separate OpenCode profiles are not supported"));}
        if options.resume || !options.extra.is_empty() || options.fresh || options.remote_control {return Err(Error::new(1,"Use saved HGS session names to resume OpenCode; select a model with --model provider/model"));}
        let project=if project=="/" {"root".into()}else{sanitize(&project)};
        let tag=if options.tag.is_empty(){"main"}else{&options.tag};let name=format!("opencode/{project}/{tag}");
        state::opencode::launch(&name,&directory,options.launch_id.as_deref(),options.model.as_deref(),options.dry,options.new_only).map_err(|e|Error::new(1,e))?;
        if !options.dry {println!("OpenCode session ready: {name}\nOpen this session in Zerus.");}return Ok(0);
    }
    if options.model.is_some() {return Err(Error::new(1,"--model is an OpenCode launch option; other agent flags go after --"));}
    if command_name == "dsh" {
        if options
            .account
            .as_deref()
            .is_some_and(|id| id != "native-dsh")
        {
            return Err(Error::new(
                1,
                "Choose the native DeepSeek account; manage its login in the native UI",
            ));
        }
        if options.resume_id.is_some() || !options.extra.is_empty() || options.fresh {
            return Err(Error::new(
                1,
                "Use HGS session names to resume DeepSeek; native CLI flags belong in dsh itself",
            ));
        }
        let project = if project == "/" {
            "root".into()
        } else {
            sanitize(&project)
        };
        let name = format!(
            "dsh/{project}{}",
            if options.tag.is_empty() {
                String::new()
            } else {
                format!("/{}", options.tag)
            }
        );
        state::dsh::launch(
            &name,
            &directory,
            options.launch_id.as_deref(),
            options.dry,
            options.new_only,
        )
        .map_err(|e| Error::new(1, e))?;
        if !options.dry {
            println!("DeepSeek session ready: {name}\nOpen this session in Zerus.");
        }
        return Ok(0);
    }
    let explicit_account = options.account.is_some();
    // Exact conversation lookup is scoped to the chosen account's native home.
    if !explicit_account && options.resume_id.is_some() {
        options.account = crate::accounts::default_account(config, &command_name)?;
    }
    let mut account_environment = if let Some(id) = &options.account {
        crate::accounts::select(config, id, &command_name)?
    } else {
        Vec::new()
    };
    let requested_session = if let Some(id) = &options.resume_id {
        if !matches!(command_name.as_str(), "claude" | "codex" | "kimi") {
            return Err(Error::new(
                1,
                "resuming a conversation ID requires claude, codex or kimi",
            ));
        }
        if options.tag.is_empty() {
            let found = state::find_requested_session(&command_name, id)
                .map_err(|error| Error::new(1, error))?;
            options.tag = format!("resume-{id}");
            found
        } else {
            None
        }
    } else {
        None
    };
    let project = if project == "/" {
        "root".into()
    } else {
        sanitize(&project)
    };
    let session = requested_session.unwrap_or_else(|| {
        format!(
            "{command_name}/{project}{}",
            if options.tag.is_empty() {
                String::new()
            } else {
                format!("/{}", options.tag)
            }
        )
    });
    let existing = session_exists(&session);
    if explicit_account
        && (existing || (!options.fresh && binding_exists(config, &session)?))
    {
        return Err(Error::new(1, "This session already has an account. Open it from Sessions, or choose a new session name."));
    }
    let mut requested_saved = false;
    if let Some(id) = &options.resume_id {
        if existing || binding_exists(config, &session)? {
            requested_saved = state::check_requested_session(&session, &command_name, id)
                .map_err(|error| Error::new(1, error))?;
        }
    }
    if options.new_only && (existing || binding_exists(config, &session)?) {
        return Err(Error::new(
            1,
            format!(
                "session '{session}' already exists; choose another name or open it from Sessions"
            ),
        ));
    }
    // Attaching/restoring a named session keeps its saved account binding.
    if !explicit_account && options.resume_id.is_none() && !existing
        && (options.fresh || !binding_exists(config, &session)?) {
        options.account = crate::accounts::default_account(config, &command_name)?;
        if let Some(id) = &options.account {
            account_environment = crate::accounts::select(config, id, &command_name)?;
        }
    }
    #[cfg(unix)]
    let shell = env::var("SHELL").unwrap_or_else(|_| "/bin/sh".into());
    let (mut base, mut resume): (Vec<String>, Vec<String>) = match command_name.as_str() {
        "claude" | "kimi" => (
            vec![command_name.clone()],
            vec![command_name.clone(), "-c".into()],
        ),
        "codex" => (
            vec!["codex".into()],
            vec!["codex".into(), "resume".into(), "--last".into()],
        ),
        #[cfg(unix)]
        "sh" => (vec![shell.clone(), "-l".into()], Vec::new()),
        // PowerShell, untracked like the Unix login shell (spec D8).
        #[cfg(windows)]
        "sh" => (
            platform::powershell().map_err(|e| Error::new(1, e))?,
            Vec::new(),
        ),
        _ => (vec![command.into()], Vec::new()),
    };
    let launcher = Path::new(&directory).join(format!("run_{command_name}.sh"));
    if !options.bare && !directory.is_empty() && platform::executable(&launcher) {
        base[0] = launcher.to_string_lossy().into_owned();
        if !resume.is_empty() {
            resume[0] = base[0].clone();
        }
    }
    if let Some(id) = &options.resume_id {
        base.extend([
            match command_name.as_str() {
                "codex" => "resume",
                "claude" => "--resume",
                "kimi" => "--session",
                _ => unreachable!(),
            }
            .into(),
            id.clone(),
        ]);
    }
    let mut auto = options.resume && !options.fresh && options.resume_id.is_none();
    if auto && resume.is_empty() {
        eprintln!("hgs: -c: no resume mode known for {command_name}; starting it as is");
        auto = false;
    }
    if command_name == "claude" && auto {
        let history_root = env::var("HGS_CLAUDE_PROJECTS").unwrap_or_else(|_| {
            let home = env::var("CLAUDE_CONFIG_DIR")
                .ok()
                .filter(|value| !value.is_empty())
                .unwrap_or_else(|| format!("{}/.claude", config.home));
            Path::new(&home)
                .join("projects")
                .to_string_lossy()
                .into_owned()
        });
        let escaped: String = directory
            .chars()
            .map(|c| if c.is_ascii_alphanumeric() { c } else { '-' })
            .collect();
        auto = fs::read_dir(Path::new(&history_root).join(escaped))
            .ok()
            .is_some_and(|entries| {
                entries
                    .filter_map(|e| e.ok())
                    .any(|entry| entry.path().extension().is_some_and(|ext| ext == "jsonl"))
            });
    }
    if !options.extra.is_empty() {
        match command_name.as_str() {
            "codex" => auto = false,
            "claude"
                if options
                    .extra
                    .iter()
                    .any(|arg| matches!(arg.as_str(), "-c" | "--continue" | "-r" | "--resume")) =>
            {
                auto = false
            }
            "kimi"
                if options.extra.iter().any(|arg| {
                    matches!(arg.as_str(), "-c" | "--continue" | "-S" | "--session")
                }) =>
            {
                auto = false
            }
            _ => {}
        }
        base.extend(options.extra.clone());
        if !resume.is_empty() {
            resume.extend(options.extra.clone());
        }
    }
    if options.remote_control {
        if command_name == "claude" {
            base.extend(["--remote-control".into(), session.clone()]);
            resume.extend(["--remote-control".into(), session.clone()]);
        } else {
            eprintln!("hgs: --rc is claude-only; ignored for {command_name}");
        }
    }
    let executable = executable_path()?.to_string_lossy().into_owned();
    // The Unix login shell is the pane's root process. On Windows PowerShell
    // runs under `--run` as well: the host starts every root process as a new
    // process group, which ignores Ctrl+C together with everything it starts,
    // and only the supervisor turns Ctrl+C back on for its child (spec D11).
    let command = if command_name == "sh" && cfg!(unix) {
        base
    } else {
        #[cfg(unix)]
        let mut words = vec![
            shell,
            "-lic".into(),
            if account_environment.is_empty() {
                "exec \"$0\" --run \"$@\"".into()
            } else {
                format!(
                    "exec env {} \"$0\" --run \"$@\"",
                    platform::join(&account_environment)
                )
            },
            executable.clone(),
        ];
        // The supervisor starts directly, with the session's variables, which
        // carry the account (spec D8).
        #[cfg(windows)]
        let mut words = vec![executable.clone(), "--run".to_owned()];
        if auto {
            words.push(resume.len().to_string());
            words.extend(resume);
        } else {
            words.push("0".into());
        }
        words.extend(base);
        words
    };
    if existing {
        if options.dry {
            if !options.detached && config.tab {
                println!(
                    "tab {session} color={}",
                    platform::tab_color(config, "local")
                );
            }
            return attach(config, &session, client, true);
        }
        if let Some(id) = &options.resume_id {
            eprintln!("hgs: conversation {id} is running as {session}");
        } else if options.fresh
            || options.resume
            || options.remote_control
            || !options.extra.is_empty()
        {
            eprintln!("hgs: session {session} exists, attaching; launch options ignored");
        }
        if options.detached {
            println!("hgs: session {session} already running");
            return Ok(0);
        }
        if let Some(id) = &options.resume_id {
            return attach_requested(config, &session, client, &command_name, id);
        }
        return attach(config, &session, client, false);
    }
    let mut environment = vec![format!("HGS_CLIENT={client}")];
    if let Some(id) = &options.launch_id {
        // Set only on the session actually created by this command. A name
        // collision must never claim another session for the GUI's group.
        environment.push(format!("HGS_LAUNCH_ID={id}"));
    }
    environment.extend(account_environment);
    environment.extend(platform::gui_env());
    if !options.fresh
        && (requested_saved || (options.resume_id.is_none() && binding_exists(config, &session)?))
    {
        if !options.detached && !options.dry {
            terminal().attachable()?;
        }
        env::set_var("HGS_CLIENT", client);
        if let Some(id) = &options.resume_id {
            env::set_var("HGS_REQUESTED_ID", id);
        } else {
            env::remove_var("HGS_REQUESTED_ID");
        }
        let mut state_args = vec![session.clone()];
        if options.dry {
            state_args.push("--dry-run".into());
        }
        let rc = state_command("resume", &state_args)?;
        return if rc == 0 && !options.detached && !options.dry {
            attach(config, &session, client, false)
        } else {
            Ok(rc)
        };
    }
    let mut spec = LaunchSpec {
        name: session.clone(),
        cwd: directory,
        env: environment,
        command,
        detached: options.detached,
    };
    if options.dry {
        if !options.detached && config.tab {
            println!(
                "tab {session} color={}",
                platform::tab_color(config, "local")
            );
        }
        println!("{}", terminal().describe(&DryRun::Launch(&spec)));
        return Ok(0);
    }
    if matches!(command_name.as_str(), "claude" | "codex" | "kimi")
        && env::var("HGS_TRACKING").as_deref() != Ok("0")
    {
        spec.env.extend([
            format!("HGS_SESSION={session}"),
            format!("HGS_AGENT={command_name}"),
            format!(
                "HGS_ACCOUNT_ID={}",
                options.account.as_deref().unwrap_or("")
            ),
            format!("HGS_STATE_DIR={}", config.state.display()),
            format!("HGS_EXECUTABLE={executable}"),
            "HGS_RUN_ID=".into(),
            "HGS_EXPECTED_ID=".into(),
            "HGS_ARCHIVE_ID=".into(),
            "HGS_FRESH_REPLACES=".into(),
            format!("HGS_FRESH={}", if options.fresh { "1" } else { "0" }),
            format!(
                "HGS_REQUESTED_ID={}",
                options.resume_id.as_deref().unwrap_or("")
            ),
            "HGS_TRACKING=1".into(),
        ]);
        for key in ["CLAUDE_CONFIG_DIR", "CODEX_HOME", "KIMI_CODE_HOME"] {
            spec.env
                .push(format!("{key}={}", env::var(key).unwrap_or_default()));
        }
    }
    if options.detached {
        let rc = terminal().launch(&spec)?.code;
        if rc == 0 {
            if let Some(id) = &options.launch_id {
                // A session that ends before a listing shows it still ends
                // its launch's placeholder (`state::launches`).
                if let Err(error) = state::launches::started(id, &session) {
                    eprintln!("hgs: cannot record launch {id}: {error}");
                }
            }
            println!("hgs: started {session}");
        }
        return Ok(rc);
    }
    let _tab = TabGuard::begin(config, &session, &platform::tab_color(config, "local"));
    let start = Instant::now();
    let exit = terminal().launch(&spec)?;
    let rc = exit.code;
    if !report_startup_failure(&session)
        && !exit.server_died
        && !session_exists(&session)
        && start.elapsed().as_secs() < 3
    {
        eprintln!("hgs: session {session} ended immediately — command not found on this box? try --dry-run");
    }
    Ok(rc)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn session_names_take_the_on_disk_case_of_the_same_folder_only() {
        let name = |logical: &str, canonical: Option<&str>| {
            on_disk_name(logical.into(), canonical.map(str::to_owned))
        };
        // `cd C:\work\myproj` names the session after the folder on disk.
        assert_eq!(name("myproj", Some("MyProj")), "MyProj");
        assert_eq!(name("SITE", Some("site")), "site");
        assert_eq!(name("ölaf", Some("Ölaf")), "Ölaf");
        assert_eq!(name("MyProj", Some("MyProj")), "MyProj");
        // A junction, link or short name with another name keeps its own.
        assert_eq!(name("site", Some("example-site")), "site");
        assert_eq!(name("PROGRA~1", Some("Program Files")), "PROGRA~1");
        assert_eq!(name("myproj", Some("myproj2")), "myproj");
        assert_eq!(name("myproj2", Some("MyProj")), "myproj2");
        // Only letter case may differ, never the letters themselves.
        assert_eq!(name("STRASSE", Some("straße")), "STRASSE");
        // A folder that cannot be resolved keeps the name it was given.
        assert_eq!(name("myproj", None), "myproj");
        // Unix names stay exactly as given, even for a link to another case.
        #[cfg(unix)]
        {
            let root = tempfile::tempdir().unwrap();
            std::fs::create_dir(root.path().join("Site")).unwrap();
            let link = root.path().join("site");
            // A case-insensitive file system (macOS) already finds `Site` as `site`.
            if !link.exists() {
                std::os::unix::fs::symlink("Site", &link).unwrap();
            }
            assert_eq!(folder_name(&link.to_string_lossy()), "site");
        }
    }

    #[test]
    fn crashes_and_stops_of_a_resume_are_final_and_quick_failures_start_fresh() {
        // A quick ordinary failure means nothing to resume.
        assert!(!resume_stands(1, 0, 5, false));
        assert!(!resume_stands(127, 4, 5, false));
        // Success, a run long enough to have resumed, a signal or crash.
        assert!(resume_stands(0, 0, 5, false));
        assert!(resume_stands(1, 5, 5, false));
        assert!(resume_stands(130, 0, 5, false));
        // Windows: Ctrl+Break or a closed console (STATUS_CONTROL_C_EXIT), an
        // access violation, a missing DLL (spec D9) all read as signals.
        for status in [0xC000_013A_u32, 0xC000_0005, 0xC000_0135] {
            let rc = platform::ntstatus_exit(status).exit_code();
            assert!(rc >= 128, "{status:#x}");
            assert!(resume_stands(rc, 0, 5, false), "{status:#x}");
        }
        // exit(-1) stays an ordinary failure.
        let rc = platform::ntstatus_exit(u32::MAX).exit_code();
        assert_eq!(rc, -1);
        assert!(!resume_stands(rc, 0, 5, false));
        // A paused agent that exits with an ordinary failure was still stopped.
        assert!(resume_stands(1, 0, 5, true));
    }
}
