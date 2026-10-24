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
    borrow::Cow,
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

pub(crate) const USAGE: &str = r#"usage: hgs [@host] <cmd> [project] [-c [ID]] [-n tag] [-d] [--rc] [--dry-run] [-- args...]
       hgs mobile-peers --json        guarded direct-peer discovery (mobile-peer ABI 1)
       hgs approval-log begin|finish|list|detail  local automatic-approval journal (JSON stdin)
       hgs mobile-peer --json         guarded direct-peer native request from JSON stdin
       hgs [@host] voice <session> --json  send and inspect persistent voice messages (JSON stdin; voice-video-v1)
       hgs [@host] speech --json      host speech providers and durable jobs (JSON stdin)
       hgs [@host] analytics --json   private bounded computer analytics (JSON stdin)
       hgs [@host] mobile phones|invite|revoke-phone <id> [--workspace ID --server URL] --json   phones paired through this mobile connection
       Speech profile management v1
       hgs [@host] a <session>        attach by full name (see hgs ls)
       hgs [@host] a <session> --existing [--run-id ID]   attach only to this live run
       hgs [@host] ls                 sessions here and on peers
       hgs ls --json --local          this box as JSON (for the tray; no ssh)
       hgs [@host] status [--json] [--local]   version, sessions and background services here and on peers
       hgs [@host] version            this hgs version (also --version)
       hgs [@host] hooks <session> --json   read scoped hook health (hooks-health-v1)
       hgs [@host] hooks --account ID [--repair] --json   inspect or explicitly repair handlers
       hgs [@host] inspect <session> [--archive <id>] [--after <cursor>]   activity as JSON
       hgs [@host] reference <session> --json [--archive ID] [--agent ID]   copy a session reference
       hgs self [--json]              the calling agent's own session reference
       hgs [@host] guide [topic]      how agents message, wait for, launch and stop sessions
       hgs inspect 'zerus://session?...'   resolve and inspect a referenced conversation
       hgs [@host] processes <session> [--archive <id>]   observed shell commands as JSON
       hgs [@host] processes <session> --output ID | --stop ID --run RUN --conversation ID [--generation ID]
       hgs [@host] send <session> --json   submit message/attachments from JSON stdin
       hgs [@host] recovery action --scoped-json exact waiting-job control
       hgs [@host] history <session> --json bounded exact public history
       hgs [@host] history-projection <session> --json retired and refreshed history rows
       hgs [@host] graph snapshot --json   bounded saved relationship metadata (partial coverage)
       hgs [@host] wait begin|ack|finish|cancel|list --json   explicit independent-agent waits (JSON stdin)
       hgs [@host] terminal <session> --json scoped terminal screen/input
       hgs [@host] AGENT DIRECTORY --new -n TAG -d --launch-id UUID
       hgs [@host] session-action <session> --json scoped native lifecycle action
       hgs [@host] compact-context <session> --json compact the idle agent context using its native command
       hgs [@host] clear-context <session> --json   clear the idle agent context using its native command
       hgs [@host] interrupt <session> --json   interrupt the current turn, keep the session
       hgs [@host] attachment <session> --request ID --index N --conversation ID   read a sent attachment
       hgs [@host] file-read <session> --json   read a document on the conversation computer
       hgs @host download --path REMOTE_PATH --output LOCAL_FILE   download a file through configured SSH
       hgs [@host] answer <session> --json   answer an exact pending question from JSON stdin
       hgs [@host] read <session> --json   mark the current reply/attention read (JSON stdin)
       hgs [@host] notifications <session> --json   shared mute preference (session-notifications-v1)
       hgs [@host] fork <session> [-n name] [-d] [--archive ID]  branch into a new conversation
       hgs [@host] effort <session> --json   change this agent's effort from JSON stdin
       hgs [@host] settings <session> --json model and effort for this session (JSON stdin)
       hgs [@host] recovery get | set | action   automatic recovery policy (JSON stdin for writes)
       hgs [@host] search --query <text> [--limit N]   search saved session contents
       hgs [@host] schedules <session> [--json]   native DeepSeek Schedule tasks (JSON stdin changes)
       hgs machine ls | set <alias> --json <profile> | remove <alias> | reset <alias>
       hgs machine resolve <alias>          effective SSH settings (no connection)
       hgs swarm get | preview PEER | join PEER | sync | worker   shared project catalog
       hgs swarm assign-launch --json   assign an exact launch to its selected project
       hgs swarm session-project --json  read an exact session's logical project choices
       hgs swarm assign-session --json   move only the exact session's project grouping
       hgs presence set --json        desktop idle/lock time from JSON stdin (written by the tray)
       hgs swarm preference get --json   shared phone notification timing
       hgs swarm preference set mobile_delivery VALUE --json   immediate|away|away_5|away_10|away_15|away_30
       hgs machine check <alias> | ssh <alias> [--directory PATH] | setup <alias> [--source <checkout>]
       hgs [@host] account ls | add ID --provider AGENT --label NAME | rm ID | restore ID | login ID
       hgs [@host] account rename ID --label NAME | inspect ID [--refresh]
       hgs [@host] account default [ID]   show or select the default account for new sessions
       hgs [@host] account set-key native-dsh --json   save API key from private JSON stdin
       hgs [@host] account start native-opencode   start the shared OpenCode service if none runs; inspection never starts it
       hgs [@host] account favorite native-opencode --model PROVIDER/MODEL --favorite on|off --request-id UUID   star an OpenCode model on that computer
       hgs [@host] account models native-opencode [--location DIR]   every OpenCode model of that computer and folder as JSON; never starts OpenCode
       hgs [@host] account install codex|claude|kimi|dsh   install an agent in this terminal
       hgs [@host] account inspect --session SESSION [--refresh]
       hgs account copy ID --from MACHINE --to MACHINE --as NEW_ID [--label NAME]
       hgs [@host] worktrees --path PATH [--refresh] [--json]   existing checkouts as JSON
       hgs [@host] git-status --path PATH --json   read-only checkout/publication status
       hgs [@host] worktrees create --path REPO --branch NAME --destination PATH [--base HEAD] [--json]
       hgs [@host] worktrees review --path PATH [--worktree WORKTREE] [--json]   cleanup verdicts and fingerprints
       hgs [@host] worktrees inspect --path WORKTREE --common-dir DIR [--file RELATIVE_PATH] [--json]   bounded read-only changes
       hgs [@host] worktrees remove --path WORKTREE --common-dir DIR --fingerprint HASH [--ignored-fingerprint HASH] [--discard-fingerprint HASH] [--delete-branch] [--json]
       Mobile worktree ABI: worktrees-v1 (catalog/create/verified project placement)
       hgs [@host] repository check|clone --url URL --parent PATH --name NAME --json
       hgs [@host] repository status --request-id UUID --json
       Repository acquisition ABI: repository-clone-v1
       Mobile repository acquisition ABI: repository-acquisition-v1
       Mobile Git status ABI: git-status-v1
       Mobile project assignment ABI: session-project-v1
       Mobile clear-context ABI: dismiss-prompt-v1
       session-notifications-v1
       hgs [@host] dirs [--hidden] [path]   directories as JSON (default: home)
       hgs [@host] dirs --create <path> [--request-id UUID]   create one folder (existing parent)
       hgs [@host] kill <session> [--archive <id>]
       hgs [@host] rename <session> <new-full-name> [--archive <id>]
       hgs [@host] archives --json           date-filtered archive pages (JSON stdin)
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
  --effort VARIANT   OpenCode effort; needs --model, one of that model's effort_options (omit for the model's default)
  --new    always create a separate session; generate a name unless -n is given
  -d       start detached (do not attach)      --fresh  explicitly replace a saved conversation
  --rc     claude --remote-control (phone)     --bare   ignore a project run_<cmd>.sh/.ps1 launcher
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

