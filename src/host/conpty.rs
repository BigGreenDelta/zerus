//! Pseudoconsoles for the session host (D6). The bundled `conpty.dll` and its
//! matched `OpenConsole.exe`, staged next to the host image, come first; the
//! inbox API in kernel32 is the degraded fallback. Neither is imported
//! statically: a static import would stop `hgs.exe` loading where the API is
//! missing.
//!
//! The pinned NuGet package `Microsoft.Windows.Console.ConPTY` 1.25.260930003
//! exports `ConptyCreatePseudoConsole`, `ConptyResizePseudoConsole`,
//! `ConptyClosePseudoConsole` and `ConptyReleasePseudoConsole` (plus the plain
//! names, `ConptyClearPseudoConsole`, `ConptyShowHidePseudoConsole`,
//! `ConptyReparentPseudoConsole`, `ConptyPackPseudoConsole` and
//! `ConptyCreatePseudoConsoleAsUser`).
//!
//! Output must be read continuously: ConPTY stops when its pipe is full, and
//! closing waits until the output is drained before Windows 11 24H2. Without
//! `ReleasePseudoConsole` (inbox before 24H2) the output pipe does not end when
//! the session's processes exit, only when the console is closed, so a session's
//! end is detected by waiting on its supervisor's process handle.
use super::spawn::{self, Child, Command};
use std::{
    fmt,
    fs::File,
    io, mem,
    os::windows::{
        ffi::OsStrExt,
        io::{AsRawHandle, FromRawHandle, OwnedHandle},
    },
    path::Path,
    ptr,
    sync::{mpsc, OnceLock},
    thread,
    time::Duration,
};
use windows_sys::{
    core::HRESULT,
    Win32::{
        Foundation::{FreeLibrary, HANDLE, HMODULE},
        System::{
            Console::{COORD, HPCON},
            Diagnostics::Debug::{
                SetThreadErrorMode, SEM_FAILCRITICALERRORS, SEM_NOOPENFILEERRORBOX,
            },
            LibraryLoader::{
                GetModuleHandleW, GetProcAddress, LoadLibraryExW, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR,
                LOAD_LIBRARY_SEARCH_SYSTEM32,
            },
            Pipes::CreatePipe,
        },
    },
};

/// The bundled pair, staged in the host image's folder (D2).
const BUNDLED_DLL: &str = "conpty.dll";
const BUNDLED_SERVER: &str = "OpenConsole.exe";

type Symbol = unsafe extern "system" fn() -> isize;
type CreateFn = unsafe extern "system" fn(COORD, HANDLE, HANDLE, u32, *mut HPCON) -> HRESULT;
type ResizeFn = unsafe extern "system" fn(HPCON, COORD) -> HRESULT;
type CloseFn = unsafe extern "system" fn(HPCON);
type ReleaseFn = unsafe extern "system" fn(HPCON) -> HRESULT;

/// Which implementation serves pseudoconsoles; the hello and `local_host` report it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Source {
    Bundled,
    Inbox,
}

impl Source {
    pub(crate) fn name(self) -> &'static str {
        match self {
            Self::Bundled => "bundled",
            Self::Inbox => "inbox",
        }
    }
}

/// Why the bundled pair is not used.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Fallback {
    /// Neither file is in the folder.
    Absent,
    /// Only one of the two files is there.
    Incomplete,
    /// `conpty.dll` does not load, with this system error.
    Load(i32),
    /// `conpty.dll` lacks this export.
    Export(&'static str),
}

impl fmt::Display for Fallback {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Absent => write!(f, "no bundled {BUNDLED_DLL} and {BUNDLED_SERVER}"),
            Self::Incomplete => {
                write!(f, "{BUNDLED_DLL} and {BUNDLED_SERVER} are not both present")
            }
            Self::Load(code) => write!(
                f,
                "the bundled {BUNDLED_DLL} does not load: {}",
                io::Error::from_raw_os_error(*code)
            ),
            Self::Export(name) => write!(f, "the bundled {BUNDLED_DLL} lacks {name}"),
        }
    }
}

/// The pseudoconsole functions of one implementation.
#[derive(Clone, Copy, Debug)]
pub(crate) struct Api {
    source: Source,
    fallback: Option<Fallback>,
    create: CreateFn,
    resize: ResizeFn,
    close: CloseFn,
    release: Option<ReleaseFn>,
}

