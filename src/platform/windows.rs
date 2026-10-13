//! Windows process, console and file primitives. A Windows build manages remote
//! machines through the system OpenSSH client; no agent session runs here.
use super::capture;
use crate::cli::{Error, Result};
use std::{
    env,
    ffi::OsString,
    fs,
    io::{self, Read},
    os::windows::{
        fs::OpenOptionsExt,
        io::{AsRawHandle, FromRawHandle, RawHandle},
        process::CommandExt,
    },
    path::{Path, PathBuf},
    process::{Child, Command, ExitStatus, Output, Stdio},
    sync::{
        atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering},
        Arc, Mutex,
    },
    thread,
    time::{Duration, Instant},
};
use windows_sys::Win32::{
    Foundation::{
        CloseHandle, GetLastError, ERROR_ACCESS_DENIED, ERROR_LOCK_VIOLATION,
        ERROR_SHARING_VIOLATION, GENERIC_READ, GENERIC_WRITE, HANDLE, INVALID_HANDLE_VALUE,
        WAIT_TIMEOUT,
    },
    Storage::FileSystem::{
        CreateFileW, GetFileInformationByHandle, GetFileType, BY_HANDLE_FILE_INFORMATION,
        FILE_FLAG_FIRST_PIPE_INSTANCE, FILE_FLAG_OPEN_REPARSE_POINT, FILE_FLAG_OVERLAPPED,
        FILE_READ_ATTRIBUTES, FILE_TYPE, FILE_TYPE_CHAR, FILE_TYPE_DISK, FILE_TYPE_PIPE,
        FILE_WRITE_ATTRIBUTES, OPEN_EXISTING, PIPE_ACCESS_INBOUND, PIPE_ACCESS_OUTBOUND,
    },
    System::{
        Console::{
            GetConsoleMode, GetStdHandle, SetConsoleCtrlHandler, SetConsoleMode, CTRL_BREAK_EVENT,
            CTRL_CLOSE_EVENT, CTRL_C_EVENT, CTRL_LOGOFF_EVENT, CTRL_SHUTDOWN_EVENT,
            ENABLE_VIRTUAL_TERMINAL_PROCESSING, STD_OUTPUT_HANDLE,
        },
        JobObjects::{
            AssignProcessToJobObject, CreateJobObjectW, JobObjectExtendedLimitInformation,
            SetInformationJobObject, JOBOBJECT_EXTENDED_LIMIT_INFORMATION,
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
        },
        Pipes::{
            CreateNamedPipeW, PIPE_READMODE_BYTE, PIPE_REJECT_REMOTE_CLIENTS, PIPE_TYPE_BYTE,
            PIPE_WAIT,
        },
        Threading::{
            GetCurrentProcess, OpenProcess, WaitForSingleObject, CREATE_NEW_PROCESS_GROUP,
            PROCESS_SYNCHRONIZE,
        },
    },
};

