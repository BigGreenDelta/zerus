//! Unix (Linux and macOS) process, terminal, clipboard and file primitives.
use super::{available, capture, silent};
use crate::{
    cli::{Error, Result},
    config::Config,
};
use std::{
    env,
    ffi::OsString,
    fs,
    io::{self, Write},
    os::unix::{
        fs::{FileTypeExt, MetadataExt, OpenOptionsExt, PermissionsExt},
        process::{CommandExt, ExitStatusExt},
    },
    path::{Path, PathBuf},
    process::{Child, Command, ExitStatus, Output, Stdio},
    sync::atomic::{AtomicI32, Ordering},
    time::Instant,
};

pub fn prepare_environment() {
    let home = env::var("HOME").unwrap_or_default();
    env::set_var(
        "PATH",
        format!(
            "{home}/.local/bin:/opt/homebrew/bin:/usr/local/bin:{}:{home}/.npm-global/bin:{home}/.kimi-code/bin",
            env::var("PATH").unwrap_or_default()
        ),
    );
    let locale = ["LC_ALL", "LC_CTYPE", "LANG"]
        .into_iter()
        .filter_map(|key| env::var(key).ok())
        .find(|v| !v.is_empty())
        .unwrap_or_default()
        .to_ascii_uppercase();
    if !locale.contains("UTF-8") && !locale.contains("UTF8") {
        let locale = env::var("HGS_LANG").unwrap_or_else(|_| "en_US.UTF-8".into());
        env::set_var("LANG", &locale);
        env::set_var("LC_CTYPE", locale);
        env::remove_var("LC_ALL");
    }
}
pub fn code(status: ExitStatus) -> i32 {
    status
        .code()
        .unwrap_or_else(|| 128 + status.signal().unwrap_or(1))
}
pub fn executable(path: impl AsRef<Path>) -> bool {
    fs::metadata(path)
        .map(|m| m.is_file() && m.permissions().mode() & 0o111 != 0)
        .unwrap_or(false)
}
pub(super) fn is_path(program: &str) -> bool {
    program.contains('/')
}
pub(super) fn candidates(dir: &Path, program: &str) -> Vec<PathBuf> {
    vec![dir.join(program)]
}
pub fn is_macos() -> bool {
    capture("uname", &[]).is_some_and(|name| name.trim() == "Darwin")
}
pub(super) fn default_ssh() -> OsString {
    "ssh".into()
}
pub fn spawn_ssh(
    command: &mut Command,
    stdin: bool,
    stdout: bool,
    stderr: bool,
) -> io::Result<super::SshChild> {
    for (piped, set) in [
        (stdin, Command::stdin as fn(&mut Command, Stdio) -> &mut Command),
        (stdout, Command::stdout),
        (stderr, Command::stderr),
    ] {
        if piped {
            set(command, Stdio::piped());
        }
    }
    let mut child = command.spawn()?;
    Ok(super::SshChild {
        stdin: child.stdin.take().map(|pipe| Box::new(pipe) as _),
        stdout: child.stdout.take().map(|pipe| Box::new(pipe) as _),
        stderr: child.stderr.take().map(|pipe| Box::new(pipe) as _),
        child,
    })
}
pub fn ssh_output(command: &mut Command) -> io::Result<Output> {
    command.output()
}
pub fn ssh_status(command: &mut Command, input: fs::File) -> io::Result<ExitStatus> {
    command.stdin(input).status()
}
pub fn home() -> Option<String> {
    env::var("HOME").ok()
}
/// `relative`, written with '/', below `base`.
pub fn below(base: &str, relative: &str) -> String {
    format!("{base}/{relative}")
}
/// This machine's short host name, the default for HGS_SELF.
pub fn hostname() -> String {
    capture("hostname", &["-s"])
        .unwrap_or_default()
        .trim()
        .to_owned()
}
/// Owner-only access for a file holding private configuration.
pub fn make_private(file: &fs::File) -> io::Result<()> {
    file.set_permissions(fs::Permissions::from_mode(0o600))
}
/// Owner-only access for a directory holding private configuration.
pub fn restrict_dir(path: &Path) -> io::Result<()> {
    fs::set_permissions(path, fs::Permissions::from_mode(0o700))
}
/// Create files owner-only.
pub fn private_open(options: &mut fs::OpenOptions) -> &mut fs::OpenOptions {
    options.mode(0o600)
}
/// Refuse a symbolic link as the final component and never block on a FIFO.
pub fn no_follow(options: &mut fs::OpenOptions) -> &mut fs::OpenOptions {
    options.custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK)
}
/// Make a completed rename in `dir` durable.
pub fn sync_dir(dir: &Path) -> io::Result<()> {
    fs::File::open(dir)?.sync_all()
}
pub(super) fn transient_replace_error(_: &io::Error) -> bool {
    false
}
/// Run `command` in a process group of its own: the terminal's job control does
/// not reach it and `kill_process_group` stops everything it started.
pub fn own_process_group(command: &mut Command) -> &mut Command {
    command.process_group(0)
}
/// Stop a child started with `own_process_group` and every process in its group.
/// Never signals the caller's own process group.
pub fn kill_process_group(child: &mut Child) {
    unsafe {
        libc::kill(-(child.id() as i32), libc::SIGKILL);
    }
}
pub fn process_alive(pid: u32) -> bool {
    let Ok(pid) = libc::pid_t::try_from(pid) else {
        return false;
    };
    pid > 0
        && (unsafe { libc::kill(pid, 0) } == 0
            || io::Error::last_os_error().raw_os_error() == Some(libc::EPERM))
}
/// Distinguishes a file from a different one later found at the same path.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FileIdentity {
    pub device: u64,
    pub inode: u64,
    pub modified: i64,
    pub modified_nanos: i64,
}
pub fn file_identity(path: &Path) -> Option<FileIdentity> {
    let metadata = fs::metadata(path).ok()?;
    Some(FileIdentity {
        device: metadata.dev(),
        inode: metadata.ino(),
        modified: metadata.mtime(),
        modified_nanos: metadata.mtime_nsec(),
    })
}
/// The temporary directory of an interactive link's marker file.
pub fn link_marker_dir() -> io::Result<tempfile::TempDir> {
    tempfile::tempdir()
}
/// An OpenSSH LocalCommand for `ssh` that creates `path`; ssh runs it with the user's shell.
pub fn create_file_command(_: &mut Command, path: &Path) -> String {
    format!("touch {}", super::quote(&path.to_string_lossy()))
}
/// Escape sequences reach Unix terminals unchanged.
pub(super) fn virtual_terminal() -> bool {
    true
}
/// When `pid` started, as `ps -o lstart=` prints it with `LC_ALL=C TZ=UTC`, for
/// example "Sat Oct 10 08:27:57 2026". Stored run identities compare this text,
/// so older records written through ps keep matching. Read from the kernel:
/// session listings check several identities per poll, and each ps launch cost
/// more than the rest of the listing. Empty when the process does not exist;
/// None when its start time cannot be read here, so callers can ask ps instead.
pub fn process_start_time(pid: u32) -> Option<String> {
    if pid == 0 {
        return Some(String::new());
    }
    process_started_at(pid).map(|started| started.map(lstart_text).unwrap_or_default())
}