/// Fails where this computer runs no local sessions (a Windows computer whose local
/// sessions are not enabled). Forwarded `@host` commands never reach it.
fn local_sessions() -> Result<()> {
    if platform::local_sessions() {
        Ok(())
    } else {
        Err(Error::new(2, platform::LOCAL_SESSIONS_UNSUPPORTED))
    }
}

// Repository preparation owns only its checkout and receipt. Like the existing
// configuration/control commands, it does not require or enable a session host.
fn needs_local_sessions(command: &str) -> bool {
    !matches!(command, "ls" | "machine" | "presence" | "swarm" | "project" | "account" | "speech" | "analytics" | "graph" | "wait" | "mobile" | "hooks" | "repository")
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
        "One-hop mobile peer routing is unavailable on a Windows gateway; local Windows sessions are supported.",
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
/// Refuse a launch or attach inside a session pane, `-d` or not; only `fork -d`
/// and `resume -d` start a session from inside one.
fn nested_guard(dry: bool) -> Result<()> {
    if !dry && terminal().inside() {
        let name = terminal().enclosing_session();
        return Err(Error::new(2, nested_refusal(cfg!(unix), Some(&name), None)));
    }
    Ok(())
}
/// Why a command refuses to run inside a session pane (`$TMUX` on Unix,
/// `HGS_HOST_PANE` in the Windows session host, spec D8): the enclosing
/// `session` when it is looked up ("" when unknown) and the detached command to
/// `retry` with. Only tmux is named, so Windows sessions are not.
fn nested_refusal(tmux: bool, session: Option<&str>, retry: Option<&str>) -> String {
    let inside = match (tmux, session) {
        (true, Some(name)) => format!("tmux session {name}"),
        (true, None) => "tmux".to_owned(),
        (false, Some(name)) if !name.is_empty() => format!("session {name}"),
        (false, _) => "a session".to_owned(),
    };
    match retry {
        Some(command) => format!("already inside {inside}; use {command} or open a new tab"),
        None => format!("already inside {inside}; open a new tab"),
    }
}

#[cfg(test)]
pub(crate) fn usage() -> &'static str {
    USAGE
}
/// Commands that only Unix dispatches: `notify` for the tray, and `clip` and
/// `paste`, which tmux's C-v binding runs.
const UNIX_ONLY_COMMANDS: [&str; 3] = ["notify", "clip", "paste"];
/// Whether a usage line tells of what only Unix has: a command of
/// `UNIX_ONLY_COMMANDS` or the Ctrl+V that runs `hgs paste`.
fn unix_only_usage(line: &str) -> bool {
    let mut words = line.split_whitespace();
    match words.next() {
        Some("hgs") => words
            .next()
            .is_some_and(|word| UNIX_ONLY_COMMANDS.contains(&word)),
        Some("Ctrl+V") => true,
        _ => false,
    }
}
/// Refuses a command of `UNIX_ONLY_COMMANDS` where Unix does not dispatch it
/// (`unix` false: Windows), so that it fails there instead of starting a
/// program of that name.
fn unix_only(command: &str, unix: bool) -> Result<()> {
    if unix || !UNIX_ONLY_COMMANDS.contains(&command) {
        return Ok(());
    }
    Err(Error::new(2, format!("{command} is not supported on Windows")))
}
/// The usage text, `USAGE` byte for byte where tmux hosts the sessions. Windows
/// has no tmux C-v binding, which pulls a peer's image through `hgs paste` and
/// `hgs clip`, no `hgs notify`, and its dry runs print the session host's
/// requests, so its text tells of none of them and does not name tmux.
fn usage_text(tmux: bool) -> Cow<'static, str> {
    if tmux {
        return USAGE.into();
    }
    USAGE
        .lines()
        .filter(|line| !unix_only_usage(line))
        .map(|line| line.replace("the tmux/ssh command", "the session-host/ssh command") + "\n")
        .collect::<String>()
        .into()
}