/// PATH, the locale and the console code page stay as they are: Rust writes
/// Unicode to the console itself, and a changed code page would outlive hgs in
/// the user's shell. Console children (ssh.exe, tar.exe) must not outlive hgs
/// when the GUI stops it, so this process joins a job that terminates its
/// remaining members when its last handle, held only here, closes.
pub fn prepare_environment() {
    unsafe {
        let job = CreateJobObjectW(std::ptr::null(), std::ptr::null());
        if job.is_null() {
            return;
        }
        let mut limits: JOBOBJECT_EXTENDED_LIMIT_INFORMATION = std::mem::zeroed();
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        // Best effort: an enclosing job that forbids nesting keeps the old behavior.
        if SetInformationJobObject(
            job,
            JobObjectExtendedLimitInformation,
            (&limits as *const JOBOBJECT_EXTENDED_LIMIT_INFORMATION).cast(),
            std::mem::size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
        ) == 0
            || AssignProcessToJobObject(job, GetCurrentProcess()) == 0
        {
            CloseHandle(job);
        }
    }
}
/// Windows reports every exit as a code; there are no signals.
pub fn code(status: ExitStatus) -> i32 {
    status.code().unwrap_or(1)
}
fn extensions() -> Vec<String> {
    env::var("PATHEXT")
        .ok()
        .filter(|value| !value.trim().is_empty())
        .unwrap_or_else(|| ".COM;.EXE;.BAT;.CMD".into())
        .split(';')
        .map(|extension| extension.trim().to_ascii_lowercase())
        .filter(|extension| extension.len() > 1 && extension.starts_with('.'))
        .collect()
}
fn runnable_name(path: &Path, extensions: &[String]) -> bool {
    path.extension().is_some_and(|extension| {
        let extension = format!(".{}", extension.to_string_lossy().to_ascii_lowercase());
        extensions.contains(&extension)
    })
}
/// A file whose extension PATHEXT lists, as cmd.exe would run it.
pub fn executable(path: impl AsRef<Path>) -> bool {
    let path = path.as_ref();
    fs::metadata(path).is_ok_and(|m| m.is_file()) && runnable_name(path, &extensions())
}
pub(super) fn is_path(program: &str) -> bool {
    program.contains(['/', '\\', ':'])
}
pub(super) fn candidates(dir: &Path, program: &str) -> Vec<PathBuf> {
    let extensions = extensions();
    if runnable_name(Path::new(program), &extensions) {
        return vec![dir.join(program)];
    }
    extensions
        .iter()
        .map(|extension| dir.join(format!("{program}{extension}")))
        .collect()
}
/// The Windows OpenSSH client from System32 first: a Git or MSYS ssh earlier on
/// PATH uses another configuration, key agent and home directory.
pub(super) fn default_ssh() -> OsString {
    env::var_os("SystemRoot")
        .map(|root| PathBuf::from(root).join(r"System32\OpenSSH\ssh.exe"))
        .filter(|path| path.is_file())
        .or_else(|| super::which("ssh"))
        .map_or_else(|| "ssh".into(), PathBuf::into_os_string)
}

