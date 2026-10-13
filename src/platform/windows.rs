//! Windows process, console and file primitives. A Windows build manages remote
//! machines through the system OpenSSH client; no agent session runs here.
use super::{
    capture, ExitInfo, FileIdentity, Presence, Private, ProcessIdentity, ProcessImage, ProcessRow,
    RunError,
};
use crate::cli::{Error, Result};
use std::{
    collections::BTreeMap,
    env,
    ffi::OsString,
    fs,
    io::{self, Read, Seek, SeekFrom},
    os::windows::{
        ffi::OsStrExt,
        fs::{MetadataExt, OpenOptionsExt},
        io::{AsRawHandle, FromRawHandle, RawHandle},
        process::CommandExt,
    },
    path::{Path, PathBuf},
    process::{Child, Command, ExitStatus, Output, Stdio},
    sync::{
        atomic::{AtomicBool, AtomicI32, AtomicU32, AtomicU64, Ordering},
        Arc, Mutex, OnceLock,
    },
    thread,
    time::{Duration, Instant},
};
use windows_sys::{
    Wdk::System::Threading::{NtQueryInformationProcess, ProcessCommandLineInformation},
    Win32::{
        Foundation::{
            CloseHandle, GetLastError, LocalFree, ERROR_ACCESS_DENIED, ERROR_INVALID_PARAMETER,
            ERROR_LOCK_VIOLATION, ERROR_MORE_DATA, ERROR_SHARING_VIOLATION, ERROR_SUCCESS,
            FILETIME, GENERIC_READ, GENERIC_WRITE, HANDLE, INVALID_HANDLE_VALUE, UNICODE_STRING,
            WAIT_OBJECT_0, WAIT_TIMEOUT,
        },
        Security::{
            Authorization::{GetSecurityInfo, SE_FILE_OBJECT},
            GetAce, GetLengthSid, GetTokenInformation, IsWellKnownSid, TokenUser,
            WinBuiltinAdministratorsSid, WinCreatorOwnerRightsSid, WinLocalSystemSid,
            ACCESS_ALLOWED_ACE, ACE_HEADER, ACL, DACL_SECURITY_INFORMATION, INHERIT_ONLY_ACE,
            OWNER_SECURITY_INFORMATION, PSECURITY_DESCRIPTOR, PSID, TOKEN_QUERY, TOKEN_USER,
        },
        Storage::FileSystem::{
            CreateFileW, GetFileInformationByHandle, GetFileType, BY_HANDLE_FILE_INFORMATION,
            FILE_ATTRIBUTE_DIRECTORY, FILE_ATTRIBUTE_REPARSE_POINT, FILE_FLAG_BACKUP_SEMANTICS,
            FILE_FLAG_FIRST_PIPE_INSTANCE, FILE_FLAG_OPEN_REPARSE_POINT, FILE_FLAG_OVERLAPPED,
            FILE_READ_ATTRIBUTES, FILE_SHARE_DELETE, FILE_SHARE_READ, FILE_SHARE_WRITE, FILE_TYPE,
            FILE_TYPE_CHAR, FILE_TYPE_DISK, FILE_TYPE_PIPE, FILE_WRITE_ATTRIBUTES, OPEN_EXISTING,
            PIPE_ACCESS_INBOUND, PIPE_ACCESS_OUTBOUND, READ_CONTROL,
        },
        System::{
            Console::{
                GenerateConsoleCtrlEvent, GetConsoleMode, GetStdHandle, SetConsoleCtrlHandler,
                SetConsoleMode, CTRL_BREAK_EVENT, CTRL_CLOSE_EVENT, CTRL_C_EVENT,
                CTRL_LOGOFF_EVENT, CTRL_SHUTDOWN_EVENT, ENABLE_VIRTUAL_TERMINAL_PROCESSING,
                STD_OUTPUT_HANDLE,
            },
            Diagnostics::ToolHelp::{
                CreateToolhelp32Snapshot, Process32FirstW, Process32NextW, Thread32First,
                Thread32Next, PROCESSENTRY32W, TH32CS_SNAPPROCESS, TH32CS_SNAPTHREAD,
                THREADENTRY32,
            },
            JobObjects::{
                AssignProcessToJobObject, CreateJobObjectW, JobObjectExtendedLimitInformation,
                SetInformationJobObject, TerminateJobObject, JOBOBJECT_EXTENDED_LIMIT_INFORMATION,
                JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
            },
            Pipes::{
                CreateNamedPipeW, PIPE_READMODE_BYTE, PIPE_REJECT_REMOTE_CLIENTS, PIPE_TYPE_BYTE,
                PIPE_WAIT,
            },
            RestartManager::{
                RmEndSession, RmGetList, RmRegisterResources, RmStartSession, CCH_RM_SESSION_KEY,
                RM_PROCESS_INFO,
            },
            Threading::{
                GetCurrentProcess, GetProcessTimes, OpenProcess, OpenProcessToken, OpenThread,
                QueryFullProcessImageNameW, ResumeThread, TerminateProcess, WaitForSingleObject,
                CREATE_BREAKAWAY_FROM_JOB, CREATE_NEW_PROCESS_GROUP, CREATE_NO_WINDOW,
                CREATE_SUSPENDED, PROCESS_NAME_WIN32, PROCESS_QUERY_LIMITED_INFORMATION,
                PROCESS_SYNCHRONIZE, PROCESS_TERMINATE, THREAD_SUSPEND_RESUME,
            },
        },
        UI::Shell::CommandLineToArgvW,
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
/// Whether this computer runs agent sessions. A Windows computer will once the
/// user enables them (the `%USERPROFILE%\.config\hgs\local-sessions` marker) and
/// its session host exists (Phase 2 milestone M3); until then it is a control
/// console for remote machines whatever the marker says.
pub fn local_sessions() -> bool {
    false
}
/// Windows reports every exit as a code; crash statuses read as signals (see `exit_info`).
pub fn code(status: ExitStatus) -> i32 {
    ExitInfo::of(status).exit_code()
}
/// NTSTATUS values that end a process the way a signal ends a Unix one, with
/// that signal's number: Ctrl+C or a closed console, memory faults, illegal
/// instructions, arithmetic faults, breakpoints and fail-fast aborts (which is
/// how a Rust panic set to abort ends).
const CRASH_SIGNALS: [(u32, i32); 18] = [
    (0xC000_013A, 2),  // STATUS_CONTROL_C_EXIT: SIGINT
    (0xC000_0005, 11), // STATUS_ACCESS_VIOLATION: SIGSEGV
    (0xC000_0006, 11), // STATUS_IN_PAGE_ERROR
    (0xC000_00FD, 11), // STATUS_STACK_OVERFLOW
    (0xC000_008C, 11), // STATUS_ARRAY_BOUNDS_EXCEEDED
    (0xC000_001D, 4),  // STATUS_ILLEGAL_INSTRUCTION: SIGILL
    (0xC000_0096, 4),  // STATUS_PRIVILEGED_INSTRUCTION
    (0xC000_008D, 8),  // STATUS_FLOAT_DENORMAL_OPERAND: SIGFPE
    (0xC000_008E, 8),  // STATUS_FLOAT_DIVIDE_BY_ZERO
    (0xC000_008F, 8),  // STATUS_FLOAT_INEXACT_RESULT
    (0xC000_0090, 8),  // STATUS_FLOAT_INVALID_OPERATION
    (0xC000_0091, 8),  // STATUS_FLOAT_OVERFLOW
    (0xC000_0092, 8),  // STATUS_FLOAT_STACK_CHECK
    (0xC000_0093, 8),  // STATUS_FLOAT_UNDERFLOW
    (0xC000_0094, 8),  // STATUS_INTEGER_DIVIDE_BY_ZERO
    (0xC000_0095, 8),  // STATUS_INTEGER_OVERFLOW
    (0x8000_0003, 5),  // STATUS_BREAKPOINT: SIGTRAP
    (0xC000_0409, 6),  // STATUS_STACK_BUFFER_OVERRUN (fail fast): SIGABRT
];
/// Other codes, including large ones such as `exit(-1)`, stay exit codes.
pub(super) fn exit_info(status: ExitStatus) -> ExitInfo {
    let Some(code) = status.code() else {
        return ExitInfo {
            code: None,
            signal: None,
        };
    };
    match CRASH_SIGNALS
        .iter()
        .find(|(status, _)| *status == code as u32)
    {
        Some(&(_, signal)) => ExitInfo {
            code: None,
            signal: Some(signal),
        },
        None => ExitInfo {
            code: Some(code),
            signal: None,
        },
    }
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
/// Volume and file index identify the file itself; creation times do not,
/// because NTFS tunnelling gives a replacement under the same name the old one.
pub fn file_identity(path: &Path) -> Option<FileIdentity> {
    FileIdentity::of(&fs::File::open(path).ok()?).ok()
}
impl FileIdentity {
    /// The identity and metadata of an open file. (Its callers in `state` are
    /// Unix-only for now, as is `at`'s.)
    #[allow(dead_code)]
    pub fn with_metadata(file: &fs::File) -> io::Result<(Self, fs::Metadata)> {
        Ok((Self::of(file)?, file.metadata()?))
    }
    /// The identity and metadata of the file `path` names, following links.
    #[allow(dead_code)]
    pub fn at(path: &Path) -> io::Result<(Self, fs::Metadata)> {
        Self::with_metadata(&fs::File::open(path)?)
    }
    /// The identity of a file this process has open.
    pub fn of(file: &fs::File) -> io::Result<Self> {
        let info = file_information(file)?;
        let (modified, modified_nanos) = unix_time(ticks(info.ftLastWriteTime));
        Ok(Self {
            device: info.dwVolumeSerialNumber.into(),
            inode: (u64::from(info.nFileIndexHigh) << 32) | u64::from(info.nFileIndexLow),
            modified,
            modified_nanos,
        })
    }
}
fn file_information(file: &fs::File) -> io::Result<BY_HANDLE_FILE_INFORMATION> {
    let mut info: BY_HANDLE_FILE_INFORMATION = unsafe { std::mem::zeroed() };
    if unsafe { GetFileInformationByHandle(file.as_raw_handle() as HANDLE, &mut info) } == 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(info)
}
/// 100 ns ticks from 1601-01-01, when FILETIME starts, to 1970-01-01.
const UNIX_EPOCH_TICKS: u64 = 116_444_736_000_000_000;
fn ticks(time: FILETIME) -> u64 {
    (u64::from(time.dwHighDateTime) << 32) | u64::from(time.dwLowDateTime)
}
/// FILETIME ticks as Unix seconds and nanoseconds, floored like `st_mtime`.
fn unix_time(ticks: u64) -> (i64, i64) {
    let since = ticks as i128 - UNIX_EPOCH_TICKS as i128;
    (
        since.div_euclid(10_000_000) as i64,
        (since.rem_euclid(10_000_000) * 100) as i64,
    )
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

pub use self::process::*;
/// Process, file and control primitives for `state`.
mod process {
    use super::*;

    /// `home` as a path.
    pub fn home_dir() -> Option<PathBuf> {
        home().map(PathBuf::from)
    }
    /// The canonical form of `path` without the `\\?\` prefix that canonicalize
    /// adds, whenever the plain form names the same file. Stored and compared
    /// paths, and paths agents report, use the plain form.
    pub fn canonical(path: impl AsRef<Path>) -> io::Result<PathBuf> {
        fs::canonicalize(path).map(plain_path)
    }
    /// A verbatim path is kept only when the plain form would differ: longer than
    /// MAX_PATH, or with a component Win32 would change or treat as a device
    /// (trailing dots or spaces, `NUL`, `COM1.txt`, a stream name after `:`).
    pub(super) fn plain_path(path: PathBuf) -> PathBuf {
        let Some(text) = path.to_str() else {
            return path;
        };
        let (plain, components) = if let Some(rest) = text.strip_prefix(r"\\?\UNC\") {
            (format!(r"\\{rest}"), rest)
        } else if let Some(rest) = text.strip_prefix(r"\\?\") {
            let bytes = rest.as_bytes();
            if bytes.len() < 3 || !bytes[0].is_ascii_alphabetic() || &bytes[1..3] != b":\\" {
                return path;
            }
            (rest.to_owned(), &rest[3..])
        } else {
            return path;
        };
        let legacy = |name: &str| {
            let stem = name
                .split('.')
                .next()
                .unwrap_or("")
                .trim_end()
                .to_ascii_uppercase();
            let device = ["CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"].contains(&stem.as_str())
                || ((stem.starts_with("COM") || stem.starts_with("LPT"))
                    && stem.len() == 4
                    && stem.as_bytes()[3].is_ascii_digit()
                    && stem.as_bytes()[3] != b'0');
            !name.is_empty()
                && !name.ends_with(['.', ' '])
                && !name.contains(['<', '>', '"', '/', '|', '?', '*', ':'])
                && !device
        };
        if plain.encode_utf16().count() >= 260
            || !components
                .split('\\')
                .filter(|name| !name.is_empty())
                .all(legacy)
        {
            return path;
        }
        PathBuf::from(plain)
    }
    /// Extensions of process images and of the command shims npm installs. Not
    /// PATHEXT: it differs between computers and by default also lists scripts
    /// such as `.js`, so `codex.js` would read as `codex` on one and not another.
    const PROGRAM_EXTENSIONS: [&str; 4] = [".exe", ".com", ".bat", ".cmd"];
    /// The name a program goes by in process names and command lines: the file
    /// name without an extension that makes it a program (`node.exe` is `node`),
    /// lowercase because Windows names are case-insensitive.
    pub fn program_name(path: &Path) -> Option<String> {
        let name = path.file_name()?.to_str()?.to_lowercase();
        match name.rfind('.') {
            Some(dot) if dot > 0 && PROGRAM_EXTENSIONS.contains(&&name[dot..]) => {
                Some(name[..dot].to_owned())
            }
            _ => Some(name),
        }
    }
    /// Directories below the user profile inherit its owner-only ACL.
    pub fn private_dir(path: &Path) -> io::Result<()> {
        fs::create_dir_all(path)
    }
    /// Open a reparse point (a symbolic link) itself; it is then not a regular file.
    pub fn refuse_link(options: &mut fs::OpenOptions) -> &mut fs::OpenOptions {
        options.custom_flags(FILE_FLAG_OPEN_REPARSE_POINT)
    }
    /// A file path names no FIFO here, so opening one never waits for a writer.
    pub fn never_block(options: &mut fs::OpenOptions) -> &mut fs::OpenOptions {
        options
    }
    /// Whether `path` itself (a link or junction is a reparse point and never
    /// counts) is the expected kind of entry, owned by this user, with no access
    /// for anyone but this user, SYSTEM and Administrators and, for a file, a
    /// single link.
    pub fn verify_private(path: &Path, kind: Private) -> io::Result<bool> {
        let entry = fs::OpenOptions::new()
            .access_mode(READ_CONTROL | FILE_READ_ATTRIBUTES)
            .share_mode(FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)
            .custom_flags(FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS)
            .open(path)?;
        private_entry(&entry, kind)
    }
    /// `verify_private` for a file this process has open (for reading or writing).
    pub fn verify_private_file(file: &fs::File) -> io::Result<bool> {
        private_entry(file, Private::File)
    }
    fn private_entry(file: &fs::File, kind: Private) -> io::Result<bool> {
        let info = file_information(file)?;
        let attributes = info.dwFileAttributes;
        if attributes & FILE_ATTRIBUTE_REPARSE_POINT != 0 {
            return Ok(false);
        }
        let shaped = match kind {
            Private::Directory => attributes & FILE_ATTRIBUTE_DIRECTORY != 0,
            Private::File => attributes & FILE_ATTRIBUTE_DIRECTORY == 0 && info.nNumberOfLinks == 1,
        };
        Ok(shaped && private_security(file.as_raw_handle() as HANDLE)?)
    }
    const ACCESS_ALLOWED_ACE_TYPE: u8 = 0;
    const ACCESS_DENIED_ACE_TYPE: u8 = 1;
    /// The owner is this user and every ACE that grants access names this user,
    /// SYSTEM, Administrators or OWNER RIGHTS, the ACL a user profile passes on.
    /// An unknown kind of allowing ACE fails closed.
    fn private_security(handle: HANDLE) -> io::Result<bool> {
        let user = current_user()
            .ok_or_else(|| io::Error::other("this user's identity is unavailable"))?;
        let mut owner: PSID = std::ptr::null_mut();
        let mut dacl: *mut ACL = std::ptr::null_mut();
        let mut descriptor: PSECURITY_DESCRIPTOR = std::ptr::null_mut();
        let code = unsafe {
            GetSecurityInfo(
                handle,
                SE_FILE_OBJECT,
                OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                &mut owner,
                std::ptr::null_mut(),
                &mut dacl,
                std::ptr::null_mut(),
                &mut descriptor,
            )
        };
        if code != ERROR_SUCCESS {
            return Err(io::Error::from_raw_os_error(code as i32));
        }
        struct Descriptor(PSECURITY_DESCRIPTOR);
        impl Drop for Descriptor {
            fn drop(&mut self) {
                unsafe {
                    LocalFree(self.0);
                }
            }
        }
        let _descriptor = Descriptor(descriptor);
        if owner.is_null() || unsafe { sid_bytes(owner) } != user {
            return Ok(false);
        }
        // A missing DACL grants everyone everything.
        if dacl.is_null() {
            return Ok(false);
        }
        for index in 0..u32::from(unsafe { (*dacl).AceCount }) {
            let mut ace = std::ptr::null_mut();
            if unsafe { GetAce(dacl, index, &mut ace) } == 0 {
                return Err(io::Error::last_os_error());
            }
            let header = unsafe { &*(ace as *const ACE_HEADER) };
            if u32::from(header.AceFlags) & INHERIT_ONLY_ACE != 0
                || header.AceType == ACCESS_DENIED_ACE_TYPE
            {
                continue;
            }
            if header.AceType != ACCESS_ALLOWED_ACE_TYPE {
                return Ok(false);
            }
            let sid = unsafe { std::ptr::addr_of!((*(ace as *const ACCESS_ALLOWED_ACE)).SidStart) }
                as PSID;
            let trusted = unsafe {
                sid_bytes(sid) == user
                    || IsWellKnownSid(sid, WinLocalSystemSid) != 0
                    || IsWellKnownSid(sid, WinBuiltinAdministratorsSid) != 0
                    || IsWellKnownSid(sid, WinCreatorOwnerRightsSid) != 0
            };
            if !trusted {
                return Ok(false);
            }
        }
        Ok(true)
    }
    /// # Safety
    /// `sid` points to a valid SID.
    unsafe fn sid_bytes<'a>(sid: PSID) -> &'a [u8] {
        std::slice::from_raw_parts(sid as *const u8, GetLengthSid(sid) as usize)
    }
    /// The user SID of `process`'s token, when it can be read.
    fn token_user(process: HANDLE) -> Option<Vec<u8>> {
        let mut token = std::ptr::null_mut();
        if unsafe { OpenProcessToken(process, TOKEN_QUERY, &mut token) } == 0 {
            return None;
        }
        let token = Owned(token);
        let mut size = 0;
        unsafe {
            GetTokenInformation(token.0, TokenUser, std::ptr::null_mut(), 0, &mut size);
        }
        if size == 0 {
            return None;
        }
        // u64 elements keep the TOKEN_USER header aligned.
        let mut buffer = vec![0u64; (size as usize).div_ceil(8)];
        if unsafe {
            GetTokenInformation(
                token.0,
                TokenUser,
                buffer.as_mut_ptr().cast(),
                size,
                &mut size,
            )
        } == 0
        {
            return None;
        }
        let user = unsafe { &*(buffer.as_ptr() as *const TOKEN_USER) };
        Some(unsafe { sid_bytes(user.User.Sid) }.to_vec())
    }
    fn current_user() -> Option<&'static [u8]> {
        static USER: OnceLock<Option<Vec<u8>>> = OnceLock::new();
        USER.get_or_init(|| token_user(unsafe { GetCurrentProcess() }))
            .as_deref()
    }

    /// A kernel handle this process owns, closed when dropped.
    struct Owned(HANDLE);
    impl Drop for Owned {
        fn drop(&mut self) {
            unsafe {
                CloseHandle(self.0);
            }
        }
    }
    fn open_process(pid: u32, access: u32) -> io::Result<Owned> {
        let process = unsafe { OpenProcess(access, 0, pid) };
        if process.is_null() {
            Err(io::Error::last_os_error())
        } else {
            Ok(Owned(process))
        }
    }
    /// OpenProcess reports an ID that no process has as an invalid parameter.
    fn no_such_process(error: &io::Error) -> bool {
        error.raw_os_error() == Some(ERROR_INVALID_PARAMETER as i32)
    }
    /// A process object stays readable after its process exits while any handle
    /// to it is open; such a process is gone, like a Unix zombie.
    fn exited(process: HANDLE) -> bool {
        unsafe { WaitForSingleObject(process, 0) == WAIT_OBJECT_0 }
    }
    fn creation_ticks(process: HANDLE) -> Option<u64> {
        let mut times: [FILETIME; 4] = unsafe { std::mem::zeroed() };
        let [creation, exit, kernel, user] = &mut times;
        (unsafe { GetProcessTimes(process, creation, exit, kernel, user) } != 0)
            .then(|| ticks(*creation))
    }

    pub fn presence(pid: u32) -> Presence {
        // 0 is the idle process, never a process of ours.
        if pid == 0 {
            return Presence::Unknown;
        }
        match open_process(pid, PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SYNCHRONIZE) {
            Ok(process) if exited(process.0) => Presence::Gone,
            Ok(_) => Presence::Present,
            Err(error) if no_such_process(&error) => Presence::Gone,
            Err(_) => Presence::Unknown,
        }
    }
    /// When `pid` was created, as decimal 100 ns ticks. Records are per computer,
    /// so this compares as text exactly like the Unix `lstart` form, at a far finer
    /// resolution: Windows reuses process IDs quickly. Empty when the process does
    /// not exist or has exited; None when it cannot be opened (another user's).
    pub fn process_start_time(pid: u32) -> Option<String> {
        if pid == 0 {
            return Some(String::new());
        }
        let process =
            match open_process(pid, PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SYNCHRONIZE) {
                Ok(process) => process,
                Err(error) if no_such_process(&error) => return Some(String::new()),
                Err(_) => return None,
            };
        if exited(process.0) {
            return Some(String::new());
        }
        creation_ticks(process.0).map(|ticks| ticks.to_string())
    }
    /// There is no ps to ask.
    pub fn process_start_fallback(_: u32) -> Option<String> {
        None
    }

    /// Every process as (pid, parent, image file name) from a Toolhelp snapshot.
    fn snapshot() -> io::Result<Vec<(u32, u32, String)>> {
        let handle = unsafe { CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) };
        if handle == INVALID_HANDLE_VALUE {
            return Err(io::Error::last_os_error());
        }
        let handle = Owned(handle);
        let mut entry: PROCESSENTRY32W = unsafe { std::mem::zeroed() };
        entry.dwSize = std::mem::size_of::<PROCESSENTRY32W>() as u32;
        let mut rows = Vec::new();
        let mut more = unsafe { Process32FirstW(handle.0, &mut entry) } != 0;
        while more {
            let name = &entry.szExeFile;
            let length = name.iter().position(|c| *c == 0).unwrap_or(name.len());
            rows.push((
                entry.th32ProcessID,
                entry.th32ParentProcessID,
                String::from_utf16_lossy(&name[..length]),
            ));
            more = unsafe { Process32NextW(handle.0, &mut entry) } != 0;
        }
        Ok(rows)
    }
    /// Toolhelp keeps a parent's ID after the parent exits, and Windows soon gives
    /// that ID to another process: a parent link counts only while a process with
    /// that ID exists and was created no later than the child. Otherwise 0.
    fn verified_parent(parent: u32, child_created: u64) -> u32 {
        let created = open_process(parent, PROCESS_QUERY_LIMITED_INFORMATION)
            .ok()
            .and_then(|process| creation_ticks(process.0));
        match created {
            Some(created) if parent != 0 && created <= child_created => parent,
            _ => 0,
        }
    }
    fn listed_parent(pid: u32) -> Option<u32> {
        snapshot()
            .ok()?
            .into_iter()
            .find(|row| row.0 == pid)
            .map(|row| row.1)
    }
    /// FILETIME ticks as Unix seconds.
    fn unix_seconds(ticks: u64) -> f64 {
        (ticks as f64 - UNIX_EPOCH_TICKS as f64) / 10_000_000.
    }

    pub fn process_identity(pid: u32) -> Option<ProcessIdentity> {
        let process =
            open_process(pid, PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SYNCHRONIZE).ok()?;
        if exited(process.0) || token_user(process.0).as_deref() != current_user() {
            return None;
        }
        let created = creation_ticks(process.0)?;
        Some(ProcessIdentity {
            parent: verified_parent(listed_parent(pid)?, created),
            group: None,
            birth: created.to_string(),
            started: unix_seconds(created),
        })
    }
    pub fn describe(pid: u32) -> Option<ProcessImage> {
        let process = open_process(pid, PROCESS_QUERY_LIMITED_INFORMATION).ok()?;
        let mut path = vec![0u16; 32_768];
        let mut size = path.len() as u32;
        if unsafe {
            QueryFullProcessImageNameW(process.0, PROCESS_NAME_WIN32, path.as_mut_ptr(), &mut size)
        } == 0
        {
            return None;
        }
        Some(ProcessImage {
            executable: String::from_utf16_lossy(&path[..size as usize]),
            argv: command_line(process.0).map(|line| split_command_line(&line)),
        })
    }
    /// The command line a process was started with (Windows 8.1 and later).
    fn command_line(process: HANDLE) -> Option<Vec<u16>> {
        let mut size = 0u32;
        unsafe {
            NtQueryInformationProcess(
                process,
                ProcessCommandLineInformation,
                std::ptr::null_mut(),
                0,
                &mut size,
            );
        }
        if (size as usize) < std::mem::size_of::<UNICODE_STRING>() {
            return None;
        }
        // A UNICODE_STRING followed by its text; u64 elements keep it aligned.
        let mut buffer = vec![0u64; (size as usize).div_ceil(8)];
        let status = unsafe {
            NtQueryInformationProcess(
                process,
                ProcessCommandLineInformation,
                buffer.as_mut_ptr().cast(),
                size,
                &mut size,
            )
        };
        if status < 0 {
            return None;
        }
        let text = unsafe { &*(buffer.as_ptr() as *const UNICODE_STRING) };
        if text.Buffer.is_null() {
            return Some(Vec::new());
        }
        Some(
            unsafe { std::slice::from_raw_parts(text.Buffer, usize::from(text.Length) / 2) }
                .to_vec(),
        )
    }
    /// Arguments as the C runtime splits a command line.
    fn split_command_line(line: &[u16]) -> Vec<String> {
        // An empty line would give this process's own path instead.
        if line
            .iter()
            .all(|c| *c == u16::from(b' ') || *c == u16::from(b'\t'))
        {
            return Vec::new();
        }
        let text: Vec<u16> = line.iter().copied().chain(Some(0)).collect();
        let mut count = 0;
        let argv = unsafe { CommandLineToArgvW(text.as_ptr(), &mut count) };
        if argv.is_null() {
            return Vec::new();
        }
        let arguments = (0..usize::try_from(count).unwrap_or(0))
            .map(|index| unsafe {
                let argument = *argv.add(index);
                let length = (0..).take_while(|&at| *argument.add(at) != 0).count();
                String::from_utf16_lossy(std::slice::from_raw_parts(argument, length))
            })
            .collect();
        unsafe {
            LocalFree(argv.cast());
        }
        arguments
    }
    /// Every process with its parent as Toolhelp lists it; Windows has no process
    /// groups. Parent links are raw here; `process_identity` verifies them.
    pub fn process_table() -> std::result::Result<BTreeMap<u32, ProcessRow>, String> {
        let rows = snapshot().map_err(|_| "Live process inspection is unavailable".to_owned())?;
        Ok(rows
            .into_iter()
            .map(|(pid, parent, _)| {
                (
                    pid,
                    ProcessRow {
                        pid,
                        parent,
                        group: None,
                    },
                )
            })
            .collect())
    }
    /// The verified parent and the arguments of `pid`.
    pub fn parent_and_argv(pid: u32) -> Option<(u32, Vec<String>)> {
        let process = open_process(pid, PROCESS_QUERY_LIMITED_INFORMATION).ok()?;
        let created = creation_ticks(process.0)?;
        let argv = split_command_line(&command_line(process.0)?);
        Some((verified_parent(listed_parent(pid)?, created), argv))
    }
    pub fn parent_process_id() -> u32 {
        let created = creation_ticks(unsafe { GetCurrentProcess() }).unwrap_or(0);
        listed_parent(std::process::id()).map_or(0, |parent| verified_parent(parent, created))
    }
    /// The verified parent and image file name of `pid`, each None when unknown.
    pub fn parent_and_name(pid: u32) -> io::Result<(Option<u32>, Option<String>)> {
        let Some((_, parent, name)) = snapshot()?.into_iter().find(|row| row.0 == pid) else {
            return Ok((None, None));
        };
        let created = open_process(pid, PROCESS_QUERY_LIMITED_INFORMATION)
            .ok()
            .and_then(|process| creation_ticks(process.0));
        let parent = created
            .map(|created| verified_parent(parent, created))
            .filter(|parent| *parent != 0);
        Ok((parent, Some(name)))
    }
    /// Another process's current directory lives in its memory, not in anything
    /// the system reports; until hgs reads it there, nothing may count as unused.
    pub fn current_directories() -> std::result::Result<Vec<(u32, PathBuf, String)>, String> {
        Err("Running processes cannot be inspected on this computer".into())
    }
    /// Windows cannot list another process's open files without a driver; it can
    /// only say who holds a given file (`holds_open`).
    pub fn open_files(_: u32) -> std::result::Result<Vec<PathBuf>, RunError> {
        Err(RunError::Unsupported)
    }
    /// Whether `pid` has the file at `path` open, as the Restart Manager reports it.
    pub fn holds_open(pid: u32, path: &Path) -> std::result::Result<bool, RunError> {
        let check = |code: u32| {
            if code == ERROR_SUCCESS {
                Ok(())
            } else {
                Err(RunError::Io(io::Error::from_raw_os_error(code as i32)))
            }
        };
        struct Session(u32);
        impl Drop for Session {
            fn drop(&mut self) {
                unsafe {
                    RmEndSession(self.0);
                }
            }
        }
        let mut handle = 0;
        let mut key = [0u16; CCH_RM_SESSION_KEY as usize + 1];
        check(unsafe { RmStartSession(&mut handle, 0, key.as_mut_ptr()) })?;
        let session = Session(handle);
        let name: Vec<u16> = path.as_os_str().encode_wide().chain(Some(0)).collect();
        let files = [name.as_ptr()];
        check(unsafe {
            RmRegisterResources(
                session.0,
                1,
                files.as_ptr(),
                0,
                std::ptr::null(),
                0,
                std::ptr::null(),
            )
        })?;
        let mut holders: Vec<RM_PROCESS_INFO> = Vec::new();
        loop {
            let (mut needed, mut count, mut reasons) = (0, holders.len() as u32, 0);
            let list = if holders.is_empty() {
                std::ptr::null_mut()
            } else {
                holders.as_mut_ptr()
            };
            let code = unsafe { RmGetList(session.0, &mut needed, &mut count, list, &mut reasons) };
            if code == ERROR_SUCCESS {
                holders.truncate(count as usize);
                break;
            }
            if code != ERROR_MORE_DATA {
                return Err(RunError::Io(io::Error::from_raw_os_error(code as i32)));
            }
            holders = (0..needed).map(|_| RM_PROCESS_INFO::default()).collect();
        }
        Ok(holders
            .iter()
            .any(|holder| holder.Process.dwProcessId == pid))
    }

    /// A process to stop, pinned by an open handle: Windows never gives its ID to
    /// another process while the handle is open.
    pub struct PinnedProcess {
        pid: u32,
        handle: Owned,
    }
    impl PinnedProcess {
        pub fn pin(pid: u32, birth: &str) -> std::result::Result<Self, String> {
            let handle = open_process(
                pid,
                PROCESS_SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
            )
            .map_err(|_| "Process changed or cannot be opened".to_owned())?;
            if exited(handle.0)
                || creation_ticks(handle.0)
                    .map(|ticks| ticks.to_string())
                    .as_deref()
                    != Some(birth)
            {
                return Err("Process identity changed".into());
            }
            Ok(Self { pid, handle })
        }
        pub fn alive(&self) -> bool {
            !exited(self.handle.0)
        }
        /// Windows has no polite request to exit that reaches any process; the
        /// caller's grace period passes and `kill` ends it.
        pub fn terminate(&self) -> std::result::Result<(), String> {
            Ok(())
        }
        pub fn kill(&self) -> std::result::Result<(), String> {
            if self.alive() && unsafe { TerminateProcess(self.handle.0, 1) } == 0 && self.alive() {
                return Err(format!(
                    "Could not stop PID {}: {}",
                    self.pid,
                    io::Error::last_os_error()
                ));
            }
            Ok(())
        }
    }

    /// Windows has no request to exit that reaches a process on another console;
    /// the session host asks an agent's supervisor instead (Phase 2 D11).
    pub fn request_exit(_: u32) -> io::Result<()> {
        Err(io::Error::new(
            io::ErrorKind::Unsupported,
            "an agent on Windows is stopped through its session host",
        ))
    }

    /// Stop requests in the signal numbers records use: SIGTERM for Ctrl+Break
    /// (the pause request), SIGHUP for a console that closes.
    const SIGHUP: i32 = 1;
    const SIGTERM: i32 = 15;
    static SHUTDOWN_SIGNAL: AtomicI32 = AtomicI32::new(0);
    static SHUTDOWN_DONE: AtomicBool = AtomicBool::new(false);
    pub(super) unsafe extern "system" fn shutdown_event(event: u32) -> i32 {
        match event {
            // Ctrl+C belongs to the agent, which shares the console and gets it too.
            CTRL_C_EVENT => 1,
            CTRL_BREAK_EVENT => {
                SHUTDOWN_SIGNAL.store(SIGTERM, Ordering::SeqCst);
                1
            }
            CTRL_CLOSE_EVENT => {
                SHUTDOWN_SIGNAL.store(SIGHUP, Ordering::SeqCst);
                // Windows ends this process once the handler returns, and at the
                // latest after 5 s: leave time to record how the agent ended.
                let deadline = Instant::now() + Duration::from_secs(4);
                while !SHUTDOWN_DONE.load(Ordering::SeqCst) && Instant::now() < deadline {
                    thread::sleep(Duration::from_millis(20));
                }
                1
            }
            // Neither says which user logs off or whether this session ends.
            CTRL_LOGOFF_EVENT | CTRL_SHUTDOWN_EVENT => 1,
            _ => 0,
        }
    }
    /// A supervisor's stop handling until it is dropped. Ctrl+C belongs to the
    /// agent; Ctrl+Break and a closing console are recorded for `shutdown_signal`.
    /// The console delivers both to the agent itself, which shares it.
    pub struct ShutdownGuard(());
    impl ShutdownGuard {
        pub fn install() -> io::Result<Self> {
            SHUTDOWN_SIGNAL.store(0, Ordering::SeqCst);
            SHUTDOWN_DONE.store(false, Ordering::SeqCst);
            unsafe {
                // A process group created for this process starts with Ctrl+C
                // ignored, and children inherit that; the agent must receive it.
                SetConsoleCtrlHandler(None, 0);
                if SetConsoleCtrlHandler(Some(shutdown_event), 1) == 0 {
                    return Err(io::Error::last_os_error());
                }
            }
            Ok(Self(()))
        }
    }
    impl Drop for ShutdownGuard {
        fn drop(&mut self) {
            SHUTDOWN_DONE.store(true, Ordering::SeqCst);
            unsafe {
                SetConsoleCtrlHandler(Some(shutdown_event), 0);
            }
        }
    }
    pub fn shutdown_signal() -> i32 {
        SHUTDOWN_SIGNAL.load(Ordering::SeqCst)
    }
    /// The console delivers later requests to `child` itself. One that arrived
    /// before it started never reached it: raise Ctrl+Break again on this console.
    pub fn forward_shutdown(_: &Child) {
        if SHUTDOWN_SIGNAL.load(Ordering::SeqCst) != 0 {
            unsafe {
                GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0);
            }
        }
    }
    /// Nothing is passed on (see `forward_shutdown`), so nothing stops.
    pub fn shutdown_child_exited() {}

    /// Start `command` outside this process's job, with a hidden console of its
    /// own (its console children never open a window) and its own process group.
    /// Fails while an enclosing job forbids breakaway; `prepare_environment`'s job
    /// does today.
    pub fn spawn_detached(command: &mut Command) -> io::Result<Child> {
        command
            .creation_flags(CREATE_BREAKAWAY_FROM_JOB | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW)
            .spawn()
    }

    /// A job that ends its processes when its last handle, held only here, closes.
    /// It never allows breakaway, so nothing in it (MSYS programs break away
    /// whenever a job lets them) can leave it.
    struct Job(Owned);
    impl Job {
        fn new() -> io::Result<Self> {
            let job = unsafe { CreateJobObjectW(std::ptr::null(), std::ptr::null()) };
            if job.is_null() {
                return Err(io::Error::last_os_error());
            }
            let job = Self(Owned(job));
            let mut limits: JOBOBJECT_EXTENDED_LIMIT_INFORMATION = unsafe { std::mem::zeroed() };
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if unsafe {
                SetInformationJobObject(
                    job.0 .0,
                    JobObjectExtendedLimitInformation,
                    (&limits as *const JOBOBJECT_EXTENDED_LIMIT_INFORMATION).cast(),
                    std::mem::size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
                )
            } == 0
            {
                return Err(io::Error::last_os_error());
            }
            Ok(job)
        }
        fn terminate(&self) {
            unsafe {
                TerminateJobObject(self.0 .0, 1);
            }
        }
    }
    /// Start `command` suspended, put it in a job of its own and only then let it
    /// run, so that everything it starts is in the job too. Its own process group
    /// keeps a console Ctrl+C for the caller from reaching it.
    fn spawn_in_job(command: &mut Command) -> io::Result<(Child, Job)> {
        let job = Job::new()?;
        let mut child = command
            .creation_flags(CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP)
            .spawn()?;
        let started = if unsafe {
            AssignProcessToJobObject(job.0 .0, child.as_raw_handle() as HANDLE)
        } == 0
        {
            Err(io::Error::last_os_error())
        } else {
            resume(child.id())
        };
        if let Err(error) = started {
            let _ = child.kill();
            let _ = child.wait();
            return Err(error);
        }
        Ok((child, job))
    }
    /// Resume the threads of a process created suspended (it has only its first).
    fn resume(pid: u32) -> io::Result<()> {
        let handle = unsafe { CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0) };
        if handle == INVALID_HANDLE_VALUE {
            return Err(io::Error::last_os_error());
        }
        let handle = Owned(handle);
        let mut entry: THREADENTRY32 = unsafe { std::mem::zeroed() };
        entry.dwSize = std::mem::size_of::<THREADENTRY32>() as u32;
        let mut resumed = false;
        let mut more = unsafe { Thread32First(handle.0, &mut entry) } != 0;
        while more {
            if entry.th32OwnerProcessID == pid {
                let thread = unsafe { OpenThread(THREAD_SUSPEND_RESUME, 0, entry.th32ThreadID) };
                if thread.is_null() {
                    return Err(io::Error::last_os_error());
                }
                let thread = Owned(thread);
                if unsafe { ResumeThread(thread.0) } == u32::MAX {
                    return Err(io::Error::last_os_error());
                }
                resumed = true;
            }
            more = unsafe { Thread32Next(handle.0, &mut entry) } != 0;
        }
        if resumed {
            Ok(())
        } else {
            Err(io::Error::other("the new process has no thread to resume"))
        }
    }

    type Reader = thread::JoinHandle<io::Result<Vec<u8>>>;
    fn read_limited(pipe: Option<impl Read + Send + 'static>, limit: usize) -> Option<Reader> {
        let pipe = pipe?;
        Some(thread::spawn(move || {
            let mut bytes = Vec::new();
            pipe.take(limit as u64 + 1)
                .read_to_end(&mut bytes)
                .map(|_| bytes)
        }))
    }
    /// Run `command` without input in a job of its own (see `spawn_in_job`),
    /// collecting at most `limits` bytes of standard output and errors, until it
    /// exits and both streams end. When the output is too large, `deadline`
    /// passes, `cancelled` says so or the child fails, the whole job ends at
    /// once; whatever a successful child left running ends with the job when
    /// the call returns.
    pub fn run_bounded(
        command: &mut Command,
        limits: [usize; 2],
        deadline: Instant,
        cancelled: &dyn Fn() -> bool,
    ) -> std::result::Result<(ExitStatus, Vec<u8>, Vec<u8>), RunError> {
        command
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        let (mut child, job) = spawn_in_job(command).map_err(RunError::Spawn)?;
        let mut readers = [
            read_limited(child.stdout.take(), limits[0]),
            read_limited(child.stderr.take(), limits[1]),
        ];
        let mut outputs: [Option<Vec<u8>>; 2] = [None, None];
        let mut status = None;
        let result = loop {
            for (reader, output) in readers.iter_mut().zip(outputs.iter_mut()) {
                if reader.as_ref().is_some_and(|reader| reader.is_finished()) {
                    // A failed read leaves the stream unfinished, as on Unix: the
                    // run then ends at its deadline.
                    if let Ok(Ok(bytes)) = reader.take().unwrap().join() {
                        *output = Some(bytes);
                    }
                }
            }
            if outputs
                .iter()
                .zip(limits)
                .any(|(output, limit)| output.as_ref().is_some_and(|bytes| bytes.len() > limit))
            {
                break Err(RunError::TooLarge);
            }
            if status.is_none() {
                status = child.try_wait().map_err(RunError::Io)?;
            }
            if let Some(status) = status {
                if outputs.iter().all(Option::is_some) {
                    break Ok(status);
                }
            }
            if cancelled() || Instant::now() >= deadline {
                break Err(RunError::Stopped);
            }
            thread::sleep(Duration::from_millis(3));
        };
        if !result.as_ref().is_ok_and(ExitStatus::success) {
            job.terminate();
        }
        let _ = child.kill();
        let _ = child.wait();
        drop(job);
        let [out, err] = outputs;
        result.map(|status| (status, out.unwrap_or_default(), err.unwrap_or_default()))
    }
    /// `Command::output` with a time limit and closed input, in a job of its own
    /// (see `spawn_in_job`). Output goes to regular files, so a child writing much
    /// before it exits never blocks on a pipe and no reader thread outlives the
    /// call. The job ends at the limit, and with the call.
    pub fn output_until(
        command: &mut Command,
        duration: Duration,
    ) -> std::result::Result<Output, RunError> {
        let mut stdout = tempfile::tempfile().map_err(RunError::Io)?;
        let mut stderr = tempfile::tempfile().map_err(RunError::Io)?;
        command.stdout(Stdio::from(stdout.try_clone().map_err(RunError::Io)?));
        command.stderr(Stdio::from(stderr.try_clone().map_err(RunError::Io)?));
        command.stdin(Stdio::null());
        let (mut child, job) = spawn_in_job(command).map_err(RunError::Spawn)?;
        let deadline = Instant::now() + duration;
        let status = loop {
            if let Some(status) = child.try_wait().map_err(RunError::Io)? {
                break status;
            }
            if Instant::now() >= deadline {
                job.terminate();
                let _ = child.kill();
                let _ = child.wait();
                return Err(RunError::Stopped);
            }
            thread::sleep(Duration::from_millis(25));
        };
        drop(job);
        stdout.seek(SeekFrom::Start(0)).map_err(RunError::Io)?;
        stderr.seek(SeekFrom::Start(0)).map_err(RunError::Io)?;
        let mut output = Output {
            status,
            stdout: Vec::new(),
            stderr: Vec::new(),
        };
        stdout
            .read_to_end(&mut output.stdout)
            .map_err(RunError::Io)?;
        stderr
            .read_to_end(&mut output.stderr)
            .map_err(RunError::Io)?;
        Ok(output)
    }

    /// Keep Git for Windows from asking for credentials. A set but empty
    /// GIT_ASKPASS also overrides core.askpass and SSH_ASKPASS, Git Credential
    /// Manager must not open a window, and the ssh command runs in Git's own
    /// shell, whose `ssh` is Git's bundled client (System32 OpenSSH hangs on the
    /// synchronous pipes Git gives it). To be confirmed on a Windows computer.
    pub fn git_noninteractive(command: &mut Command) -> &mut Command {
        command
            .env("GIT_TERMINAL_PROMPT", "0")
            .env("GIT_ASKPASS", "")
            .env("SSH_ASKPASS", "")
            .env("GCM_INTERACTIVE", "never")
            .env(
                "GIT_SSH_COMMAND",
                "ssh -o BatchMode=yes -o ConnectTimeout=2",
            )
    }

    /// The modification time as Unix seconds and the nanoseconds within that second.
    pub fn modified(metadata: &fs::Metadata) -> (i64, i64) {
        unix_time(metadata.last_write_time())
    }
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

    #[test]
    fn verbatim_prefixes_go_only_when_the_plain_path_names_the_same_file() {
        let plain = |text: &str| {
            process::plain_path(PathBuf::from(text))
                .to_string_lossy()
                .into_owned()
        };
        assert_eq!(plain(r"\\?\C:\Users\a b\project"), r"C:\Users\a b\project");
        assert_eq!(plain(r"\\?\C:\"), r"C:\");
        assert_eq!(plain(r"\\?\UNC\server\share\dir"), r"\\server\share\dir");
        // Win32 would change or misread these, so they keep the prefix.
        for kept in [
            r"\\?\C:\dir\name.",
            r"\\?\C:\dir\name ",
            r"\\?\C:\NUL",
            r"\\?\C:\dir\com1.txt",
            r"\\?\C:\file:stream",
            r"\\?\Volume{0b7f}\dir",
            r"\\?\GLOBALROOT\Device\X",
        ] {
            assert_eq!(plain(kept), kept);
        }
        let long = format!(r"\\?\C:\{}", "a".repeat(300));
        assert_eq!(plain(&long), long);
        assert_eq!(plain(r"C:\already\plain"), r"C:\already\plain");
        let dir = tempfile::tempdir().unwrap();
        let path = canonical(dir.path()).unwrap();
        assert!(!path.to_string_lossy().starts_with(r"\\?\"), "{path:?}");
        assert_eq!(
            fs::canonicalize(&path).unwrap(),
            fs::canonicalize(dir.path()).unwrap()
        );
    }

    #[test]
    fn program_names_drop_runnable_extensions_and_case() {
        let name = |path: &str| program_name(Path::new(path));
        assert_eq!(
            name(r"C:\Program Files\nodejs\node.exe").as_deref(),
            Some("node")
        );
        assert_eq!(name("C:/npm/Codex.CMD").as_deref(), Some("codex"));
        // The default PATHEXT lists `.js` and `.vbs`; scripts keep their names.
        assert_eq!(name("codex.js").as_deref(), Some("codex.js"));
        assert_eq!(name("Setup.VBS").as_deref(), Some("setup.vbs"));
        assert_eq!(name("python3.12.exe").as_deref(), Some("python3.12"));
        assert_eq!(name(".exe").as_deref(), Some(".exe"));
    }

    #[test]
    fn crash_statuses_read_as_signals() {
        use std::os::windows::process::ExitStatusExt;
        let info = |raw: u32| ExitInfo::of(ExitStatus::from_raw(raw));
        assert_eq!(
            info(3),
            ExitInfo {
                code: Some(3),
                signal: None
            }
        );
        // exit(-1) is an ordinary failure, not a crash.
        assert_eq!(
            info(u32::MAX),
            ExitInfo {
                code: Some(-1),
                signal: None
            }
        );
        assert_eq!(
            info(0xC000_013A),
            ExitInfo {
                code: None,
                signal: Some(2)
            }
        );
        assert_eq!(info(0xC000_0005).signal, Some(11));
        assert_eq!(info(0xC000_0409).exit_code(), 134);
        // The --run fallback counts 128 and above as a crash, never as
        // "nothing to resume".
        assert_eq!(code(ExitStatus::from_raw(0xC000_013A)), 130);
        assert_eq!(code(ExitStatus::from_raw(1)), 1);
    }

    #[test]
    fn processes_have_identities_parents_and_images() {
        use std::time::{SystemTime, UNIX_EPOCH};
        let own = std::process::id();
        assert_eq!(presence(own), Presence::Present);
        assert_eq!(presence(0), Presence::Unknown);
        let start = process_start_time(own).unwrap();
        assert!(
            !start.is_empty() && start.bytes().all(|b| b.is_ascii_digit()),
            "{start}"
        );
        assert_eq!(process_start_fallback(own), None);
        let identity = process_identity(own).unwrap();
        assert_eq!(identity.birth, start);
        assert_eq!(identity.group, None);
        assert_eq!(identity.parent, parent_process_id());
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_secs_f64();
        assert!(identity.started > now - 86_400. && identity.started <= now + 1.);
        assert_eq!(process_table().unwrap()[&own].group, None);
        let image = describe(own).unwrap();
        assert_eq!(
            fs::canonicalize(&image.executable).unwrap(),
            fs::canonicalize(env::current_exe().unwrap()).unwrap()
        );
        assert_eq!(image.argv, Some(env::args().collect::<Vec<_>>()));

        let mut child = Command::new("cmd.exe")
            .args(["/c", "pause"])
            .stdin(Stdio::piped())
            .stdout(Stdio::null())
            .spawn()
            .unwrap();
        let (parent, name) = parent_and_name(child.id()).unwrap();
        let (argv_parent, argv) = parent_and_argv(child.id()).unwrap();
        drop(child.stdin.take());
        child.wait().unwrap();
        assert_eq!(parent, Some(own));
        assert_eq!(
            name.and_then(|name| program_name(Path::new(&name)))
                .as_deref(),
            Some("cmd")
        );
        assert_eq!(argv_parent, own);
        assert_eq!(argv[1..], ["/c", "pause"]);
        // `child` still holds the exited process's handle: it counts as gone.
        assert_eq!(process_start_time(child.id()).as_deref(), Some(""));
        assert_eq!(presence(child.id()), Presence::Gone);
        assert!(process_identity(child.id()).is_none());
        assert!(matches!(current_directories(), Err(error) if !error.is_empty()));
        assert!(matches!(open_files(own), Err(RunError::Unsupported)));
    }

    #[test]
    fn pinned_processes_end_only_their_identity() {
        let mut child = Command::new("ping")
            .args(["-n", "30", "127.0.0.1"])
            .stdout(Stdio::null())
            .spawn()
            .unwrap();
        let birth = process_identity(child.id()).unwrap().birth;
        assert!(PinnedProcess::pin(child.id(), "1").is_err());
        let pinned = PinnedProcess::pin(child.id(), &birth).unwrap();
        assert!(pinned.alive());
        // No polite request reaches an arbitrary process; the caller then kills.
        pinned.terminate().unwrap();
        assert!(pinned.alive());
        let error = request_exit(child.id()).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::Unsupported);
        assert!(pinned.alive());
        pinned.kill().unwrap();
        child.wait().unwrap();
        assert!(!pinned.alive());
        pinned.kill().unwrap();
    }

    #[test]
    fn restart_manager_names_the_holder_of_a_file() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("rollout.jsonl");
        let other = dir.path().join("other.jsonl");
        fs::write(&path, b"{}\n").unwrap();
        fs::write(&other, b"{}\n").unwrap();
        // The child holds the file as its standard input while it runs.
        let mut child = Command::new("ping")
            .args(["-n", "30", "127.0.0.1"])
            .stdin(fs::File::open(&path).unwrap())
            .stdout(Stdio::null())
            .spawn()
            .unwrap();
        let held = holds_open(child.id(), &path);
        let unrelated = holds_open(child.id(), &other);
        child.kill().unwrap();
        child.wait().unwrap();
        assert!(held.unwrap());
        assert!(!unrelated.unwrap());
    }

    #[test]
    fn bounded_runs_end_their_whole_job() {
        let never = || false;
        let far = Instant::now() + Duration::from_secs(30);
        let (status, out, err) = run_bounded(
            Command::new("cmd.exe").raw_arg("/c echo out& echo err 1>&2& exit 3"),
            [64, 64],
            far,
            &never,
        )
        .unwrap();
        assert_eq!(status.code(), Some(3));
        assert_eq!(String::from_utf8_lossy(&out).trim(), "out");
        assert_eq!(String::from_utf8_lossy(&err).trim(), "err");
        assert!(matches!(
            run_bounded(
                Command::new("cmd.exe").raw_arg("/c echo 12345"),
                [4, 64],
                far,
                &never
            ),
            Err(RunError::TooLarge)
        ));
        assert!(matches!(
            run_bounded(
                &mut Command::new("hgs-no-such-program"),
                [4, 4],
                far,
                &never
            ),
            Err(RunError::Spawn(_))
        ));
        assert!(matches!(
            run_bounded(
                Command::new("ping").args(["-n", "30", "127.0.0.1"]),
                [4096, 4096],
                far,
                &|| true
            ),
            Err(RunError::Stopped)
        ));
        // A helper that outlives the shell holds the output open; the deadline
        // ends the whole job.
        let started = Instant::now();
        let result = run_bounded(
            Command::new("cmd.exe").raw_arg("/c start /b ping -n 60 127.0.0.1 & exit 0"),
            [8192, 8192],
            Instant::now() + Duration::from_millis(500),
            &never,
        );
        assert!(matches!(result, Err(RunError::Stopped)), "{result:?}");
        assert!(started.elapsed() < Duration::from_secs(20));

        let output = output_until(
            Command::new("cmd.exe").raw_arg("/c echo out"),
            Duration::from_secs(30),
        )
        .unwrap();
        assert!(output.status.success());
        assert_eq!(String::from_utf8_lossy(&output.stdout).trim(), "out");
        assert!(matches!(
            output_until(
                Command::new("ping").args(["-n", "30", "127.0.0.1"]),
                Duration::from_millis(200)
            ),
            Err(RunError::Stopped)
        ));
    }

    #[test]
    fn private_entries_need_their_kind_and_one_link() {
        let dir = tempfile::tempdir().unwrap();
        let folder = dir.path().join("store");
        private_dir(&folder).unwrap();
        let file = folder.join("data");
        fs::write(&file, b"x").unwrap();
        // The file inherits the folder's owner and ACL, so both answers agree.
        let private = verify_private(&folder, Private::Directory).unwrap();
        assert_eq!(verify_private(&file, Private::File).unwrap(), private);
        assert_eq!(
            verify_private_file(&fs::File::open(&file).unwrap()).unwrap(),
            private
        );
        assert!(!verify_private(&folder, Private::File).unwrap());
        assert!(!verify_private(&file, Private::Directory).unwrap());
        fs::hard_link(&file, folder.join("second")).unwrap();
        assert!(!verify_private(&file, Private::File).unwrap());
        assert!(verify_private(&folder.join("missing"), Private::File).is_err());
    }

    #[test]
    fn file_identities_carry_unix_time_with_nanoseconds() {
        use std::time::UNIX_EPOCH;
        assert_eq!(unix_time(UNIX_EPOCH_TICKS), (0, 0));
        assert_eq!(unix_time(UNIX_EPOCH_TICKS + 15_000_001), (1, 500_000_100));
        assert_eq!(unix_time(UNIX_EPOCH_TICKS - 1), (-1, 999_999_900));
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("file");
        fs::write(&path, b"x").unwrap();
        let (identity, metadata) = FileIdentity::at(&path).unwrap();
        assert_eq!(file_identity(&path), Some(identity));
        assert_eq!(
            (identity.modified, identity.modified_nanos),
            modified(&metadata)
        );
        let since = metadata
            .modified()
            .unwrap()
            .duration_since(UNIX_EPOCH)
            .unwrap();
        assert_eq!(identity.modified, since.as_secs() as i64);
        assert_eq!(identity.modified_nanos, i64::from(since.subsec_nanos()));
    }

    #[test]
    fn supervisor_console_events_record_stop_requests() {
        let guard = ShutdownGuard::install().unwrap();
        let event = |kind| unsafe { process::shutdown_event(kind) };
        // Ctrl+C belongs to the agent; logoff and shutdown name no session.
        for kind in [CTRL_C_EVENT, CTRL_LOGOFF_EVENT, CTRL_SHUTDOWN_EVENT] {
            assert_eq!(event(kind), 1);
        }
        assert_eq!(shutdown_signal(), 0);
        assert_eq!(event(CTRL_BREAK_EVENT), 1);
        assert_eq!(shutdown_signal(), 15);
        drop(guard);
        // Once the outcome is recorded, a closing console waits no longer.
        let started = Instant::now();
        assert_eq!(event(CTRL_CLOSE_EVENT), 1);
        assert!(started.elapsed() < Duration::from_secs(1));
        assert_eq!(shutdown_signal(), 1);
    }

    #[test]
    fn git_never_prompts_on_windows() {
        let mut git = Command::new("git");
        git_noninteractive(&mut git);
        let environment: Vec<_> = git
            .get_envs()
            .map(|(key, value)| (key.to_str().unwrap(), value.and_then(|v| v.to_str())))
            .collect();
        assert_eq!(
            environment,
            [
                ("GCM_INTERACTIVE", Some("never")),
                ("GIT_ASKPASS", Some("")),
                (
                    "GIT_SSH_COMMAND",
                    Some("ssh -o BatchMode=yes -o ConnectTimeout=2")
                ),
                ("GIT_TERMINAL_PROMPT", Some("0")),
                ("SSH_ASKPASS", Some("")),
            ]
        );
    }
}