impl Api {
    /// The API for this process, loaded once: the bundled pair from the running
    /// image's folder when it is there and loads, else the inbox API. A loaded DLL
    /// is never unloaded, since pseudoconsoles and their closing threads use it
    /// until the process ends.
    pub(crate) fn get() -> io::Result<Self> {
        static API: OnceLock<Result<Api, (io::ErrorKind, String)>> = OnceLock::new();
        API.get_or_init(|| {
            let image =
                std::env::current_exe().map_err(|error| (error.kind(), error.to_string()))?;
            Self::load(image.parent()).map_err(|error| (error.kind(), error.to_string()))
        })
        .clone()
        .map_err(|(kind, message)| io::Error::new(kind, message))
    }

    /// The bundled pair from `folder` when it holds both files and the DLL loads
    /// with every function needed, else the inbox API. Fails only when neither is
    /// available (Windows before 10 1809).
    pub(crate) fn load(folder: Option<&Path>) -> io::Result<Self> {
        let fallback = match folder.map(bundled) {
            Some(Ok(api)) => return Ok(api),
            Some(Err(fallback)) => fallback,
            None => Fallback::Absent,
        };
        inbox(fallback)
    }

    pub(crate) fn source(&self) -> Source {
        self.source
    }

    /// Why the bundled pair was passed over, for the inbox API.
    pub(crate) fn fallback(&self) -> Option<Fallback> {
        self.fallback
    }

    /// Whether `PseudoConsole::release` can let the output end with the session.
    pub(crate) fn can_release(&self) -> bool {
        self.release.is_some()
    }
}

fn bundled(folder: &Path) -> Result<Api, Fallback> {
    let dll = folder.join(BUNDLED_DLL);
    match (dll.is_file(), folder.join(BUNDLED_SERVER).is_file()) {
        (true, true) => {}
        (false, false) => return Err(Fallback::Absent),
        _ => return Err(Fallback::Incomplete),
    }
    let module =
        load_library(&dll).map_err(|error| Fallback::Load(error.raw_os_error().unwrap_or(0)))?;
    let (create, resize, close) = match bundled_functions(module) {
        Ok(functions) => functions,
        Err(fallback) => {
            // SAFETY: the module was loaded above and nothing from it is kept.
            unsafe { FreeLibrary(module) };
            return Err(fallback);
        }
    };
    // SAFETY: conpty.h of the pinned package declares these exports with exactly
    // these signatures, and the module stays loaded for the process's life.
    Ok(unsafe {
        Api {
            source: Source::Bundled,
            fallback: None,
            create: mem::transmute::<Symbol, CreateFn>(create),
            resize: mem::transmute::<Symbol, ResizeFn>(resize),
            close: mem::transmute::<Symbol, CloseFn>(close),
            release: lookup(module, "ConptyReleasePseudoConsole")
                .map(|release| mem::transmute::<Symbol, ReleaseFn>(release)),
        }
    })
}

fn bundled_functions(module: HMODULE) -> Result<(Symbol, Symbol, Symbol), Fallback> {
    let required = |name: &'static str| lookup(module, name).ok_or(Fallback::Export(name));
    Ok((
        required("ConptyCreatePseudoConsole")?,
        required("ConptyResizePseudoConsole")?,
        required("ConptyClosePseudoConsole")?,
    ))
}

fn inbox(fallback: Fallback) -> io::Result<Api> {
    let name: Vec<u16> = "kernel32.dll".encode_utf16().chain(Some(0)).collect();
    // SAFETY: a NUL-terminated name; kernel32 is loaded in every process and its
    // module handle needs no release.
    let module = unsafe { GetModuleHandleW(name.as_ptr()) };
    if module.is_null() {
        return Err(io::Error::last_os_error());
    }
    let required = |name: &str| {
        lookup(module, name).ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::Unsupported,
                "this Windows has no pseudoconsole API (it needs Windows 10 1809 or later)",
            )
        })
    };
    let (create, resize, close) = (
        required("CreatePseudoConsole")?,
        required("ResizePseudoConsole")?,
        required("ClosePseudoConsole")?,
    );
    // SAFETY: consoleapi.h declares these kernel32 exports with exactly these
    // signatures (ReleasePseudoConsole since Windows 11 24H2).
    Ok(unsafe {
        Api {
            source: Source::Inbox,
            fallback: Some(fallback),
            create: mem::transmute::<Symbol, CreateFn>(create),
            resize: mem::transmute::<Symbol, ResizeFn>(resize),
            close: mem::transmute::<Symbol, CloseFn>(close),
            release: lookup(module, "ReleasePseudoConsole")
                .map(|release| mem::transmute::<Symbol, ReleaseFn>(release)),
        }
    })
}