#[cfg(target_os = "macos")]
fn process_started_at(pid: u32) -> Option<Option<u64>> {
    // The kinfo_proc that ps reads, which also covers other users' processes.
    // Its extern_proc begins with p_starttime, a timeval whose seconds come first.
    let mut mib = [libc::CTL_KERN, libc::KERN_PROC, libc::KERN_PROC_PID, pid as libc::c_int];
    let mut info = [0u64; 128]; // kinfo_proc is 648 bytes
    let mut size = std::mem::size_of_val(&info);
    let rc = unsafe {
        libc::sysctl(mib.as_mut_ptr(), 4, info.as_mut_ptr().cast(), &mut size, std::ptr::null_mut(), 0)
    };
    match (rc, size) {
        (0, 0) => Some(None), // no such process
        (0, size) if size >= 8 => Some(Some(info[0])),
        _ => None,
    }
}

#[cfg(target_os = "linux")]
fn process_started_at(pid: u32) -> Option<Option<u64>> {
    use std::sync::OnceLock;
    // procps: boot time from /proc/stat plus the start in clock ticks, truncated.
    static BOOT: OnceLock<Option<u64>> = OnceLock::new();
    let stat = match fs::read_to_string(format!("/proc/{pid}/stat")) {
        Ok(stat) => stat,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Some(None),
        Err(_) => return None,
    };
    // The command name may contain spaces and parentheses; fields follow its last ')'.
    let ticks: u64 = stat.rsplit_once(')')?.1.split_whitespace().nth(19)?.parse().ok()?;
    let boot = (*BOOT.get_or_init(|| {
        fs::read_to_string("/proc/stat")
            .ok()?
            .lines()
            .find_map(|line| line.strip_prefix("btime ")?.trim().parse().ok())
    }))?;
    let hertz = u64::try_from(unsafe { libc::sysconf(libc::_SC_CLK_TCK) }).ok().filter(|&hz| hz > 0)?;
    Some(Some(boot + ticks / hertz))
}