// Windows OpenSSH never exits once it has used a synchronous pipe for one of its
// standard streams, and synchronous pipes are what std, Python, PowerShell and
// Qt give a child. With overlapped pipe ends (as libuv gives them) it behaves.
// Every pipe between hgs and ssh.exe is therefore an overlapped named pipe, and
// so is every relayed stream of this process (see `relayed`). Only `ssh -G`
// (no input, fresh output files) and NUL as output get other handles.
fn overlapped_pipe(child_reads: bool) -> io::Result<(fs::File, fs::File)> {
    static NEXT: AtomicU32 = AtomicU32::new(0);
    let unique = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |time| time.subsec_nanos());
    let name: Vec<u16> = format!(
        r"\\.\pipe\hgs-{}-{}-{unique}",
        std::process::id(),
        NEXT.fetch_add(1, Ordering::Relaxed)
    )
    .encode_utf16()
    .chain(Some(0))
    .collect();
    let (direction, access) = if child_reads {
        (PIPE_ACCESS_OUTBOUND, GENERIC_READ | FILE_WRITE_ATTRIBUTES)
    } else {
        (PIPE_ACCESS_INBOUND, GENERIC_WRITE | FILE_READ_ATTRIBUTES)
    };
    unsafe {
        let ours = CreateNamedPipeW(
            name.as_ptr(),
            direction | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1,
            65536,
            65536,
            0,
            std::ptr::null(),
        );
        if ours == INVALID_HANDLE_VALUE {
            return Err(io::Error::last_os_error());
        }
        let ours = fs::File::from_raw_handle(ours);
        let theirs = CreateFileW(
            name.as_ptr(),
            access,
            0,
            std::ptr::null(),
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            std::ptr::null_mut(),
        );
        if theirs == INVALID_HANDLE_VALUE {
            return Err(io::Error::last_os_error());
        }
        Ok((ours, fs::File::from_raw_handle(theirs)))
    }
}
type Setter = fn(&mut Command, Stdio) -> &mut Command;
const STREAMS: [Setter; 3] = [Command::stdin, Command::stdout, Command::stderr];
/// Start an ssh client with the requested standard streams piped to this
/// process. Unpiped input is a pipe that ends at once, whatever the caller set:
/// the peer never sees the end of NUL (`Stdio::null()`) through Windows OpenSSH.
pub fn spawn_ssh(
    command: &mut Command,
    stdin: bool,
    stdout: bool,
    stderr: bool,
) -> io::Result<super::SshChild> {
    let mut ours: [Option<fs::File>; 3] = Default::default();
    for (index, piped) in [stdin, stdout, stderr].into_iter().enumerate() {
        if piped || index == 0 {
            let (mine, theirs) = overlapped_pipe(index == 0)?;
            STREAMS[index](command, theirs.into());
            ours[index] = Some(mine);
        }
    }
    let spawned = command.spawn();
    // Keep no copy of the child's ends, so reads end when the client exits.
    for (index, pipe) in ours.iter().enumerate() {
        if pipe.is_some() {
            STREAMS[index](command, Stdio::null());
        }
    }
    let [input, output, errors] = ours;
    Ok(super::SshChild {
        child: spawned?,
        // Unpiped input ends here, when its only writer closes.
        stdin: input.filter(|_| stdin).map(|pipe| Box::new(pipe) as _),
        stdout: output.map(|pipe| Box::new(pipe) as _),
        stderr: errors.map(|pipe| Box::new(pipe) as _),
    })
}
/// `Command::output` for an ssh client: stdin closed, stdout and stderr captured.
pub fn ssh_output(command: &mut Command) -> io::Result<Output> {
    let mut ssh = spawn_ssh(command, false, true, true)?;
    let read = |pipe: Option<Box<dyn Read + Send>>| {
        pipe.map(|mut pipe| {
            thread::spawn(move || {
                let mut bytes = Vec::new();
                pipe.read_to_end(&mut bytes).map(|_| bytes)
            })
        })
    };
    let (stdout, stderr) = (read(ssh.stdout.take()), read(ssh.stderr.take()));
    let status = ssh.child.wait()?;
    let collect = |reader: Option<thread::JoinHandle<io::Result<Vec<u8>>>>| {
        reader.map_or(Ok(Vec::new()), |reader| {
            reader
                .join()
                .map_err(|_| io::Error::other("ssh output reader failed"))?
        })
    };
    Ok(Output {
        status,
        stdout: collect(stdout)?,
        stderr: collect(stderr)?,
    })
}
/// `Command::status` for an ssh client that reads `input` and shares this
/// process's stdout and stderr. This process's stdin is not passed on.
pub fn ssh_status(command: &mut Command, input: fs::File) -> io::Result<ExitStatus> {
    let relay = Relay::prepare(command, Some(input))?;
    let spawned = command.spawn();
    let pumps = relay.start(command);
    let status = spawned.and_then(|mut child| child.wait());
    pumps.finish();
    status
}
/// Whether a child gets one of this process's standard streams through a relay
/// pipe rather than the handle itself. Windows OpenSSH hangs on a synchronous
/// pipe, never sends the end of a file or of a character device such as NUL,
/// and writes a file from its start, over what `>>` keeps. Only a console
/// (`ssh -t` and Ctrl+C need it) and an absent or invalid handle (unknown type)
/// are passed on as they are.
fn relayed(file_type: FILE_TYPE, console: bool) -> bool {
    !console && matches!(file_type, FILE_TYPE_DISK | FILE_TYPE_CHAR | FILE_TYPE_PIPE)
}
fn needs_relay(handle: RawHandle) -> bool {
    let mut mode = 0;
    unsafe {
        relayed(
            GetFileType(handle as HANDLE),
            GetConsoleMode(handle as HANDLE, &mut mode) != 0,
        )
    }
}
/// A child's standard streams relayed through overlapped pipes: this process's
/// own unless they are consoles (see `relayed`), and a file given to read
/// instead of this process's stdin. Output reaches a redirected file through
/// this process's handle, at its position. Input ends where its source ends.
struct Relay {
    pipes: [Option<fs::File>; 3],
    source: Option<fs::File>,
}
impl Relay {
    fn prepare(command: &mut Command, source: Option<fs::File>) -> io::Result<Self> {
        let handles = [
            io::stdin().as_raw_handle(),
            io::stdout().as_raw_handle(),
            io::stderr().as_raw_handle(),
        ];
        let mut pipes: [Option<fs::File>; 3] = Default::default();
        for (index, handle) in handles.into_iter().enumerate() {
            if (index == 0 && source.is_some()) || needs_relay(handle) {
                let (mine, theirs) = overlapped_pipe(index == 0)?;
                STREAMS[index](command, theirs.into());
                pipes[index] = Some(mine);
            }
        }
        Ok(Self { pipes, source })
    }
    /// Call after spawning: the child holds its ends, this process keeps none.
    fn start(self, command: &mut Command) -> Pumps {
        for (index, pipe) in self.pipes.iter().enumerate() {
            if pipe.is_some() {
                STREAMS[index](command, Stdio::inherit());
            }
        }
        let Self {
            pipes: [input, output, errors],
            source,
        } = self;
        // Never joined: this process's stdin may stay open after the child exits.
        // The pipe closes at the end of the input, which the child then reads.
        if let Some(mut pipe) = input {
            thread::spawn(move || {
                let _ = match source {
                    Some(mut file) => io::copy(&mut file, &mut pipe),
                    None => io::copy(&mut io::stdin().lock(), &mut pipe),
                };
            });
        }
        let mut pumps = Vec::new();
        if let Some(pipe) = output {
            pumps.push(Pump::start(pipe, io::stdout));
        }
        if let Some(pipe) = errors {
            pumps.push(Pump::start(pipe, io::stderr));
        }
        Pumps(pumps)
    }
}
/// What a relay thread is doing, for `Pumps::finish`.
#[derive(Default)]
struct Progress {
    /// Waiting in a read of the child's pipe (not writing to this process's stream).
    reading: AtomicBool,
    /// Completed reads; a change means data arrived or the pipe ended.
    reads: AtomicU64,
}
struct Pump {
    thread: thread::JoinHandle<()>,
    progress: Arc<Progress>,
}
impl Pump {
    fn start<W: io::Write + 'static>(from: fs::File, to: fn() -> W) -> Self {
        let progress = Arc::new(Progress::default());
        let shared = Arc::clone(&progress);
        let thread = thread::spawn(move || copy_flushed(from, to(), &shared));
        Self { thread, progress }
    }
}
fn copy_flushed(mut from: fs::File, mut to: impl io::Write, progress: &Progress) {
    let mut buffer = [0; 8192];
    loop {
        progress.reading.store(true, Ordering::SeqCst);
        let read = from.read(&mut buffer);
        progress.reads.fetch_add(1, Ordering::SeqCst);
        progress.reading.store(false, Ordering::SeqCst);
        let Ok(count @ 1..) = read else {
            break;
        };
        // Blocks while the reader of this process's output is busy, as the child
        // itself would block on a stream it had inherited.
        if to
            .write_all(&buffer[..count])
            .and_then(|()| to.flush())
            .is_err()
        {
            break;
        }
    }
}
/// How long a pump may wait for data after the child has exited.
const RELAY_IDLE: Duration = Duration::from_secs(5);
struct Pumps(Vec<Pump>);
impl Pumps {
    /// Relay all of the child's output after it exits, however late this
    /// process's reader takes it: data already read or still in the pipe is
    /// never dropped. Only a pump that has waited in a read with no data for
    /// `RELAY_IDLE` is abandoned; then a helper the child left running (a
    /// connection multiplexer, for example) holds the pipe open.
    fn finish(self) {
        let started = Instant::now();
        let mut pending: Vec<(Pump, u64, Instant)> = self
            .0
            .into_iter()
            .map(|pump| {
                let reads = pump.progress.reads.load(Ordering::SeqCst);
                (pump, reads, started)
            })
            .collect();
        while !pending.is_empty() {
            let now = Instant::now();
            pending.retain_mut(|(pump, reads, idle_since)| {
                if pump.thread.is_finished() {
                    return false;
                }
                let current = pump.progress.reads.load(Ordering::SeqCst);
                if current != *reads || !pump.progress.reading.load(Ordering::SeqCst) {
                    *reads = current;
                    *idle_since = now;
                }
                now.duration_since(*idle_since) < RELAY_IDLE
            });
            if !pending.is_empty() {
                thread::sleep(Duration::from_millis(10));
            }
        }
    }
}
/// HOME when Git Bash or the user sets it, otherwise the user profile. The
/// layout below it is the same as on Unix (`.config\hgs`, `.local\state\hgs`).
pub fn home() -> Option<String> {
    home_from(env::var("HOME").ok(), env::var("USERPROFILE").ok())
}
fn home_from(home: Option<String>, profile: Option<String>) -> Option<String> {
    home.filter(|value| !value.is_empty())
        .or(profile.filter(|value| !value.is_empty()))
}
/// `relative`, written with '/', below `base` with Windows separators.
pub fn below(base: &str, relative: &str) -> String {
    relative
        .split(['/', '\\'])
        .filter(|part| !part.is_empty())
        .fold(PathBuf::from(base), |path, part| path.join(part))
        .to_string_lossy()
        .into_owned()
}
/// The lowercased computer name, the default for HGS_SELF.
pub fn hostname() -> String {
    env::var("COMPUTERNAME")
        .ok()
        .filter(|name| !name.trim().is_empty())
        .or_else(|| capture("hostname", &[]))
        .unwrap_or_default()
        .trim()
        .to_lowercase()
}
// Files below the user profile inherit its owner-only ACL. Windows has no mode
// bits to narrow, so the private-file helpers keep that inherited protection.
pub fn make_private(_: &fs::File) -> io::Result<()> {
    Ok(())
}
pub fn restrict_dir(_: &Path) -> io::Result<()> {
    Ok(())
}
pub fn private_open(options: &mut fs::OpenOptions) -> &mut fs::OpenOptions {
    options
}
/// Open a reparse point (a symbolic link) itself; it is then not a regular file.
pub fn no_follow(options: &mut fs::OpenOptions) -> &mut fs::OpenOptions {
    options.custom_flags(FILE_FLAG_OPEN_REPARSE_POINT)
}
/// NTFS journals the rename itself, and a directory cannot be flushed through a
/// handle that std can open.
pub fn sync_dir(_: &Path) -> io::Result<()> {
    Ok(())
}
/// A scanner, the indexer or a reader without delete sharing holds the file.
pub(super) fn transient_replace_error(error: &io::Error) -> bool {
    error.raw_os_error().is_some_and(|code| {
        [
            ERROR_ACCESS_DENIED,
            ERROR_SHARING_VIOLATION,
            ERROR_LOCK_VIOLATION,
        ]
        .contains(&(code as u32))
    })
}
/// A console Ctrl+C for the caller does not reach the child.
pub fn own_process_group(command: &mut Command) -> &mut Command {
    command.creation_flags(CREATE_NEW_PROCESS_GROUP)
}
/// Stop a child. Its own children end with this process's job.
pub fn kill_process_group(child: &mut Child) {
    let _ = child.kill();
}
pub fn process_alive(pid: u32) -> bool {
    if pid == 0 {
        return false;
    }
    unsafe {
        let process = OpenProcess(PROCESS_SYNCHRONIZE, 0, pid);
        if process.is_null() {
            // Another user's process exists but cannot be opened.
            return GetLastError() == ERROR_ACCESS_DENIED;
        }
        let running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
        CloseHandle(process);
        running
    }
}
/// Distinguishes a file from a different one later found at the same path.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FileIdentity {
    pub device: u64,
    pub inode: u64,
    pub modified: i64,
    pub modified_nanos: i64,
}
/// Volume and file index identify the file itself; creation times do not,
/// because NTFS tunnelling gives a replacement under the same name the old one.
pub fn file_identity(path: &Path) -> Option<FileIdentity> {
    let file = fs::File::open(path).ok()?;
    let mut info: BY_HANDLE_FILE_INFORMATION = unsafe { std::mem::zeroed() };
    if unsafe { GetFileInformationByHandle(file.as_raw_handle() as HANDLE, &mut info) } == 0 {
        return None;
    }
    Some(FileIdentity {
        device: info.dwVolumeSerialNumber.into(),
        inode: (u64::from(info.nFileIndexHigh) << 32) | u64::from(info.nFileIndexLow),
        modified: (i64::from(info.ftLastWriteTime.dwHighDateTime) << 32)
            | i64::from(info.ftLastWriteTime.dwLowDateTime),
        modified_nanos: 0,
    })
}
/// Link markers live in `hgs-link-<pid>-<random>` folders of the temporary
/// directory. A client that is killed leaves its folder behind: the desktop
/// app's Disconnect ends the attach client with its job, and a closed
/// pseudoconsole ends its clients almost at once. The next interactive link
/// removes the folders whose process is gone.
const LINK_MARKER_PREFIX: &str = "hgs-link-";
/// This process's marker folder, for the console handler.
static LINK_MARKER: Mutex<Option<PathBuf>> = Mutex::new(None);
/// The temporary directory of an interactive link's marker file.
pub fn link_marker_dir() -> io::Result<tempfile::TempDir> {
    let parent = env::temp_dir();
    remove_stale_link_markers(&parent);
    let directory = tempfile::Builder::new()
        .prefix(&format!("{LINK_MARKER_PREFIX}{}-", std::process::id()))
        .tempdir_in(&parent)?;
    if let Ok(mut current) = LINK_MARKER.lock() {
        *current = Some(directory.path().to_owned());
    }
    Ok(directory)
}
fn remove_stale_link_markers(parent: &Path) {
    let Ok(entries) = fs::read_dir(parent) else {
        return;
    };
    for entry in entries.flatten() {
        let name = entry.file_name();
        let owner = name
            .to_str()
            .and_then(|name| name.strip_prefix(LINK_MARKER_PREFIX))
            .and_then(|rest| rest.split_once('-'))
            .and_then(|(pid, _)| pid.parse::<u32>().ok());
        if owner.is_some_and(|pid| !process_alive(pid)) {
            let _ = fs::remove_dir_all(entry.path());
        }
    }
}
fn remove_link_marker() {
    if let Ok(current) = LINK_MARKER.lock() {
        if let Some(path) = current.as_deref() {
            let _ = fs::remove_dir_all(path);
        }
    }
}
/// Carries a LocalCommand's file path from hgs through ssh.exe to cmd.exe.
const LOCAL_COMMAND_PATH: &str = "HGS_LINK_MARKER";
/// An OpenSSH LocalCommand for `ssh` that creates `path`. Windows OpenSSH expands
/// its `%` tokens (an unknown one is fatal) and runs the result with cmd.exe
/// (`_wsystem`), which expands `%NAME%` pairs, so a temporary folder below a
/// profile name containing `%` cannot be written into the command. cmd.exe reads
/// the path from the client's environment instead: neither layer expands the
/// value again, and the quotes keep characters such as `&` and `^` literal.
pub fn create_file_command(ssh: &mut Command, path: &Path) -> String {
    ssh.env(LOCAL_COMMAND_PATH, path);
    format!("type nul > \"%%{LOCAL_COMMAND_PATH}%%\"")
}
/// Escape sequences need virtual terminal processing; enable it when the
/// console allows. A legacy console without it gets no sequences at all.
pub(super) fn virtual_terminal() -> bool {
    unsafe {
        let output = GetStdHandle(STD_OUTPUT_HANDLE);
        let mut mode = 0;
        if output.is_null()
            || output == INVALID_HANDLE_VALUE
            || GetConsoleMode(output, &mut mode) == 0
        {
            return false;
        }
        mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING != 0
            || SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0
    }
}
pub fn gui_env() -> Vec<String> {
    Vec::new()
}