/// Load the DLL at the absolute `path`, resolving its own dependencies only from
/// its folder and System32. The host has no one to answer a "Bad Image" or
/// missing-file box, so this thread shows none while it loads.
fn load_library(path: &Path) -> io::Result<HMODULE> {
    let mut wide: Vec<u16> = path.as_os_str().encode_wide().collect();
    if wide.contains(&0) {
        return Err(io::Error::from(io::ErrorKind::InvalidInput));
    }
    wide.push(0);
    let mut previous = 0;
    // SAFETY: only this thread's error mode changes, and it is restored below.
    unsafe {
        SetThreadErrorMode(
            SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX,
            &mut previous,
        )
    };
    // SAFETY: a NUL-terminated absolute path, as LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
    // requires, and no file handle.
    let module = unsafe {
        LoadLibraryExW(
            wide.as_ptr(),
            ptr::null_mut(),
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32,
        )
    };
    let error = io::Error::last_os_error();
    // SAFETY: restores the mode saved above; the old mode is not needed.
    unsafe { SetThreadErrorMode(previous, ptr::null_mut()) };
    if module.is_null() {
        Err(error)
    } else {
        Ok(module)
    }
}

fn lookup(module: HMODULE, name: &str) -> Option<Symbol> {
    let name: Vec<u8> = name.bytes().chain(Some(0)).collect();
    // SAFETY: `module` is loaded and `name` is a NUL-terminated ASCII name.
    unsafe { GetProcAddress(module, name.as_ptr()) }
}

/// An HRESULT as a result; HRESULT_FROM_WIN32 values give their system error.
fn checked(result: HRESULT) -> io::Result<()> {
    if result >= 0 {
        return Ok(());
    }
    let code = result as u32;
    if code & 0xFFFF_0000 == 0x8007_0000 {
        Err(io::Error::from_raw_os_error((code & 0xFFFF) as i32))
    } else {
        Err(io::Error::other(format!(
            "the pseudoconsole call failed with HRESULT {code:#010x}"
        )))
    }
}

/// A console size in cells.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Size {
    pub columns: u16,
    pub rows: u16,
}

impl Size {
    fn coord(self) -> io::Result<COORD> {
        let cells = |count: u16| i16::try_from(count).ok().filter(|count| *count > 0);
        match (cells(self.columns), cells(self.rows)) {
            (Some(columns), Some(rows)) => Ok(COORD {
                X: columns,
                Y: rows,
            }),
            _ => Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "a console needs 1 to 32767 columns and rows",
            )),
        }
    }
}

/// The host's ends of a pseudoconsole's pipes: synchronous anonymous pipes, as
/// ConPTY requires, which no child inherits.
#[derive(Debug)]
pub(crate) struct Pipes {
    /// Bytes for the console's input: keys, pastes and replies to queries.
    pub input: File,
    /// What the console renders, as VT sequences. Keep reading it until it ends,
    /// on a thread that never closes the console (see `PseudoConsole::close`).
    pub output: File,
}

/// A pseudoconsole. Dropping it closes it as `close` does, without waiting.
#[derive(Debug)]
pub(crate) struct PseudoConsole {
    api: Api,
    /// None once closed.
    handle: Option<HPCON>,
    /// The console's ends of the pipes, kept until a child has started on it.
    /// Then only the console holds them, so the output ends when it does.
    console_ends: Option<(OwnedHandle, OwnedHandle)>,
}

impl PseudoConsole {
    /// A pseudoconsole of `size` and the host's ends of its pipes. It never gets
    /// PSEUDOCONSOLE_INHERIT_CURSOR: there is no cursor to inherit, and ConPTY
    /// would ask the host where it is.
    pub(crate) fn new(api: Api, size: Size) -> io::Result<(Self, Pipes)> {
        let size = size.coord()?;
        let (console_input, input) = pipe()?;
        let (output, console_output) = pipe()?;
        let mut handle: HPCON = 0;
        // SAFETY: both of the console's ends are open synchronous pipe handles,
        // which ConPTY duplicates, and `handle` is writable.
        checked(unsafe {
            (api.create)(
                size,
                console_input.as_raw_handle(),
                console_output.as_raw_handle(),
                0,
                &mut handle,
            )
        })?;
        Ok((
            Self {
                api,
                handle: Some(handle),
                console_ends: Some((console_input, console_output)),
            },
            Pipes {
                input: File::from(input),
                output: File::from(output),
            },
        ))
    }