#[cfg(not(any(target_os = "macos", target_os = "linux")))]
fn process_started_at(_pid: u32) -> Option<Option<u64>> {
    None
}

/// `%a %b %e %H:%M:%S %Y` in the C locale and UTC, the form ps and ctime print.
fn lstart_text(seconds: u64) -> String {
    const DAYS: [&str; 7] = ["Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"];
    const MONTHS: [&str; 12] = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
    let (days, time) = (seconds / 86_400, seconds % 86_400);
    // Civil date from days since 1970-01-01 (Howard Hinnant's algorithm).
    let shifted = days as i64 + 719_468;
    let era = shifted.div_euclid(146_097);
    let day_of_era = shifted - era * 146_097;
    let year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36_524 - day_of_era / 146_096) / 365;
    let day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    let month_index = (5 * day_of_year + 2) / 153;
    let day = day_of_year - (153 * month_index + 2) / 5 + 1;
    let month = if month_index < 10 { month_index + 3 } else { month_index - 9 };
    let year = year_of_era + era * 400 + i64::from(month <= 2);
    format!(
        "{} {} {:>2} {:02}:{:02}:{:02} {}",
        DAYS[(days % 7) as usize],
        MONTHS[(month - 1) as usize],
        day,
        time / 3600,
        time % 3600 / 60,
        time % 60,
        year
    )
}