static INTERRUPTED: AtomicBool = AtomicBool::new(false);
/// The console delivers Ctrl+C and Ctrl+Break to every attached process. The
/// child (ssh.exe) handles them; hgs only waits for its exit. When the console
/// itself goes away (its window closes, the user logs off), this process ends
/// once the handler returns and no destructor runs, so the link marker is
/// removed here.
unsafe extern "system" fn interrupted(event: u32) -> i32 {
    match event {
        CTRL_C_EVENT | CTRL_BREAK_EVENT => {
            INTERRUPTED.store(true, Ordering::Relaxed);
            1
        }
        CTRL_CLOSE_EVENT | CTRL_LOGOFF_EVENT | CTRL_SHUTDOWN_EVENT => {
            remove_link_marker();
            0
        }
        _ => 0,
    }
}
/// Run an interactive child on the shared console and wait for it. An
/// interrupted wait reports 130 like Unix SIGINT; otherwise the child's code.
pub fn wait_interactive(command: &mut Command) -> Result<i32> {
    struct Handler;
    impl Drop for Handler {
        fn drop(&mut self) {
            unsafe {
                SetConsoleCtrlHandler(Some(interrupted), 0);
            }
        }
    }
    INTERRUPTED.store(false, Ordering::Relaxed);
    // Never SetConsoleCtrlHandler(NULL, TRUE): children would inherit ignoring Ctrl+C.
    let _handler = (unsafe { SetConsoleCtrlHandler(Some(interrupted), 1) } != 0).then_some(Handler);
    let relay = Relay::prepare(command, None)?;
    let spawned = command.spawn();
    let pumps = relay.start(command);
    let mut child = spawned.map_err(|e| {
        Error::new(
            127,
            format!("{}: {e}", command.get_program().to_string_lossy()),
        )
    })?;
    let rc = code(child.wait()?);
    pumps.finish();
    Ok(if INTERRUPTED.load(Ordering::Relaxed) {
        130
    } else {
        rc
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn home_prefers_a_set_home_over_the_profile() {
        let some = |value: &str| Some(value.to_owned());
        assert_eq!(
            home_from(some(r"C:\msys"), some(r"C:\Users\a")).as_deref(),
            Some(r"C:\msys")
        );
        assert_eq!(
            home_from(some(""), some(r"C:\Users\a")).as_deref(),
            Some(r"C:\Users\a")
        );
        assert_eq!(
            home_from(None, some(r"C:\Users\a")).as_deref(),
            Some(r"C:\Users\a")
        );
        assert_eq!(home_from(None, some("")), None);
        assert_eq!(
            below(r"C:\Users\a b", ".config/hgs"),
            r"C:\Users\a b\.config\hgs"
        );
        assert_eq!(
            below(r"C:\Users\a\", ".local/state"),
            r"C:\Users\a\.local\state"
        );
    }

    #[test]
    fn pathext_decides_what_runs() {
        let dir = tempfile::tempdir().unwrap();
        for name in ["tool.EXE", "script.cmd", "plain", "notes.txt"] {
            fs::write(dir.path().join(name), b"").unwrap();
        }
        assert!(executable(dir.path().join("tool.EXE")));
        assert!(executable(dir.path().join("script.cmd")));
        assert!(!executable(dir.path().join("plain")));
        assert!(!executable(dir.path().join("notes.txt")));
        assert!(candidates(dir.path(), "tool").contains(&dir.path().join("tool.exe")));
        assert_eq!(
            candidates(dir.path(), "tool.exe"),
            [dir.path().join("tool.exe")]
        );
        assert!(is_path(r"C:\tools\ssh.exe") && is_path("./ssh") && !is_path("ssh"));
    }

    #[test]
    fn only_consoles_and_absent_streams_reach_a_child_unrelayed() {
        use windows_sys::Win32::Storage::FileSystem::FILE_TYPE_UNKNOWN;
        // A console: `ssh -t` and Ctrl+C need the handle itself.
        assert!(!relayed(FILE_TYPE_CHAR, true));
        // NUL or a serial port, a redirected file (`< in`, `>> log`) and any pipe.
        for file_type in [FILE_TYPE_CHAR, FILE_TYPE_DISK, FILE_TYPE_PIPE] {
            assert!(relayed(file_type, false), "{file_type}");
        }
        // No standard handle at all, or an invalid one.
        assert!(!relayed(FILE_TYPE_UNKNOWN, false));

        let dir = tempfile::tempdir().unwrap();
        let file = fs::File::create(dir.path().join("log")).unwrap();
        let nul = fs::File::open("NUL").unwrap();
        let (ours, theirs) = overlapped_pipe(true).unwrap();
        for handle in [&file, &nul, &ours, &theirs] {
            assert!(needs_relay(handle.as_raw_handle()));
        }
        assert!(!needs_relay(std::ptr::null_mut()));
        assert!(!needs_relay(INVALID_HANDLE_VALUE as RawHandle));
        // A console, when this test runs with one.
        if let Ok(console) = fs::OpenOptions::new()
            .read(true)
            .write(true)
            .open("CONOUT$")
        {
            assert!(!needs_relay(console.as_raw_handle()));
        }
    }

    #[test]
    fn processes_and_files_have_stable_identities() {
        assert!(process_alive(std::process::id()));
        assert!(!process_alive(0));
        assert!(!process_alive(u32::MAX));
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("file");
        fs::write(&path, b"one").unwrap();
        let first = file_identity(&path).unwrap();
        assert_eq!(file_identity(&path), Some(first));
        let next = dir.path().join("next");
        fs::write(&next, b"two").unwrap();
        fs::remove_file(&path).unwrap();
        fs::rename(&next, &path).unwrap();
        assert_ne!(file_identity(&path).unwrap().inode, first.inode);
    }

    #[test]
    fn link_markers_survive_command_characters_in_the_temporary_path() {
        let dir = tempfile::tempdir().unwrap();
        // A profile name may contain `%`; OpenSSH and cmd.exe both expand it.
        let folder = dir.path().join("50%off %OS% a&b ^c");
        fs::create_dir(&folder).unwrap();
        let marker = folder.join("up");
        let mut ssh = Command::new("ssh");
        let command = create_file_command(&mut ssh, &marker);
        assert_eq!(command, r#"type nul > "%%HGS_LINK_MARKER%%""#);
        let (_, value) = ssh
            .get_envs()
            .find(|(name, _)| *name == LOCAL_COMMAND_PATH)
            .unwrap();
        assert_eq!(value, Some(marker.as_os_str()));
        // OpenSSH turns `%%` into `%` and runs the result with `cmd.exe /c`,
        // which inherits the client's environment.
        let status = Command::new("cmd.exe")
            .arg("/c")
            .raw_arg(command.replace("%%", "%"))
            .env(LOCAL_COMMAND_PATH, &marker)
            .status()
            .unwrap();
        assert!(status.success());
        assert!(marker.is_file());
    }

    #[test]
    fn link_markers_of_gone_clients_are_removed() {
        let dir = tempfile::tempdir().unwrap();
        // A client that has exited, as one the desktop app killed.
        let mut gone = Command::new("cmd.exe")
            .args(["/c", "exit"])
            .spawn()
            .unwrap();
        let gone_pid = gone.id();
        gone.wait().unwrap();
        let stale = dir
            .path()
            .join(format!("{LINK_MARKER_PREFIX}{gone_pid}-AbC123"));
        let live = dir
            .path()
            .join(format!("{LINK_MARKER_PREFIX}{}-XyZ789", std::process::id()));
        let unrelated = dir.path().join("hgs-linkage");
        for folder in [&stale, &live, &unrelated] {
            fs::create_dir(folder).unwrap();
            fs::write(folder.join("up"), b"").unwrap();
        }
        remove_stale_link_markers(dir.path());
        assert!(!stale.exists());
        assert!(live.join("up").is_file());
        assert!(unrelated.join("up").is_file());
    }

    #[test]
    fn a_closing_console_removes_the_link_marker() {
        let marker = link_marker_dir().unwrap();
        let name = marker
            .path()
            .file_name()
            .unwrap()
            .to_string_lossy()
            .into_owned();
        assert!(name.starts_with(&format!("{LINK_MARKER_PREFIX}{}-", std::process::id())));
        fs::write(marker.path().join("up"), b"").unwrap();
        // Ctrl+C belongs to ssh.exe; the marker stays.
        assert_eq!(unsafe { interrupted(CTRL_C_EVENT) }, 1);
        assert!(marker.path().join("up").is_file());
        INTERRUPTED.store(false, Ordering::Relaxed);
        // The console closes: the process ends after the handler returns.
        assert_eq!(unsafe { interrupted(CTRL_CLOSE_EVENT) }, 0);
        assert!(!marker.path().exists());
    }

    #[test]
    fn private_files_stay_writable_and_directories_need_no_sync() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("private.json");
        let file = private_open(fs::OpenOptions::new().create_new(true).write(true))
            .open(&path)
            .unwrap();
        make_private(&file).unwrap();
        restrict_dir(dir.path()).unwrap();
        sync_dir(dir.path()).unwrap();
        assert!(!fs::metadata(&path).unwrap().permissions().readonly());
    }

    #[test]
    fn replacement_waits_for_a_reader_without_delete_sharing() {
        use std::io::Write;
        let dir = tempfile::tempdir().unwrap();
        let target = dir.path().join("catalog.json");
        fs::write(&target, b"old").unwrap();
        // FILE_SHARE_READ only, as some readers and scanners open files.
        let reader = fs::OpenOptions::new()
            .read(true)
            .share_mode(1)
            .open(&target)
            .unwrap();
        let release = std::thread::spawn(move || {
            std::thread::sleep(std::time::Duration::from_millis(150));
            drop(reader);
        });
        let mut temporary = tempfile::NamedTempFile::new_in(dir.path()).unwrap();
        temporary.write_all(b"new").unwrap();
        super::super::persist(temporary, &target).unwrap();
        release.join().unwrap();
        assert_eq!(fs::read(&target).unwrap(), b"new");

        // A holder that never lets go fails within the bounded wait.
        let _reader = fs::OpenOptions::new()
            .read(true)
            .share_mode(1)
            .open(&target)
            .unwrap();
        let mut temporary = tempfile::NamedTempFile::new_in(dir.path()).unwrap();
        temporary.write_all(b"blocked").unwrap();
        let started = std::time::Instant::now();
        assert!(super::super::persist(temporary, &target).is_err());
        assert!(started.elapsed() < std::time::Duration::from_secs(3));
        assert_eq!(fs::read(&target).unwrap(), b"new");
    }
}