    /// Start `command` on this console. Its flags may not ask for another console
    /// (`no_window`). Once it has started, the console's own pipe ends here close.
    pub(crate) fn spawn(&mut self, command: &Command) -> io::Result<Child> {
        let handle = self.open()?;
        // SAFETY: `self` is borrowed for the whole call, so the pseudoconsole
        // stays open until CreateProcessW has returned.
        let child = unsafe { spawn::spawn_attached(command, handle) }?;
        self.console_ends = None;
        Ok(child)
    }

    pub(crate) fn resize(&self, size: Size) -> io::Result<()> {
        let size = size.coord()?;
        let handle = self.open()?;
        // SAFETY: the pseudoconsole is open.
        checked(unsafe { (self.api.resize)(handle, size) })
    }

    /// Let the console end with its last client, so that the output ends when the
    /// session's processes have exited (ReleasePseudoConsole: the bundled pair and
    /// Windows 11 24H2). Call it once the first process has started on the
    /// console, which holds a reference from then on. False when this API lacks
    /// it; then the output ends only when the console is closed. The console must
    /// still be closed.
    pub(crate) fn release(&self) -> io::Result<bool> {
        let Some(release) = self.api.release else {
            return Ok(false);
        };
        let handle = self.open()?;
        // SAFETY: the pseudoconsole is open.
        checked(unsafe { release(handle) })?;
        Ok(true)
    }

    /// Close the console on a thread of its own and return at once. Its clients
    /// get CTRL_CLOSE_EVENT. ClosePseudoConsole can wait until the output is
    /// drained, so it never runs on the thread reading `Pipes::output`, which
    /// keeps reading until the output ends. If no thread can start, the console
    /// stays open until the host exits rather than risk that deadlock.
    pub(crate) fn close(mut self) -> io::Result<Closing> {
        self.console_ends = None;
        match self.handle.take() {
            Some(handle) => close_on_thread(self.api.close, handle),
            None => Ok(Closing::done()),
        }
    }

    fn open(&self) -> io::Result<HPCON> {
        self.handle
            .ok_or_else(|| io::Error::other("the pseudoconsole is closed"))
    }
}

impl Drop for PseudoConsole {
    fn drop(&mut self) {
        // Never on this thread, which may be the one reading the output.
        if let Some(handle) = self.handle.take() {
            let _ = close_on_thread(self.api.close, handle);
        }
    }
}

/// A ClosePseudoConsole call running on a thread of its own.
#[derive(Debug)]
pub(crate) struct Closing(mpsc::Receiver<()>);

impl Closing {
    fn done() -> Self {
        let (done, closing) = mpsc::channel();
        let _ = done.send(());
        Self(closing)
    }

    /// Whether the call has returned, waiting at most `timeout`.
    pub(crate) fn wait(&self, timeout: Duration) -> bool {
        !matches!(
            self.0.recv_timeout(timeout),
            Err(mpsc::RecvTimeoutError::Timeout)
        )
    }
}

fn close_on_thread(close: CloseFn, handle: HPCON) -> io::Result<Closing> {
    let (done, closing) = mpsc::channel();
    thread::Builder::new()
        .name("hgs-conpty-close".into())
        .spawn(move || {
            // SAFETY: the pseudoconsole is open, and this is the only close of it:
            // its owner gave up the handle.
            unsafe { close(handle) };
            let _ = done.send(());
        })?;
    Ok(Closing(closing))
}