fn runtime_directory() -> PathBuf {
    env::var_os("XDG_RUNTIME_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(format!("/run/user/{}", unsafe { libc::getuid() })))
}
pub fn wayland_env() -> Option<String> {
    if let Ok(value) = env::var("WAYLAND_DISPLAY") {
        if !value.is_empty() {
            return Some(value);
        }
    }
    let mut entries: Vec<_> = fs::read_dir(runtime_directory())
        .ok()?
        .filter_map(|entry| entry.ok())
        .collect();
    entries.sort_by_key(|entry| entry.file_name());
    for entry in entries {
        let name = entry.file_name().to_string_lossy().into_owned();
        if name.starts_with("wayland-")
            && !name.ends_with(".lock")
            && entry.file_type().is_ok_and(|t| t.is_socket())
        {
            env::set_var("WAYLAND_DISPLAY", &name);
            return Some(name);
        }
    }
    None
}
pub fn gui_env() -> Vec<String> {
    if is_macos() {
        return Vec::new();
    }
    let mut result = Vec::new();
    let mut system_env: Option<String> = None;
    for key in ["WAYLAND_DISPLAY", "DISPLAY", "XAUTHORITY"] {
        if env::var(key).is_ok_and(|v| !v.is_empty()) {
            continue;
        }
        let sysenv = system_env.get_or_insert_with(|| {
            capture("systemctl", &["--user", "show-environment"]).unwrap_or_default()
        });
        let mut value = sysenv
            .lines()
            .find_map(|line| line.strip_prefix(&format!("{key}=")))
            .unwrap_or("")
            .to_owned();
        if value.starts_with('"') && value.ends_with('"') && value.len() >= 2 {
            value = value[1..value.len() - 1].into();
        }
        if value.is_empty() && key == "WAYLAND_DISPLAY" {
            value = wayland_env().unwrap_or_default();
        }
        if value.is_empty() || (key == "XAUTHORITY" && fs::File::open(&value).is_err()) {
            continue;
        }
        result.push(format!("{key}={value}"));
    }
    result
}

/// Title output goes to the attached client terminals, never to an agent's pane
/// input. SSH relays the same OSC sequence to clients on another machine.
pub fn refresh_session_title(session: &str) -> Result<()> {
    if session.chars().any(char::is_control) {
        return Err(Error::new(1, "terminal title contains control characters"));
    }
    let target = format!("={session}");
    if !silent("tmux", &["has-session", "-t", &target]) {
        return Ok(()); // Saved sessions have no attached terminals.
    }
    let option_target = format!("={session}:");
    for (key, value) in [("set-titles-string", "#S"), ("set-titles", "on")] {
        if !silent("tmux", &["set-option", "-t", &option_target, key, value]) {
            return Err(Error::new(1, format!("cannot update tmux {key}")));
        }
    }
    let terminals = capture(
        "tmux",
        &["list-clients", "-t", &target, "-F", "#{client_tty}"],
    )
    .ok_or_else(|| Error::new(1, "cannot list attached terminals"))?;
    // Konsole's previous hgs attachment may use a literal tab format. %w makes
    // it follow the window title; OSC 0 is also understood by macOS terminals.
    let title = format!("\x1b]30;%w\x07\x1b]0;{session}\x07");
    let mut seen = std::collections::BTreeSet::new();
    for path in terminals.lines().filter(|path| !path.is_empty()) {
        if !seen.insert(path) {
            continue;
        }
        if !path.starts_with("/dev/") || path.chars().any(char::is_control) {
            return Err(Error::new(1, "tmux returned an invalid client terminal"));
        }
        let mut terminal = fs::OpenOptions::new()
            .write(true)
            .custom_flags(libc::O_NOCTTY | libc::O_NONBLOCK | libc::O_NOFOLLOW)
            .open(path)?;
        let metadata = terminal.metadata()?;
        if !metadata.file_type().is_char_device() || metadata.uid() != unsafe { libc::getuid() } {
            return Err(Error::new(1, "client terminal is not owned by this user"));
        }
        terminal.write_all(title.as_bytes())?;
    }
    Ok(())
}

static CHILD_PID: AtomicI32 = AtomicI32::new(0);
static INTERRUPTED: AtomicI32 = AtomicI32::new(0);
extern "C" fn forward_signal(signal: libc::c_int) {
    INTERRUPTED.store(signal, Ordering::Relaxed);
    let pid = CHILD_PID.load(Ordering::Relaxed);
    if pid > 0 {
        unsafe {
            libc::kill(pid, signal);
        }
    }
}
/// Keep terminal restoration in normal Rust control flow even on SIGINT/TERM/HUP.
pub fn wait_interactive(command: &mut Command) -> Result<i32> {
    struct Handlers(Vec<(i32, libc::sighandler_t)>);
    impl Drop for Handlers {
        fn drop(&mut self) {
            CHILD_PID.store(0, Ordering::Relaxed);
            for &(sig, old) in &self.0 {
                unsafe {
                    libc::signal(sig, old);
                }
            }
        }
    }
    INTERRUPTED.store(0, Ordering::Relaxed);
    let _handlers = Handlers(
        [libc::SIGINT, libc::SIGTERM, libc::SIGHUP]
            .into_iter()
            .map(|sig| {
                (sig, unsafe {
                    libc::signal(sig, forward_signal as *const () as libc::sighandler_t)
                })
            })
            .collect(),
    );
    let mut child = command.spawn().map_err(|e| {
        Error::new(
            127,
            format!("{}: {e}", command.get_program().to_string_lossy()),
        )
    })?;
    CHILD_PID.store(child.id() as i32, Ordering::Relaxed);
    let signal = INTERRUPTED.load(Ordering::Relaxed);
    if signal != 0 {
        unsafe {
            libc::kill(child.id() as i32, signal);
        }
    }
    let rc = code(child.wait()?);
    let signal = INTERRUPTED.load(Ordering::Relaxed);
    Ok(if signal == 0 { rc } else { 128 + signal })
}

fn applescript_string(text: &str) -> String {
    text.replace('\\', "\\\\").replace('"', "\\\"")
}
pub fn clip() -> Result<i32> {
    if is_macos() {
        if !available("osascript") {
            eprintln!("hgs: clip: no osascript");
            return Ok(2);
        }
        if capture("osascript", &["-e", "clipboard info for «class PNGf»"])
            .unwrap_or_default()
            .trim()
            .is_empty()
        {
            return Ok(1);
        }
        let temporary = tempfile::NamedTempFile::new()?;
        let path = applescript_string(&temporary.path().to_string_lossy());
        if silent(
            "osascript",
            &[
                "-e",
                &format!("set f to open for access POSIX file \"{path}\" with write permission"),
                "-e",
                "set eof f to 0",
                "-e",
                "write (the clipboard as «class PNGf») to f",
                "-e",
                "close access f",
            ],
        ) {
            io::copy(&mut fs::File::open(temporary.path())?, &mut io::stdout())?;
            return Ok(0);
        }
        eprintln!("hgs: clip: osascript could not read the clipboard as PNG");
        return Ok(2);
    }
    if !available("wl-paste") {
        eprintln!("hgs: clip: no wl-paste (wl-clipboard)");
        return Ok(2);
    }
    if wayland_env().is_none() {
        eprintln!(
            "hgs: clip: no Wayland socket in {}",
            runtime_directory().display()
        );
        return Ok(2);
    }
    if !capture("wl-paste", &["-l"]).is_some_and(|types| types.lines().any(|t| t == "image/png")) {
        return Ok(1);
    }
    Ok(code(
        Command::new("wl-paste")
            .args(["-t", "image/png"])
            .status()?,
    ))
}
fn clip_set(path: &Path) -> bool {
    if is_macos() {
        silent(
            "osascript",
            &[
                "-e",
                &format!(
                    "set the clipboard to (read (POSIX file \"{}\") as «class PNGf»)",
                    applescript_string(&path.to_string_lossy())
                ),
            ],
        )
    } else {
        if !available("wl-copy") || wayland_env().is_none() {
            return false;
        }
        let Ok(file) = fs::File::open(path) else {
            return false;
        };
        Command::new("wl-copy")
            .args(["-t", "image/png"])
            .stdin(file)
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status()
            .is_ok_and(|s| s.success())
    }
}
fn paste_log(config: &Config, message: &str) {
    let path = PathBuf::from(
        env::var("XDG_STATE_HOME").unwrap_or_else(|_| format!("{}/.local/state", config.home)),
    )
    .join("hgs");
    if fs::create_dir_all(&path).is_err() {
        return;
    }
    if let Ok(mut file) = fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(path.join("paste.log"))
    {
        let stamp = capture("date", &["+%Y-%m-%d %H:%M:%S"]).unwrap_or_default();
        let _ = writeln!(file, "{} {message}", stamp.trim());
    }
}
pub fn paste(config: &Config, session: &str, pane: &str, client: &str) -> Result<i32> {
    let message = |text: String| {
        if !client.is_empty() {
            silent(
                "tmux",
                &[
                    "display-message",
                    "-c",
                    client,
                    "-d",
                    "4000",
                    &format!("hgs: {text}"),
                ],
            );
        }
    };
    let environment = capture(
        "tmux",
        &[
            "show-environment",
            "-t",
            &format!("={session}"),
            "HGS_CLIENT",
        ],
    )
    .unwrap_or_default();
    let who = environment
        .trim_end_matches('\n')
        .strip_prefix("HGS_CLIENT=")
        .unwrap_or(environment.trim_end_matches('\n'));
    if matches!(who, "" | "-HGS_CLIENT" | "local") {
        paste_log(
            config,
            &format!(
                "sess={session} who={} local-passthrough",
                if who.is_empty() { "unset" } else { who }
            ),
        );
    } else {
        message(format!("pulling clipboard from {who}"));
        match tempfile::NamedTempFile::new() {
            Ok(temporary) => {
                let start = Instant::now();
                let rc = temporary
                    .reopen()
                    .ok()
                    .and_then(|file| {
                        super::ssh()
                            .args(crate::machines::ssh_options(config, who))
                            .args([
                                "-o",
                                "BatchMode=yes",
                                "-o",
                                "ConnectTimeout=3",
                                who,
                                "~/.local/bin/hgs clip",
                            ])
                            .stdout(file)
                            .stderr(Stdio::null())
                            .status()
                            .ok()
                    })
                    .map(code)
                    .unwrap_or(255);
                let prefix = format!(
                    "sess={session} who={who} rc={rc} ssh={}s",
                    start.elapsed().as_secs()
                );
                match rc {
                    0 if clip_set(temporary.path()) => paste_log(
                        config,
                        &format!(
                            "{prefix} bytes={} set=ok",
                            temporary.as_file().metadata().map(|m| m.len()).unwrap_or(0)
                        ),
                    ),
                    0 => {
                        message("cannot set clipboard on this box".into());
                        paste_log(config, &format!("{prefix} set=FAIL"));
                    }
                    1 => {
                        message(format!("no image in {who} clipboard"));
                        paste_log(config, &format!("{prefix} no-image-on-{who}"));
                    }
                    2 => {
                        message(format!("{who} has no clipboard tool (hgs clip rc 2)"));
                        paste_log(config, &format!("{prefix} no-tool-on-{who}"));
                    }
                    _ => {
                        message(format!("{who} unreachable, pasting local clipboard"));
                        paste_log(config, &format!("{prefix} ssh-failed"));
                    }
                }
            }
            Err(_) => paste_log(config, &format!("sess={session} who={who} mktemp-failed")),
        }
    }
    // A failed clipboard transfer must never swallow the agent's paste key.
    let _ = Command::new("tmux")
        .args(["send-keys", "-t", pane, "C-v"])
        .status();
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ps_start(pid: u32) -> Option<String> {
        let output = Command::new("ps")
            .args(["-p", &pid.to_string(), "-o", "lstart="])
            .env("LC_ALL", "C")
            .env("TZ", "UTC")
            .output()
            .ok()?;
        Some(String::from_utf8_lossy(&output.stdout).trim().to_owned())
    }

    #[test]
    fn start_time_text_matches_ps_lstart_form() {
        // C locale, UTC, the day padded with a space: stored identities compare as text.
        assert_eq!(lstart_text(0), "Thu Jan  1 00:00:00 1970");
        assert_eq!(lstart_text(1_791_620_877), "Sat Oct 10 08:27:57 2026");
        assert_eq!(lstart_text(1_759_309_200), "Wed Oct  1 09:00:00 2025");
        assert_eq!(lstart_text(951_782_400), "Tue Feb 29 00:00:00 2000");
    }

    #[test]
    fn process_start_time_matches_ps() {
        let parent = unsafe { libc::getppid() } as u32;
        for pid in [std::process::id(), parent, 1] {
            let Some(expected) = ps_start(pid) else { return };
            let actual = process_start_time(pid).expect("readable start time");
            // A process that cannot be read must stay unknown rather than look absent.
            if expected.is_empty() {
                continue;
            }
            assert_eq!(actual, expected, "pid {pid}");
        }
    }

    #[test]
    fn missing_process_has_no_start_time() {
        assert_eq!(process_start_time(0), Some(String::new()));
        // pid_max is far below this on Linux and macOS.
        assert_eq!(process_start_time(99_999_999), Some(String::new()));
    }

    #[test]
    #[ignore = "compares every visible process with ps; run manually on each platform"]
    fn every_process_start_time_matches_ps() {
        let output = Command::new("ps")
            .args(["-axo", "pid=,lstart="])
            .env("LC_ALL", "C")
            .env("TZ", "UTC")
            .output()
            .unwrap();
        let (mut checked, mut differ) = (0, Vec::new());
        for line in String::from_utf8_lossy(&output.stdout).lines() {
            let line = line.trim_start();
            let Some((pid, expected)) = line.split_once(' ') else { continue };
            let pid: u32 = pid.parse().unwrap();
            match process_start_time(pid) {
                Some(actual) if actual == expected.trim() => checked += 1,
                Some(actual) if actual.is_empty() => {} // exited since ps listed it
                other => differ.push(format!("{pid}: {other:?} != {expected:?}")),
            }
        }
        assert!(differ.is_empty(), "{checked} matched; differ: {differ:#?}");
        assert!(checked > 10);
    }
}