pub fn dispatch(mut args: Vec<String>) -> Result<i32> {
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
    // An encrypted gateway binds every local native call to the physical
    // machine it advertised. This check also works on Windows, without a
    // shell, stdin rewriting or a second native process.
    if args.first().is_some_and(|a| a == "--expect-machine") {
        let expected = args.get(1).ok_or_else(|| Error::new(2, "Missing expected machine identity"))?;
        if uuid::Uuid::parse_str(expected).ok().is_none_or(|id| id.to_string() != *expected)
            || crate::swarm::local_node_id(&config).map_err(|_| Error::new(1,"Native machine identity is unavailable"))? != *expected
        {
            return Err(Error::new(1,"Mobile target identity changed; refresh computers"));
        }
        args.drain(..2);
        if args.first().is_none_or(|a| a.starts_with('@')
            || (a.starts_with('-') && !(args.len()==1 && matches!(a.as_str(),"--help"|"-h"|"--version")))) {
            return Err(Error::new(2,"A guarded mobile call requires a local command"));
        }
        if args.first().is_some_and(|a| a == "__state") {
            session_process()?;
            return state::dispatch(&args[1..]).map_err(|e| Error::new(1,e));
        }
    }
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
        eprint!("{}", usage_text(cfg!(unix)));
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
            print!("{}", usage_text(cfg!(unix)));
            println!("Encrypted workspace ABI: guarded-local-v1");
            #[cfg(windows)]
            print!("{}\nMobile gateway ABI: windows-local-v1\n", crate::host::admin::HELP);
            return Ok(0);
        }
        "--version" | "version" => return crate::status::version(&config, &target, args),
        // Read-only on every machine; never routed as an agent launch.
        "status" => return crate::status::run(&config, &target, args, dry),
        // Text compiled into this hgs; a peer answers with its own.
        "guide" => {
            if !target.is_empty() && !config.is_self(&target) {
                return remote(&config, &target, command, args, dry);
            }
            return crate::agent_guide::dispatch(args);
        }
        "self" => {
            if !target.is_empty() && !config.is_self(&target) {
                return Err(Error::new(1, "hgs self names the session that runs it; omit @host"));
            }
            if dry {
                return Err(Error::new(1, "hgs self does not support --dry-run"));
            }
            local_sessions()?;
            return crate::session_reference::current(&config, args);
        }
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
    if command == "approval-log" {
        return crate::approval_log::dispatch(&config, &target, args, dry);
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
    if command == "send" && args.len() == 2 && args[1] == "--json" && !dry {
        if let Some(origin) = state::session_graph::send_origin() {
            return state::session_graph::forward_send(&target, args, origin);
        }
    }
    if !target.is_empty() && !config.is_self(&target) {
        return remote(&config, &target, command, args, dry);
    }
    // Commands that only read or change this machine's configuration, or reach
    // remote machines, also run on a build without local sessions.
    if needs_local_sessions(command) {
        local_sessions()?;
    }
    // Only Unix has arms for these below; elsewhere they would reach the launch arm.
    unix_only(command, cfg!(unix))?;
    match command.as_str() {
        "hooks" => state::hook_health::dispatch(&config, args, dry),
        "analytics" => crate::analytics::dispatch(&config, args, dry),
        "speech" => crate::speech::dispatch(&config, args, dry),
        "mobile" => crate::phones::dispatch(&config, args, dry),
        "__dsh-ui-url" => {
            let url = state::dsh::native_ui_url(require(args, 0, "Choose a native session")?)
                .map_err(|e| Error::new(1, e))?;
            println!("{}", json!({"url":url}));
            Ok(0)
        }
        "ls" => list(&config, args, dry),
        "archives" => {
            let mobile = cfg!(windows) && args.last().map(String::as_str)==Some("--windows-mobile");
            let args = if mobile { &args[..args.len()-1] } else { args };
            if (args != ["--json"] && args != ["lookup","--json"]) || dry { return Err(Error::new(1,"usage: hgs archives [lookup] --json (JSON query on stdin)")); }
            let query = state::archive_catalog::read_query().map_err(|e| Error::new(1,e))?;
            let result = if args.first().map(String::as_str) == Some("lookup") {
                if mobile { state::archive_catalog::lookup_scoped(&query,true) } else { state::archive_catalog::lookup(&query) }
            } else if mobile { state::archive_catalog::page_scoped(&query,true) }
            else { state::archive_catalog::page(&query) }.map_err(|e| Error::new(1,e))?;
            println!("{result}"); Ok(0)
        },
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
        "inspect" | "processes" | "worktrees" | "git-status" | "repository" | "dirs" | "send" | "send-now" | "session-action" | "terminal" | "history" | "history-projection" | "interrupt" | "clear-context" | "compact-context" | "answer" | "effort" | "settings" | "search" | "read" | "notifications"
        | "attachment" | "file-read" | "recovery" | "voice" | "schedules" | "graph" | "wait" => {
            if command == "repository" && dry { return Err(Error::new(1,"Repository operations do not support --dry-run")); }
            if command == "wait" && dry { return Err(Error::new(1,"Wait operations do not support --dry-run")); }
            if command == "graph" && dry { return Err(Error::new(1,"Graph reads do not support --dry-run")); }
            if command == "dirs" && has(args, "--create") && dry {
                return Err(Error::new(1, "folder creation does not support --dry-run"));
            }
            if command == "worktrees" && args.first().is_some_and(|s| s == "create" || s == "remove") && dry {
                return Err(Error::new(1, "worktree creation and removal do not support --dry-run"));
            }
            if (matches!(command.as_str(), "processes" | "send" | "send-now" | "session-action" | "terminal" | "interrupt" | "clear-context" | "compact-context" | "answer" | "effort" | "settings" | "recovery" | "read" | "notifications" | "voice" | "schedules") || (command=="attachment" && has(args,"--stage"))) && dry {
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
fn peer_json_archives(config: &Config, peer: &str, archives: bool) -> Value {
    let output = platform::ssh_output(
        platform::ssh()
            .args(crate::machines::ssh_options(config, peer))
            .args([
                "-o",
                "BatchMode=yes",
                "-o",
                "ConnectTimeout=3",
                peer,
                if archives { "~/.local/bin/hgs ls --local --json" } else { "~/.local/bin/hgs ls --local --json --no-archives" },
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
        // Phone management reaches the relay through the gateway peer.
        "mobile" => {
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
        "archives" | "kill" | "terminate" | "pause" | "inspect" | "reference" | "processes" | "worktrees" | "git-status" | "repository" | "dirs" | "project" | "archive" | "rename"
        | "send" | "send-now" | "session-action" | "terminal" | "history" | "history-projection" | "interrupt" | "clear-context" | "compact-context" | "answer" | "effort" | "settings" | "search" | "schedules" | "graph" | "wait" | "guide" | "dsh" | "opencode" | "attachment" | "file-read" | "recovery" | "swarm" | "read" | "notifications" | "speech" | "analytics" | "voice" | "hooks" | "--help" => {
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
    let remote = if command == "repository" {
        platform::remote_sh(&format!("if ~/.local/bin/hgs --help 2>/dev/null | grep -qF 'repository-clone-v1'; then exec ~/.local/bin/hgs {}; else echo 'hgs: update the selected computer to clone repositories' >&2; exit 1; fi",platform::join(&remote_args)))
    } else if command == "wait" {
        // Old peers otherwise interpret an unknown command as an agent launch.
        // Probe the same installed executable without consuming JSON stdin.
        // These probes are POSIX scripts; a fish login shell runs them under sh.
        platform::remote_sh(&format!("if ~/.local/bin/hgs --help 2>/dev/null | grep -qF 'hgs [@host] wait'; then exec ~/.local/bin/hgs {}; else echo 'hgs: update the selected computer to track waits' >&2; exit 1; fi",platform::join(&remote_args)))
    } else if command == "notifications" {
        platform::remote_sh(&format!("if ~/.local/bin/hgs --help 2>/dev/null | grep -qEx '[[:space:]]*session-notifications-v1[[:space:]]*'; then exec ~/.local/bin/hgs {}; else echo 'hgs: update the selected computer to change notification preferences' >&2; exit 1; fi",platform::join(&remote_args)))
    } else if command == "hooks" {
        platform::remote_sh(&format!("if ~/.local/bin/hgs --help 2>/dev/null | grep -qF 'hooks-health-v1'; then exec ~/.local/bin/hgs {}; else echo 'hgs: update the selected computer to inspect hook health' >&2; exit 1; fi",platform::join(&remote_args)))
    } else if command == "analytics" {
        platform::remote_sh(&format!("if ~/.local/bin/hgs --help 2>/dev/null | grep -qF 'hgs [@host] analytics --json'; then exec ~/.local/bin/hgs {} --read-only; else echo 'hgs: update the selected computer to read analytics' >&2; exit 1; fi",platform::join(&remote_args)))
    } else if command == "guide" {
        platform::remote_sh(&format!("if ~/.local/bin/hgs --help 2>/dev/null | grep -qF 'hgs [@host] guide'; then exec ~/.local/bin/hgs {}; else echo 'hgs: the selected computer has no agent guide; check hgs @host -h' >&2; exit 1; fi",platform::join(&remote_args)))
    } else { format!("~/.local/bin/hgs {}", platform::remote_join(&remote_args)) };
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
        println!("{}", peer_json_archives(config, target, !has(&args,"--no-archives")));
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

pub(crate) fn local_json(config: &Config) -> Result<Value> { local_json_archives(config, true) }
fn local_json_archives(config: &Config, archives: bool) -> Result<Value> {
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
    // A session renamed before merge_listing reads the panes keeps its ID.
    let ids = listing
        .sessions
        .iter()
        .map(|row| (row.name.clone(), row.id.clone()))
        .collect();
    let mut merged = state::merge_listing_archives(raw, &ids, archives).map_err(|e| Error::new(1, e))?;
    merged["archive_paging"] = json!(true);
    merged["archive_revision"] = json!(state::archive_catalog::revision().map_err(|e|Error::new(1,e))?);
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
        let archives = !has(args,"--no-archives");
        let local = local_json_archives(config, archives)?;
        if has(args, "--local") {
            println!("{local}");
        } else {
            let mut fleet = vec![local];
            fleet.extend(config.peers.iter().map(|peer| peer_json_archives(config, peer, archives)));
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
    if state::opencode::exists(session) {
        return state::opencode::attach(session, run_id, dry).map_err(|error| Error::new(1, error));
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
        return Err(Error::new(1, state::backend::INVALID_SESSION_ID.text()));
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
            nested_refusal(cfg!(unix), None, Some("fork -d")),
        ));
    }
    env::set_var("HGS_CLIENT", client);
    let target = state::fork_session(name, tag, archive, expected_run, expected_conversation, dry)
        .map_err(|error| Error::new(1, error))?;
    if detached || dry {
        Ok(0)
    } else if state::dsh::exists(&target) {
        crate::native_ui::open(config, "", &target, false)
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
            nested_refusal(cfg!(unix), None, Some("resume -d")),
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
    effort: Option<String>,
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
                "--effort" => { options.effort=Some(require(args,i+1,"--effort needs a model variant")?.into()); i+=1; }
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
        if options.effort.is_some() && options.model.is_none() {return Err(Error::new(1,"--effort needs --model"));}
        let project=if project=="/" {"root".into()}else{sanitize(&project)};
        let tag=if options.tag.is_empty(){"main"}else{&options.tag};let name=format!("opencode/{project}/{tag}");
        state::opencode::launch(&name,&directory,options.launch_id.as_deref(),options.model.as_deref(),options.effort.as_deref(),options.dry,options.new_only).map_err(|e|Error::new(1,e))?;
        if !options.dry {println!("OpenCode session ready: {name}\nOpen this session in Zerus.");}return Ok(0);
    }
    if options.model.is_some() || options.effort.is_some() {return Err(Error::new(1,"--model and --effort are OpenCode launch options; other agent flags go after --"));}
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
    // A project launcher replaces the program, which for a Windows `sh` session
    // is its PowerShell with any options (`powershell.exe -NoLogo`).
    let program = if command_name == "sh" && cfg!(windows) { base.len() } else { 1 };
    crate::project_launcher::apply(
        &directory,
        &command_name,
        options.bare,
        program,
        &mut base,
        &mut resume,
    )
    .map_err(|error| Error::new(1, error))?;
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
    let hint_environment = crate::config::agent_hint_environment(&command_name);
    #[cfg(unix)]
    let supervisor_environment = account_environment
        .iter().chain(&hint_environment).cloned().collect::<Vec<_>>();
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
            crate::accounts::supervisor_script(&supervisor_environment),
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
    environment.extend(hint_environment);
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
    fn repository_preparation_does_not_admit_session_commands_on_control_consoles() {
        for command in ["ls", "machine", "presence", "swarm", "project", "account",
            "speech", "analytics", "graph", "wait", "mobile", "hooks", "repository"] {
            assert!(!needs_local_sessions(command), "{command}");
        }
        for command in ["a", "kill", "send", "send-now", "inspect", "history",
            "history-projection", "processes", "terminal", "session-action", "interrupt",
            "clear-context", "compact-context", "answer", "effort", "settings", "recovery",
            "read", "notifications", "voice", "schedules", "attachment", "file-read",
            "claude", "codex", "kimi", "sh", "resume", "pause", "fork", "reference",
            "archive", "terminate", "dirs", "worktrees", "git-status", "__state", "__host",
            "repository-git-worker", "Repository", "repository-extra", ""] {
            assert!(needs_local_sessions(command), "{command}");
        }
    }

    #[test]
    fn shared_notifications_advertises_its_exact_standalone_marker_on_every_platform() {
        for tmux in [false, true] {
            let help = usage_text(tmux);
            assert_eq!(
                help.lines()
                    .filter(|line| line.trim() == "session-notifications-v1")
                    .count(),
                1
            );
            assert!(help.contains("notifications <session> --json"));
        }
    }

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
    fn inside_a_session_only_tmux_is_named_and_a_launch_is_refused_even_detached() {
        // Unix keeps tmux's wording byte for byte.
        assert_eq!(
            nested_refusal(true, Some("sh/MyProj/work-1"), None),
            "already inside tmux session sh/MyProj/work-1; open a new tab"
        );
        assert_eq!(
            nested_refusal(true, Some(""), None),
            "already inside tmux session ; open a new tab"
        );
        assert_eq!(
            nested_refusal(true, None, Some("fork -d")),
            "already inside tmux; use fork -d or open a new tab"
        );
        assert_eq!(
            nested_refusal(true, None, Some("resume -d")),
            "already inside tmux; use resume -d or open a new tab"
        );
        // A Windows host session is named as the host resolves it, without tmux.
        assert_eq!(
            nested_refusal(false, Some("sh/MyProj/work-1"), None),
            "already inside session sh/MyProj/work-1; open a new tab"
        );
        assert_eq!(
            nested_refusal(false, Some(""), None),
            "already inside a session; open a new tab"
        );
        assert_eq!(
            nested_refusal(false, None, Some("fork -d")),
            "already inside a session; use fork -d or open a new tab"
        );
        assert_eq!(
            nested_refusal(false, None, Some("resume -d")),
            "already inside a session; use resume -d or open a new tab"
        );

        // On either platform a launch inside a session is refused with or
        // without -d, as inside tmux; a dry run is not.
        struct Inside;
        impl TerminalBackend for Inside {
            fn inside(&self) -> bool {
                true
            }
            fn enclosing_session(&self) -> String {
                "sh/MyProj/work-1".into()
            }
        }
        struct Outside;
        impl TerminalBackend for Outside {}
        let launch = |words: &[&str], dry| {
            let args = words.iter().map(|word| word.to_string()).collect::<Vec<_>>();
            LaunchOptions::parse(&args, dry)
        };
        state::backend::with_fake(&Inside, || {
            for words in [&["MyProj", "-n", "x", "-d"][..], &["MyProj", "-n", "x"]] {
                let error = launch(words, false).err().unwrap();
                assert_eq!(error.code, 2);
                assert_eq!(
                    error.message,
                    nested_refusal(cfg!(unix), Some("sh/MyProj/work-1"), None)
                );
            }
            assert!(launch(&["MyProj", "-n", "x", "-d"], true).is_ok());
        });
        state::backend::with_fake(&Outside, || {
            assert!(launch(&["MyProj", "-n", "x", "-d"], false).is_ok());
        });
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

    #[test]
    fn usage_names_tmux_only_where_it_hosts_the_sessions() {
        // Unix keeps tmux's text byte for byte, capability markers included.
        assert_eq!(usage_text(true), USAGE);
        // Windows has no `hgs paste`, `hgs clip` or `hgs notify`, no Ctrl+V
        // that pulls a peer's image, and its dry runs print the host's
        // requests; every other line stays as it is.
        let neutral = usage_text(false);
        assert!(!neutral.contains("tmux"), "{neutral}");
        assert!(!neutral.contains("Ctrl+V"), "{neutral}");
        assert_eq!(
            neutral,
            USAGE
                .replace(
                    "       hgs paste <session> <pane> [client]   the tmux C-v binding; not for hand use\n",
                    ""
                )
                .replace(
                    "       hgs clip                       PNG from this box's clipboard to stdout (rc 1: no image)\n",
                    ""
                )
                .replace(
                    "       hgs notify [--bell] <session> [text]   event for the tray (no tray = no-op)\n",
                    ""
                )
                .replace(
                    "  --dry-run  print the tmux/ssh command\n",
                    "  --dry-run  print the session-host/ssh command\n"
                )
                .replace(
                    "Ctrl+V in a session attached from a peer pulls the image from that box's clipboard.\n",
                    ""
                )
        );
    }

    #[test]
    fn usage_lists_opencode_effort_and_the_explicit_service_start() {
        let usage = usage_text(true);
        // `--effort` is an OpenCode launch option that hgs refuses without
        // `--model` or with an empty value; omitting it keeps the model's default.
        assert!(usage.contains(
            "\n  --effort VARIANT   OpenCode effort; needs --model, one of that model's effort_options (omit for the model's default)\n"
        ), "{usage}");
        // The only explicit start of the shared service; inspection never starts it.
        assert!(usage.contains(
            "\n       hgs [@host] account start native-opencode   start the shared OpenCode service if none runs; inspection never starts it\n"
        ), "{usage}");
        // Clients look for these commands before offering stars and All models.
        assert!(usage.contains(
            "\n       hgs [@host] account favorite native-opencode --model PROVIDER/MODEL --favorite on|off --request-id UUID   star an OpenCode model on that computer\n"
        ), "{usage}");
        assert!(usage.contains(
            "\n       hgs [@host] account models native-opencode [--location DIR]   every OpenCode model of that computer and folder as JSON; never starts OpenCode\n"
        ), "{usage}");
    }

    #[test]
    fn unix_only_commands_are_refused_elsewhere_instead_of_launched() {
        for command in ["notify", "clip", "paste"] {
            // Unix dispatches them as before.
            assert!(unix_only(command, true).is_ok(), "{command}");
            // Windows refuses each with exit code 2 before the launch arm.
            let error = unix_only(command, false).unwrap_err();
            assert_eq!(error.code, 2);
            assert_eq!(
                error.message,
                format!("{command} is not supported on Windows")
            );
        }
        // Any other word stays a command or a program to launch.
        for command in ["claude", "sh", "ls", "clipboard", "pastes", "notify-send"] {
            assert!(unix_only(command, false).is_ok(), "{command}");
            assert!(unix_only(command, true).is_ok(), "{command}");
        }
    }

    #[test]
    fn an_existing_attach_names_tmux_only_where_it_hosts_the_sessions() {
        // A session ID in another form than `$N` is refused before attaching.
        use state::backend::PaneInfo;
        struct Answers;
        impl TerminalBackend for Answers {
            fn probe(&self, _target: Target, _fields: &[PaneField]) -> Option<PaneInfo> {
                Some(PaneInfo::new("@1\trun-1\n".into()))
            }
        }
        let config = Config {
            home: String::new(),
            dir: PathBuf::new(),
            state: PathBuf::new(),
            selves: Vec::new(),
            peers: Vec::new(),
            base_peers: Vec::new(),
            peers_from_environment: false,
            machines: Default::default(),
            tab: false,
            tab_colors: String::new(),
        };
        let args = ["claude/MyProj/work-1", "--existing"].map(String::from);
        let error =
            state::backend::with_fake(&Answers, || attach_command(&config, &args, "local", false))
                .unwrap_err();
        assert_eq!(error.code, 1);
        assert_eq!(error.message, state::backend::INVALID_SESSION_ID.text());
    }
}