/// An anonymous pipe as (read end, write end), neither inheritable.
fn pipe() -> io::Result<(OwnedHandle, OwnedHandle)> {
    let (mut read, mut write) = (ptr::null_mut(), ptr::null_mut());
    // SAFETY: both out-pointers are writable; without security attributes the
    // handles are not inheritable.
    if unsafe { CreatePipe(&mut read, &mut write, ptr::null(), 0) } == 0 {
        return Err(io::Error::last_os_error());
    }
    // SAFETY: CreatePipe opened both handles, and only the returned values own them.
    Ok(unsafe {
        (
            OwnedHandle::from_raw_handle(read),
            OwnedHandle::from_raw_handle(write),
        )
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::host::{
        job::{Job, Limits},
        spawn::{Environment, Flags},
    };
    use std::{
        env, fs,
        io::Read,
        os::windows::io::AsHandle,
        path::PathBuf,
        sync::{
            atomic::{AtomicU32, Ordering},
            Arc, Mutex,
        },
        time::Instant,
    };

    const LIMIT: Duration = Duration::from_secs(30);
    const SIZE: Size = Size {
        columns: 120,
        rows: 30,
    };

    fn cmd() -> PathBuf {
        PathBuf::from(env::var_os("SystemRoot").expect("SystemRoot is set"))
            .join(r"System32\cmd.exe")
    }

    fn strings(argv: &[&str]) -> Vec<String> {
        argv.iter().map(|argument| argument.to_string()).collect()
    }

    /// A marker no other test or earlier run prints.
    fn unique(label: &str) -> String {
        static NEXT: AtomicU32 = AtomicU32::new(0);
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_or(0, |time| time.subsec_nanos());
        format!(
            "hgs-{label}-{}-{}-{nanos}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        )
    }

    /// Everything the console writes, read on a thread of its own until it ends.
    struct Reader {
        bytes: Arc<Mutex<Vec<u8>>>,
        thread: thread::JoinHandle<()>,
    }

    impl Reader {
        fn start(mut output: File) -> Self {
            let bytes = Arc::new(Mutex::new(Vec::new()));
            let shared = Arc::clone(&bytes);
            let thread = thread::spawn(move || {
                let mut buffer = [0; 4096];
                while let Ok(count @ 1..) = output.read(&mut buffer) {
                    shared.lock().unwrap().extend_from_slice(&buffer[..count]);
                }
            });
            Self { bytes, thread }
        }

        fn text(&self) -> String {
            String::from_utf8_lossy(&self.bytes.lock().unwrap()).into_owned()
        }

        fn wait_for(&self, needle: &str) -> bool {
            eventually(|| self.text().contains(needle))
        }

        fn wait_end(&self) -> bool {
            eventually(|| self.thread.is_finished())
        }
    }

    /// Whether `done` holds within `LIMIT`.
    fn eventually(done: impl Fn() -> bool) -> bool {
        let deadline = Instant::now() + LIMIT;
        while !done() {
            if Instant::now() >= deadline {
                return false;
            }
            thread::sleep(Duration::from_millis(10));
        }
        true
    }

    fn on_console<'a>(
        argv: &'a [String],
        environment: &'a Environment,
        application: &'a Path,
    ) -> Command<'a> {
        Command {
            application,
            argv,
            environment,
            directory: None,
            flags: Flags {
                new_process_group: true,
                ..Flags::default()
            },
        }
    }

    #[test]
    fn the_bundled_pair_comes_first_and_inbox_is_the_fallback() {
        let dir = tempfile::tempdir().unwrap();
        let api = Api::load(Some(dir.path())).unwrap();
        assert_eq!(api.source(), Source::Inbox);
        assert_eq!(api.fallback(), Some(Fallback::Absent));
        // Half a pair is not used; the file is never loaded.
        fs::write(dir.path().join(BUNDLED_SERVER), b"").unwrap();
        assert_eq!(
            Api::load(Some(dir.path())).unwrap().fallback(),
            Some(Fallback::Incomplete)
        );
        assert_eq!(Api::load(None).unwrap().fallback(), Some(Fallback::Absent));

        let api = Api::get().unwrap();
        let folder = env::current_exe().unwrap().parent().unwrap().to_owned();
        let staged = folder.join(BUNDLED_DLL).is_file() && folder.join(BUNDLED_SERVER).is_file();
        assert_eq!(api.source() == Source::Bundled, staged);
        assert_eq!(
            api.source().name(),
            if staged { "bundled" } else { "inbox" }
        );
        assert_eq!(
            Fallback::Incomplete.to_string(),
            "conpty.dll and OpenConsole.exe are not both present"
        );
    }

    #[test]
    fn output_reaches_the_host_and_the_console_closes_after_its_child() {
        let api = Api::get().unwrap();
        assert!(PseudoConsole::new(
            api,
            Size {
                columns: 0,
                rows: 30
            }
        )
        .is_err());
        let (mut console, Pipes { input, output }) = PseudoConsole::new(api, SIZE).unwrap();
        let reader = Reader::start(output);
        console
            .resize(Size {
                columns: 100,
                rows: 40,
            })
            .unwrap();
        let environment = Environment::current().unwrap();
        let application = cmd();
        let refused = strings(&["cmd.exe", "/d", "/c", "exit"]);
        let hidden = Command {
            flags: Flags {
                no_window: true,
                ..Flags::default()
            },
            ..on_console(&refused, &environment, &application)
        };
        assert_eq!(
            console.spawn(&hidden).unwrap_err().kind(),
            io::ErrorKind::InvalidInput
        );

        let marker = unique("echo");
        let argv = strings(&["cmd.exe", "/d", "/c", "echo", &marker]);
        let child = console
            .spawn(&on_console(&argv, &environment, &application))
            .unwrap();
        let released = console.release().unwrap();
        assert_eq!(released, api.can_release());
        assert!(reader.wait_for(&marker), "{}", reader.text());
        assert_eq!(child.wait(LIMIT).unwrap(), Some(0));
        if released {
            // The console ended with its last client: the output ends unclosed.
            assert!(reader.wait_end(), "{}", reader.text());
        }
        // Closing after the child has exited returns promptly while the reader
        // drains whatever is left.
        let closing = console.close().unwrap();
        assert!(closing.wait(LIMIT));
        assert!(reader.wait_end());
        drop(input);
    }

    #[test]
    fn the_environment_block_reaches_the_child() {
        let api = Api::get().unwrap();
        let (mut console, Pipes { input, output }) = PseudoConsole::new(api, SIZE).unwrap();
        let reader = Reader::start(output);
        let value = unique("env");
        let mut environment = Environment::current().unwrap();
        environment.set("zerus_test_var", "stale").unwrap();
        environment.set("ZERUS_TEST_VAR", &value).unwrap();
        environment.set("ZERUS_TEST_GONE", "present").unwrap();
        environment.remove("zerus_test_gone").unwrap();
        let argv = strings(&[
            "cmd.exe",
            "/d",
            "/c",
            "echo",
            "%ZERUS_TEST_VAR%+%ZERUS_TEST_GONE%",
        ]);
        let application = cmd();
        let child = console
            .spawn(&on_console(&argv, &environment, &application))
            .unwrap();
        // cmd.exe leaves an undefined variable as it is written.
        let expected = format!("{value}+%ZERUS_TEST_GONE%");
        assert!(reader.wait_for(&expected), "{}", reader.text());
        assert_eq!(child.wait(LIMIT).unwrap(), Some(0));
        assert!(!reader.text().contains("stale"));
        assert!(console.close().unwrap().wait(LIMIT));
        assert!(reader.wait_end());
        drop(input);
    }

    #[test]
    fn a_suspended_child_joins_a_nested_session_job_before_it_runs() {
        let api = Api::get().unwrap();
        let (mut console, Pipes { input, output }) = PseudoConsole::new(api, SIZE).unwrap();
        let reader = Reader::start(output);
        // Stands in for the host's own job, which holds every session job (D3, D9).
        let host = Job::new(Limits {
            kill_on_close: true,
            ..Limits::default()
        })
        .unwrap();
        let session = Job::new(Limits::SESSION).unwrap();
        let environment = Environment::current().unwrap();
        let argv = strings(&["cmd.exe", "/d", "/c", "ping", "-n", "30", "127.0.0.1"]);
        let application = cmd();
        let child = console
            .spawn(&Command {
                flags: Flags {
                    suspended: true,
                    new_process_group: true,
                    ..Flags::default()
                },
                ..on_console(&argv, &environment, &application)
            })
            .unwrap();
        assert_eq!(child.wait(Duration::ZERO).unwrap(), None);
        host.assign(child.process.as_handle()).unwrap();
        session.adopt_suspended(&child).unwrap();
        for job in [&host, &session] {
            assert!(job.contains(child.process.as_handle()).unwrap());
            assert!(job.process_ids().unwrap().contains(&child.pid));
        }
        // What the child starts after it resumes is in the session job too.
        assert!(
            eventually(|| session.process_ids().unwrap().len() >= 2),
            "{:?}",
            session.process_ids()
        );
        console
            .resize(Size {
                columns: 80,
                rows: 24,
            })
            .unwrap();
        session.terminate(7).unwrap();
        assert_eq!(child.wait(LIMIT).unwrap(), Some(7));
        assert!(session.wait_empty(LIMIT).unwrap());
        assert!(host.wait_empty(LIMIT).unwrap());
        assert!(console.close().unwrap().wait(LIMIT));
        assert!(reader.wait_end());
        drop(input);
    }
}
