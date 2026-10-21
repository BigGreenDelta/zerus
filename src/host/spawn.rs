//! Starting processes for the session host (D4, D8). Stable Rust's
//! `std::process::Command` cannot attach a pseudoconsole, withhold every handle or
//! leave a child suspended, so the host calls `CreateProcessW` itself and builds the
//! environment block and command line that call takes. Those builders are portable
//! and tested everywhere; starting processes is Windows-only.
use std::{fmt, io, iter};

/// The longest command line `CreateProcessW` takes, in UTF-16 units without its NUL.
const COMMAND_LINE_LIMIT: usize = 32_766;

/// A Windows environment under construction: a client's whole block with the host's
/// overrides and removals (D8). Windows names are case-insensitive, so `Path` and
/// `PATH` are one variable. Drive entries such as `=C:`, which hold each drive's
/// current directory for relative paths like `D:notes.txt`, are kept; `cmd.exe`'s
/// other `=` entries (`=::`, `=ExitCode`) are its own state and are dropped.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub(crate) struct Environment {
    /// At most one variable per name, compared case-insensitively.
    variables: Vec<Variable>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct Variable {
    /// The name as Windows compares it (see `fold`).
    key: Vec<u16>,
    name: String,
    value: String,
}

/// Why an environment entry was refused. Messages name variables, never their values,
/// which may be secrets.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum EnvironmentError {
    /// The entry at this index of a block has no `NAME=` part.
    Malformed(usize),
    /// A name that is empty, holds NUL, or holds `=` other than a drive entry's
    /// leading one.
    Name(String),
    /// The value of this variable holds NUL.
    Value(String),
    /// The name or value of this variable is not Unicode.
    #[cfg(any(windows, test))]
    NotUnicode(String),
}

impl fmt::Display for EnvironmentError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Malformed(index) => write!(f, "environment entry {index} is not NAME=VALUE"),
            Self::Name(name) => write!(f, "invalid environment variable name {name:?}"),
            Self::Value(name) => write!(f, "the value of environment variable {name} contains NUL"),
            #[cfg(any(windows, test))]
            Self::NotUnicode(name) => write!(f, "environment variable {name:?} is not Unicode"),
        }
    }
}

impl std::error::Error for EnvironmentError {}

impl From<EnvironmentError> for io::Error {
    fn from(error: EnvironmentError) -> Self {
        io::Error::new(io::ErrorKind::InvalidInput, error)
    }
}

impl Environment {
    /// Variables from `NAME=VALUE` entries, such as a client's whole block. A name
    /// repeated in another case keeps its first value.
    pub(crate) fn parse<I, S>(entries: I) -> Result<Self, EnvironmentError>
    where
        I: IntoIterator<Item = S>,
        S: AsRef<str>,
    {
        let mut environment = Self::default();
        for (index, entry) in entries.into_iter().enumerate() {
            let entry = entry.as_ref();
            // A name may start with `=`; the separator is the first `=` after that.
            let Some(at) = entry
                .bytes()
                .skip(1)
                .position(|byte| byte == b'=')
                .map(|at| at + 1)
            else {
                return Err(EnvironmentError::Malformed(index));
            };
            let (name, value) = (&entry[..at], &entry[at + 1..]);
            if name.starts_with('=') && !drive_entry(name) {
                continue;
            }
            check(name, value)?;
            if environment.position(name).is_none() {
                environment.variables.push(Variable {
                    key: fold(name),
                    name: name.to_owned(),
                    value: value.to_owned(),
                });
            }
        }
        Ok(environment)
    }

    /// This process's own environment, which the launcher hands to the host (D4).
    #[cfg(windows)]
    pub(crate) fn current() -> Result<Self, EnvironmentError> {
        Self::from_os_entries(std::env::vars_os(), false)
    }

    /// A complete resolved hint snapshot will replace exactly these two
    /// optional values. Drop their raw entries before Unicode validation so a
    /// malformed optional value cannot reject the session's entire launch.
    #[cfg(windows)]
    pub(crate) fn current_for_hint_snapshot() -> Result<Self, EnvironmentError> {
        Self::from_os_entries(std::env::vars_os(), true)
    }

    /// Starting the persistent host also captures its caller's environment.
    /// Preserve valid raw controls, but normalize malformed optional controls
    /// to off without relaxing validation of any unrelated environment entry.
    #[cfg(windows)]
    pub(crate) fn current_with_optional_hint_controls() -> Result<Self, EnvironmentError> {
        let (environment, disabled) = Self::from_os_optional_hint_controls(std::env::vars_os())?;
        if disabled {
            eprintln!("{}", crate::config::HINT_POLICY_DIAGNOSTIC);
        }
        Ok(environment)
    }

    #[cfg(any(windows, test))]
    fn from_os_optional_hint_controls(
        raw: impl IntoIterator<Item = (std::ffi::OsString, std::ffi::OsString)>,
    ) -> Result<(Self, bool), EnvironmentError> {
        let raw = raw.into_iter().collect::<Vec<_>>();
        let disabled = raw.iter().any(|(name, value)| {
            name.to_str().is_some_and(hint_control)
                && value.to_str().is_none_or(|value| value.contains('\0'))
        });
        let mut environment = Self::from_os_entries(raw, disabled)?;
        if disabled {
            environment.set("HGS_AGENT_HINT", "0")?;
            environment.set("HGS_AGENT_HINT_OFF", "")?;
        }
        Ok((environment, disabled))
    }

    #[cfg(any(windows, test))]
    fn from_os_entries(
        raw: impl IntoIterator<Item = (std::ffi::OsString, std::ffi::OsString)>,
        replace_hint_controls: bool,
    ) -> Result<Self, EnvironmentError> {
        let mut entries = Vec::new();
        for (name, value) in raw {
            if replace_hint_controls && name.to_str().is_some_and(hint_control) {
                continue;
            }
            match (name.into_string(), value.into_string()) {
                (Ok(name), Ok(value)) => entries.push(format!("{name}={value}")),
                (name, _) => {
                    let name = name.unwrap_or_else(|name| name.to_string_lossy().into_owned());
                    return Err(EnvironmentError::NotUnicode(name));
                }
            }
        }
        Self::parse(entries)
    }
    pub(crate) fn get(&self, name: &str) -> Option<&str> {
        self.position(name)
            .map(|index| self.variables[index].value.as_str())
    }

    /// Set `name` to `value`. A variable that exists in another case keeps the
    /// spelling of its name (the system's `Path`, say); a new one takes this one.
    pub(crate) fn set(&mut self, name: &str, value: &str) -> Result<(), EnvironmentError> {
        check(name, value)?;
        match self.position(name) {
            Some(index) => self.variables[index].value = value.to_owned(),
            None => self.variables.push(Variable {
                key: fold(name),
                name: name.to_owned(),
                value: value.to_owned(),
            }),
        }
        Ok(())
    }

    /// Every variable as `NAME=VALUE`, in the order it was first given; `parse`
    /// reads them back unchanged.
    pub(crate) fn entries(&self) -> Vec<String> {
        self.variables
            .iter()
            .map(|variable| format!("{}={}", variable.name, variable.value))
            .collect()
    }

    /// Remove `name` in whatever case it is spelled.
    pub(crate) fn remove(&mut self, name: &str) -> Result<(), EnvironmentError> {
        check(name, "")?;
        if let Some(index) = self.position(name) {
            self.variables.remove(index);
        }
        Ok(())
    }

    /// The block `CreateProcessW` takes with `CREATE_UNICODE_ENVIRONMENT`: each
    /// `NAME=VALUE` in UTF-16 followed by NUL, then one more NUL (two NULs when
    /// empty). Windows wants the entries sorted by name, "case-insensitive, Unicode
    /// order, without regard to locale": names are compared as their upper-cased
    /// UTF-16 units, ordinally, as `CompareStringOrdinal` does when it ignores
    /// case. So `AB` sorts before `A_B` (`B` is below `_`), unlike a lower-cased
    /// comparison, and drive entries come before letters.
    pub(crate) fn block(&self) -> Vec<u16> {
        let mut sorted: Vec<&Variable> = self.variables.iter().collect();
        sorted.sort_by(|a, b| a.key.cmp(&b.key));
        let mut block = Vec::new();
        for variable in sorted {
            block.extend(variable.name.encode_utf16());
            block.push(u16::from(b'='));
            block.extend(variable.value.encode_utf16());
            block.push(0);
        }
        if block.is_empty() {
            block.push(0);
        }
        block.push(0);
        block
    }

    fn position(&self, name: &str) -> Option<usize> {
        let key = fold(name);
        self.variables
            .iter()
            .position(|variable| variable.key == key)
    }
}

#[cfg(any(windows, test))]
fn hint_control(name: &str) -> bool {
    name.eq_ignore_ascii_case("HGS_AGENT_HINT") || name.eq_ignore_ascii_case("HGS_AGENT_HINT_OFF")
}

/// `=X:`, where Windows keeps drive X's current directory for its programs.
fn drive_entry(name: &str) -> bool {
    matches!(name.as_bytes(), [b'=', drive, b':'] if drive.is_ascii_alphabetic())
}

fn check(name: &str, value: &str) -> Result<(), EnvironmentError> {
    if name.is_empty() || name.contains('\0') || (name.contains('=') && !drive_entry(name)) {
        return Err(EnvironmentError::Name(name.to_owned()));
    }
    if value.contains('\0') {
        return Err(EnvironmentError::Value(name.to_owned()));
    }
    Ok(())
}

/// A name as Windows compares names: UTF-16 units, each upper-cased on its own.
fn fold(name: &str) -> Vec<u16> {
    name.encode_utf16().map(upcase).collect()
}

/// One UTF-16 unit upper-cased by its simple mapping when that is a single unit,
/// otherwise unchanged (lone surrogates, and letters such as `ß` whose upper case
/// is longer). This matches the system's table for the letters names use.
fn upcase(unit: u16) -> u16 {
    let Some(character) = char::from_u32(u32::from(unit)) else {
        return unit;
    };
    let mut upper = character.to_uppercase();
    match (upper.next(), upper.next()) {
        (Some(upper), None) => u16::try_from(u32::from(upper)).unwrap_or(unit),
        _ => unit,
    }
}

/// Why an argv has no Windows command line.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum CommandLineError {
    /// There is no program name.
    Empty,
    /// The argument at this index contains NUL.
    Nul(usize),
    /// The program name contains `"`, which its parsing rules cannot carry.
    QuotedProgram,
    /// The line is longer than `CreateProcessW` accepts.
    TooLong,
}

impl fmt::Display for CommandLineError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Empty => f.write_str("a command line needs a program name"),
            Self::Nul(index) => write!(f, "argument {index} contains NUL"),
            Self::QuotedProgram => f.write_str("a program name cannot contain a double quote"),
            Self::TooLong => write!(
                f,
                "the command line is longer than {COMMAND_LINE_LIMIT} characters"
            ),
        }
    }
}

impl std::error::Error for CommandLineError {}

/// One argument as the MSVC C runtime and `CommandLineToArgvW` read it back. It is
/// quoted when it is empty or holds whitespace or `"`. Backslashes are literal
/// except before a quote: a literal `"` becomes `\"` after its backslashes are
/// doubled, and backslashes before the closing quote are doubled. Program names
/// follow other rules (see `command_line`).
pub(crate) fn quote_argument(argument: &str) -> String {
    if !argument.is_empty() && !argument.contains([' ', '\t', '\n', '\x0b', '"']) {
        return argument.to_owned();
    }
    let mut quoted = String::with_capacity(argument.len() + 2);
    quoted.push('"');
    let mut backslashes = 0;
    for character in argument.chars() {
        if character == '\\' {
            backslashes += 1;
            continue;
        }
        // 2n + 1 backslashes before a quote read as n backslashes and the quote.
        let run = if character == '"' {
            backslashes * 2 + 1
        } else {
            backslashes
        };
        quoted.extend(iter::repeat_n('\\', run));
        quoted.push(character);
        backslashes = 0;
    }
    // 2n backslashes before the closing quote read as n.
    quoted.extend(iter::repeat_n('\\', backslashes * 2));
    quoted.push('"');
    quoted
}

/// The command line for `argv`, program name first. The C runtime reads a program
/// name without backslash escapes, up to the next space or tab, or from a leading
/// quote to the next quote: it is quoted when needed, never escaped, and may not
/// contain `"`. The arguments follow, quoted by `quote_argument`.
pub(crate) fn command_line<S: AsRef<str>>(argv: &[S]) -> Result<String, CommandLineError> {
    let (program, arguments) = argv.split_first().ok_or(CommandLineError::Empty)?;
    if let Some(index) = argv
        .iter()
        .position(|argument| argument.as_ref().contains('\0'))
    {
        return Err(CommandLineError::Nul(index));
    }
    let program = program.as_ref();
    if program.contains('"') {
        return Err(CommandLineError::QuotedProgram);
    }
    let mut line = if program.is_empty() || program.contains([' ', '\t', '\n', '\x0b']) {
        format!("\"{program}\"")
    } else {
        program.to_owned()
    };
    for argument in arguments {
        line.push(' ');
        line.push_str(&quote_argument(argument.as_ref()));
    }
    if line.encode_utf16().count() > COMMAND_LINE_LIMIT {
        return Err(CommandLineError::TooLong);
    }
    Ok(line)
}

#[cfg(windows)]
pub(crate) use self::native::*;

#[cfg(windows)]
mod native {
    use super::{command_line, Environment};
    use crate::host::job;
    use std::{
        ffi::{c_void, OsStr},
        fmt, io, mem,
        os::windows::{
            ffi::OsStrExt,
            io::{AsRawHandle, FromRawHandle, OwnedHandle},
        },
        path::Path,
        ptr,
        time::Duration,
    };
    use windows_sys::Win32::{
        Foundation::{ERROR_ACCESS_DENIED, WAIT_OBJECT_0, WAIT_TIMEOUT},
        System::{
            Console::HPCON,
            Threading::{
                CreateProcessW, DeleteProcThreadAttributeList, GetExitCodeProcess,
                InitializeProcThreadAttributeList, ResumeThread, TerminateProcess,
                UpdateProcThreadAttribute, WaitForSingleObject, CREATE_BREAKAWAY_FROM_JOB,
                CREATE_NEW_PROCESS_GROUP, CREATE_NO_WINDOW, CREATE_SUSPENDED,
                CREATE_UNICODE_ENVIRONMENT, EXTENDED_STARTUPINFO_PRESENT,
                LPPROC_THREAD_ATTRIBUTE_LIST, PROCESS_INFORMATION,
                PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, STARTF_USESTDHANDLES, STARTUPINFOEXW,
                STARTUPINFOW,
            },
        },
    };

    /// How a process starts, beyond what every start does.
    #[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
    pub(crate) struct Flags {
        /// CREATE_SUSPENDED: its first thread waits for `Child::resume`, so the
        /// process can join a job before it runs (D9).
        pub suspended: bool,
        /// CREATE_NEW_PROCESS_GROUP: it leads a console process group of its own.
        pub new_process_group: bool,
        /// CREATE_NO_WINDOW: a console program gets a hidden console of its own.
        /// Required without a pseudoconsole, refused with one.
        pub no_window: bool,
        /// CREATE_BREAKAWAY_FROM_JOB: it starts outside this process's job, which
        /// must allow that (see `spawn_detached`).
        pub breakaway_from_job: bool,
    }

    /// A process to start.
    #[derive(Clone, Copy, Debug)]
    pub(crate) struct Command<'a> {
        /// The executable's absolute path. It is `lpApplicationName`, so no search
        /// path is ever consulted.
        pub application: &'a Path,
        /// The program name and arguments, as the child's C runtime will read them.
        pub argv: &'a [String],
        /// Its whole environment; nothing comes from this process's.
        pub environment: &'a Environment,
        /// Its absolute working directory; this process's when None.
        pub directory: Option<&'a Path>,
        pub flags: Flags,
    }

    /// A started process. Both handles close when this is dropped; the process
    /// keeps running.
    #[derive(Debug)]
    pub(crate) struct Child {
        pub process: OwnedHandle,
        /// Its first thread, which `resume` starts.
        pub thread: OwnedHandle,
        pub pid: u32,
    }

    impl Child {
        /// Let a process started suspended run.
        pub(crate) fn resume(&self) -> io::Result<()> {
            // SAFETY: `thread` is the open handle CreateProcessW returned, with
            // every access right including THREAD_SUSPEND_RESUME.
            if unsafe { ResumeThread(self.thread.as_raw_handle()) } == u32::MAX {
                return Err(io::Error::last_os_error());
            }
            Ok(())
        }

        /// Its exit code once it has exited, waiting at most `timeout`; None while
        /// it still runs.
        pub(crate) fn wait(&self, timeout: Duration) -> io::Result<Option<u32>> {
            let process = self.process.as_raw_handle();
            // SAFETY: `process` is an open process handle with SYNCHRONIZE access.
            match unsafe { WaitForSingleObject(process, milliseconds(timeout)) } {
                WAIT_OBJECT_0 => {
                    let mut code = 0;
                    // SAFETY: the same handle has PROCESS_QUERY_INFORMATION access,
                    // and `code` is writable.
                    if unsafe { GetExitCodeProcess(process, &mut code) } == 0 {
                        return Err(io::Error::last_os_error());
                    }
                    Ok(Some(code))
                }
                WAIT_TIMEOUT => Ok(None),
                _ => Err(io::Error::last_os_error()),
            }
        }

        /// End the process at once with `code`. Its children keep running unless
        /// a job ends them.
        pub(crate) fn terminate(&self, code: u32) -> io::Result<()> {
            // SAFETY: `process` is an open process handle with PROCESS_TERMINATE access.
            if unsafe { TerminateProcess(self.process.as_raw_handle(), code) } == 0 {
                return Err(io::Error::last_os_error());
            }
            Ok(())
        }
    }

    /// A timeout in milliseconds, short of INFINITE.
    fn milliseconds(timeout: Duration) -> u32 {
        u32::try_from(timeout.as_millis())
            .unwrap_or(u32::MAX)
            .min(u32::MAX - 1)
    }

    /// Start `command` with a hidden console of its own, which its flags must ask
    /// for (`no_window`): a child sharing this process's console would get this
    /// process's standard handles, which may be redirected. A new console gives a
    /// console program its own. The child receives no handle of this process
    /// (`bInheritHandles` is FALSE), and its environment is the command's own
    /// Unicode block.
    pub(crate) fn spawn(command: &Command) -> io::Result<Child> {
        if !command.flags.no_window {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "a process needs a console of its own",
            ));
        }
        create(command, None)
    }

    /// `spawn` attached to a pseudoconsole; `PseudoConsole::spawn` is the safe way
    /// to call it.
    ///
    /// # Safety
    /// `console` must be an open pseudoconsole until this returns:
    /// `CreateProcessW` reads it through the attribute list.
    pub(crate) unsafe fn spawn_attached(command: &Command, console: HPCON) -> io::Result<Child> {
        if command.flags.no_window {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "a process on a pseudoconsole gets no other console",
            ));
        }
        create(command, Some(console))
    }

    fn create(command: &Command, console: Option<HPCON>) -> io::Result<Child> {
        let invalid = |message: &str| io::Error::new(io::ErrorKind::InvalidInput, message);
        if !command.application.is_absolute() {
            return Err(invalid("the program must be an absolute path"));
        }
        if command
            .directory
            .is_some_and(|directory| !directory.is_absolute())
        {
            return Err(invalid("the working directory must be an absolute path"));
        }
        let application = wide(command.application.as_os_str())?;
        let mut line: Vec<u16> = command_line(command.argv)
            .map_err(|error| io::Error::new(io::ErrorKind::InvalidInput, error))?
            .encode_utf16()
            .chain(Some(0))
            .collect();
        let environment = command.environment.block();
        let directory = command
            .directory
            .map(|directory| wide(directory.as_os_str()))
            .transpose()?;

        let mut attributes = AttributeList::new(1)?;
        if let Some(console) = console {
            // SAFETY: the value of this attribute is the pseudoconsole handle
            // itself, which the caller keeps open until CreateProcessW has
            // returned; the list is dropped before this function returns.
            unsafe {
                attributes.set(
                    PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE as usize,
                    console as *const c_void,
                    mem::size_of::<HPCON>(),
                )?;
            }
        }
        let startup = STARTUPINFOEXW {
            StartupInfo: STARTUPINFOW {
                cb: mem::size_of::<STARTUPINFOEXW>() as u32,
                // On a pseudoconsole the standard handles stay NULL instead of
                // copies of this process's, which may be redirected; NULL ones
                // become the pseudoconsole's when the child attaches (as Windows
                // Terminal and node-pty start their shells).
                dwFlags: if console.is_some() {
                    STARTF_USESTDHANDLES
                } else {
                    0
                },
                ..STARTUPINFOW::default()
            },
            lpAttributeList: attributes.as_mut_ptr(),
        };
        let flags = command.flags;
        let creation = CREATE_UNICODE_ENVIRONMENT
            | EXTENDED_STARTUPINFO_PRESENT
            | [
                (flags.suspended, CREATE_SUSPENDED),
                (flags.new_process_group, CREATE_NEW_PROCESS_GROUP),
                (flags.no_window, CREATE_NO_WINDOW),
                (flags.breakaway_from_job, CREATE_BREAKAWAY_FROM_JOB),
            ]
            .into_iter()
            .filter(|(set, _)| *set)
            .fold(0, |all, (_, flag)| all | flag);
        let mut information = PROCESS_INFORMATION::default();
        // SAFETY: every pointer is valid for the call: the NUL-terminated
        // application path, the writable NUL-terminated command line, the
        // environment block ending in two NULs, the optional NUL-terminated
        // directory, and the extended startup information (a pointer to the whole
        // STARTUPINFOEXW, as EXTENDED_STARTUPINFO_PRESENT says) whose attribute
        // list lives in `attributes`. No handle is inherited.
        let created = unsafe {
            CreateProcessW(
                application.as_ptr(),
                line.as_mut_ptr(),
                ptr::null(),
                ptr::null(),
                0,
                creation,
                environment.as_ptr().cast(),
                directory
                    .as_ref()
                    .map_or(ptr::null(), |directory| directory.as_ptr()),
                (&startup as *const STARTUPINFOEXW).cast::<STARTUPINFOW>(),
                &mut information,
            )
        };
        if created == 0 {
            return Err(io::Error::last_os_error());
        }
        // SAFETY: CreateProcessW opened both handles for this process, and only
        // the returned Child owns them.
        Ok(unsafe {
            Child {
                process: OwnedHandle::from_raw_handle(information.hProcess),
                thread: OwnedHandle::from_raw_handle(information.hThread),
                pid: information.dwProcessId,
            }
        })
    }

    /// `text` in UTF-16 with a final NUL; an embedded NUL is refused.
    fn wide(text: &OsStr) -> io::Result<Vec<u16>> {
        let mut wide: Vec<u16> = text.encode_wide().collect();
        if wide.contains(&0) {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "a path contains NUL",
            ));
        }
        wide.push(0);
        Ok(wide)
    }

    /// A process and thread attribute list in memory of this process, deleted when
    /// dropped. The memory does not move while the list exists.
    struct AttributeList {
        /// usize elements keep the pointers the list stores aligned.
        buffer: Vec<usize>,
    }

    impl AttributeList {
        fn new(count: u32) -> io::Result<Self> {
            let mut size = 0;
            // SAFETY: a null list only asks for the size it needs; that call fails
            // with ERROR_INSUFFICIENT_BUFFER by design and writes only `size`.
            unsafe {
                InitializeProcThreadAttributeList(ptr::null_mut(), count, 0, &mut size);
            }
            if size == 0 {
                return Err(io::Error::last_os_error());
            }
            let mut buffer = vec![0usize; size.div_ceil(mem::size_of::<usize>())];
            // SAFETY: `buffer` holds at least `size` writable, aligned bytes.
            if unsafe {
                InitializeProcThreadAttributeList(buffer.as_mut_ptr().cast(), count, 0, &mut size)
            } == 0
            {
                return Err(io::Error::last_os_error());
            }
            Ok(Self { buffer })
        }

        fn as_mut_ptr(&mut self) -> LPPROC_THREAD_ATTRIBUTE_LIST {
            self.buffer.as_mut_ptr().cast()
        }

        /// # Safety
        /// The list keeps `value` itself, not a copy of what it points to: it must
        /// stay valid for `size` bytes while the list is used.
        unsafe fn set(
            &mut self,
            attribute: usize,
            value: *const c_void,
            size: usize,
        ) -> io::Result<()> {
            // SAFETY: the list was initialized in `new` and is not deleted while
            // `self` lives; the caller vouches for `value`.
            if UpdateProcThreadAttribute(
                self.as_mut_ptr(),
                0,
                attribute,
                value,
                size,
                ptr::null_mut(),
                ptr::null(),
            ) == 0
            {
                return Err(io::Error::last_os_error());
            }
            Ok(())
        }
    }

    impl Drop for AttributeList {
        fn drop(&mut self) {
            // SAFETY: the list was initialized in `new` and is deleted only here.
            unsafe { DeleteProcThreadAttributeList(self.as_mut_ptr()) }
        }
    }

    /// Why the host could not be started outside this process's job (D4).
    #[derive(Debug)]
    pub(crate) enum LaunchError {
        /// This process's job forbids breakaway, so whatever it starts would end
        /// with it.
        BreakawayForbidden,
        /// `CreateProcessW` failed for another reason.
        Spawn(io::Error),
    }

    impl fmt::Display for LaunchError {
        fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
            match self {
                Self::BreakawayForbidden => f.write_str(
                    "this process runs in a job that forbids breakaway, so a session host \
                     started from here would end with it",
                ),
                Self::Spawn(error) => write!(f, "cannot start the session host: {error}"),
            }
        }
    }

    impl std::error::Error for LaunchError {}

    /// D4: start `command` the way a client starts the host: outside this
    /// process's job (CREATE_BREAKAWAY_FROM_JOB), with a hidden console of its own
    /// (CREATE_NO_WINDOW), in its own process group, with a Unicode environment and
    /// no inherited handle. The command's own flags are replaced by these.
    ///
    /// A job that forbids breakaway makes `CreateProcessW` fail with
    /// ERROR_ACCESS_DENIED; that is reported as `BreakawayForbidden` and never
    /// retried without breakaway, which would tie the host's life to this
    /// process's job. Breakaway can also succeed and leave the process in a job
    /// further up; the host checks that itself (`job::join_host_job`).
    pub(crate) fn spawn_detached(command: &Command) -> Result<Child, LaunchError> {
        let command = Command {
            flags: Flags {
                suspended: false,
                new_process_group: true,
                no_window: true,
                breakaway_from_job: true,
            },
            ..*command
        };
        spawn(&command).map_err(|error| {
            let denied = error.raw_os_error() == Some(ERROR_ACCESS_DENIED as i32);
            if denied && matches!(job::current_job_allows_breakaway(), Ok(Some(false))) {
                LaunchError::BreakawayForbidden
            } else {
                LaunchError::Spawn(error)
            }
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn decode(block: &[u16]) -> Vec<String> {
        assert!(block.ends_with(&[0, 0]), "{block:?}");
        String::from_utf16(&block[..block.len() - 2])
            .unwrap()
            .split('\0')
            .filter(|entry| !entry.is_empty())
            .map(String::from)
            .collect()
    }

    #[test]
    fn environment_names_are_one_variable_in_any_case() {
        let mut environment = Environment::parse([
            r"Path=C:\Windows",
            r"=C:=C:\work",
            r"=::=::\",
            "=ExitCode=00000001",
            r"windir=C:\Windows",
            // A repeat in another case: the first value stays.
            r"PATH=C:\ignored",
            "EMPTY=",
            "a=b=c",
        ])
        .unwrap();
        assert_eq!(environment.get("PATH"), Some(r"C:\Windows"));
        assert_eq!(environment.get("A"), Some("b=c"));
        assert_eq!(environment.get("empty"), Some(""));
        assert_eq!(environment.get("=c:"), Some(r"C:\work"));
        assert_eq!(environment.get("=::"), None);
        assert_eq!(environment.get("=ExitCode"), None);

        // The existing spelling stays; the override's value wins.
        environment.set("PATH", r"C:\bin").unwrap();
        environment.set("TERM", "xterm-256color").unwrap();
        environment.remove("WINDIR").unwrap();
        environment.remove("ABSENT").unwrap();
        assert_eq!(
            decode(&environment.block()),
            [
                r"=C:=C:\work",
                "a=b=c",
                "EMPTY=",
                r"Path=C:\bin",
                "TERM=xterm-256color"
            ]
        );
        assert_eq!(Environment::default().block(), [0, 0]);
    }

    #[test]
    fn environment_blocks_sort_by_upper_cased_names() {
        let environment =
            Environment::parse(["a_b=1", "AB=2", "Ab1=3", "é=4", "F=5", "ß=6"]).unwrap();
        // `B` (0x42) is below `_` (0x5F) once upper-cased; `É` (0xC9) is above `F`.
        assert_eq!(
            decode(&environment.block()),
            ["AB=2", "Ab1=3", "a_b=1", "F=5", "é=4", "ß=6"]
        );
        assert_eq!(environment.get("É"), Some("4"));
        // `ß` has no single upper-case letter; it stays itself.
        assert_eq!(environment.get("SS"), None);
        assert_eq!(environment.get("ß"), Some("6"));
        assert_eq!(upcase(0xD800), 0xD800);
        assert_eq!(upcase(u16::from(b'z')), u16::from(b'Z'));
    }

    #[test]
    fn environment_entries_refuse_what_windows_cannot_hold() {
        let parse = |entries: &[&str]| Environment::parse(entries).unwrap_err();
        assert_eq!(parse(&["A=1", "NOEQUALS"]), EnvironmentError::Malformed(1));
        assert_eq!(parse(&[""]), EnvironmentError::Malformed(0));
        assert_eq!(parse(&["=C:"]), EnvironmentError::Malformed(0));
        assert_eq!(parse(&["A=x\0y"]), EnvironmentError::Value("A".into()));
        assert_eq!(parse(&["A\0B=1"]), EnvironmentError::Name("A\0B".into()));

        let mut environment = Environment::default();
        for name in ["", "A=B", "=X", "=CD:", "A\0"] {
            assert_eq!(
                environment.set(name, "x"),
                Err(EnvironmentError::Name(name.into()))
            );
            assert!(environment.remove(name).is_err());
        }
        assert_eq!(
            environment.set("SECRET", "x\0y"),
            Err(EnvironmentError::Value("SECRET".into()))
        );
        environment.set("=d:", r"D:\data").unwrap();
        assert_eq!(decode(&environment.block()), [r"=d:=D:\data"]);
        // Messages name the variable, never its value.
        let error = io::Error::from(EnvironmentError::Value("SECRET".into()));
        assert_eq!(error.kind(), io::ErrorKind::InvalidInput);
        assert!(!error.to_string().contains("x\0y"));
        assert_eq!(
            EnvironmentError::Malformed(3).to_string(),
            "environment entry 3 is not NAME=VALUE"
        );
    }

    #[test]
    fn agent_hint_raw_environment_replaces_only_exact_controls_before_unicode_validation() {
        use std::ffi::OsString;
        #[cfg(unix)]
        let malformed = {
            use std::os::unix::ffi::OsStringExt;
            OsString::from_vec(vec![0xff])
        };
        #[cfg(windows)]
        let malformed = {
            use std::os::windows::ffi::OsStringExt;
            OsString::from_wide(&[0xd800])
        };
        let entries = |key: &str| {
            vec![
                (OsString::from("KEEP"), OsString::from("unchanged")),
                (OsString::from(key), malformed.clone()),
            ]
        };
        for key in ["HGS_AGENT_HINT", "hgs_agent_hint_off"] {
            assert!(matches!(Environment::from_os_entries(entries(key), false),
                Err(EnvironmentError::NotUnicode(name)) if name == key));
            let normalized = Environment::from_os_entries(entries(key), true).unwrap();
            assert_eq!(normalized.get("KEEP"), Some("unchanged"));
            assert_eq!(normalized.get(key), None);
            let (bootstrap, disabled) =
                Environment::from_os_optional_hint_controls(entries(key)).unwrap();
            assert!(disabled);
            assert_eq!(bootstrap.get("HGS_AGENT_HINT"), Some("0"));
            assert_eq!(bootstrap.get("HGS_AGENT_HINT_OFF"), Some(""));
            assert_eq!(bootstrap.get("KEEP"), Some("unchanged"));
        }
        for key in ["UNRELATED", "HGS_AGENT_HINT_OTHER"] {
            assert!(matches!(Environment::from_os_entries(entries(key), true),
                Err(EnvironmentError::NotUnicode(name)) if name == key));
            assert!(
                matches!(Environment::from_os_optional_hint_controls(entries(key)),
                Err(EnvironmentError::NotUnicode(name)) if name == key)
            );
        }
        let (bootstrap, disabled) = Environment::from_os_optional_hint_controls([
            (OsString::from("HGS_AGENT_HINT"), OsString::from("0")),
            (OsString::from("HGS_AGENT_HINT_OFF"), OsString::from("work")),
        ])
        .unwrap();
        assert!(!disabled);
        assert_eq!(bootstrap.get("HGS_AGENT_HINT"), Some("0"));
        assert_eq!(bootstrap.get("HGS_AGENT_HINT_OFF"), Some("work"));
        assert!(matches!(
            Environment::from_os_entries([(malformed, OsString::from("unchanged"))], true),
            Err(EnvironmentError::NotUnicode(_))
        ));
        assert!(matches!(Environment::from_os_entries(
            [(OsString::from("UNRELATED"), OsString::from("bad\0value"))], true),
            Err(EnvironmentError::Value(name)) if name == "UNRELATED"));
    }
    /// argv as the MSVC C runtime (2008 and later) reads a command line.
    fn split(line: &str) -> Vec<String> {
        let chars: Vec<char> = line.chars().collect();
        let mut at = 0;
        let mut program = String::new();
        if chars.first() == Some(&'"') {
            at = 1;
            while at < chars.len() && chars[at] != '"' {
                program.push(chars[at]);
                at += 1;
            }
            at += 1;
        } else {
            while at < chars.len() && chars[at] != ' ' && chars[at] != '\t' {
                program.push(chars[at]);
                at += 1;
            }
        }
        let mut argv = vec![program];
        loop {
            while at < chars.len() && (chars[at] == ' ' || chars[at] == '\t') {
                at += 1;
            }
            if at >= chars.len() {
                return argv;
            }
            let mut argument = String::new();
            let mut quoted = false;
            while at < chars.len() {
                let character = chars[at];
                if !quoted && (character == ' ' || character == '\t') {
                    break;
                }
                if character == '\\' {
                    let run = chars[at..].iter().take_while(|&&c| c == '\\').count();
                    at += run;
                    if chars.get(at) == Some(&'"') {
                        argument.extend(iter::repeat_n('\\', run / 2));
                        if run % 2 == 1 {
                            argument.push('"');
                            at += 1;
                        }
                    } else {
                        argument.extend(iter::repeat_n('\\', run));
                    }
                } else if character == '"' {
                    if quoted && chars.get(at + 1) == Some(&'"') {
                        argument.push('"');
                        at += 2;
                    } else {
                        quoted = !quoted;
                        at += 1;
                    }
                } else {
                    argument.push(character);
                    at += 1;
                }
            }
            argv.push(argument);
        }
    }

    /// Every string up to `length` characters over the characters that matter.
    fn tricky(length: usize) -> Vec<String> {
        let mut all = vec![String::new()];
        let mut last = vec![String::new()];
        for _ in 0..length {
            last = last
                .iter()
                .flat_map(|prefix| ['a', ' ', '\t', '\\', '"'].map(|c| format!("{prefix}{c}")))
                .collect();
            all.extend(last.iter().cloned());
        }
        all
    }

    #[test]
    fn the_reference_parser_reads_the_documented_examples() {
        // From Microsoft's "Parsing C++ command-line arguments".
        for (line, expected) in [
            (r#"p "a b c" d e"#, &["a b c", "d", "e"][..]),
            (r#"p "ab\"c" "\\" d"#, &[r#"ab"c"#, r"\", "d"]),
            (r#"p a\\\b d"e f"g h"#, &[r"a\\\b", "de fg", "h"]),
            (r#"p a\\\"b c d"#, &[r#"a\"b"#, "c", "d"]),
            (r#"p a\\\\"b c" d e"#, &[r"a\\b c", "d", "e"]),
            (r#"p a"b"" c d"#, &[r#"ab" c d"#]),
        ] {
            assert_eq!(split(line)[1..], *expected, "{line}");
        }
        assert_eq!(
            split(r#""C:\Program Files\x.exe" y"#),
            [r"C:\Program Files\x.exe", "y"]
        );
    }

    #[test]
    fn arguments_are_quoted_for_the_c_runtime() {
        for (argument, quoted) in [
            ("", r#""""#),
            ("plain", "plain"),
            (r"C:\dir\", r"C:\dir\"),
            (r"\\server\share", r"\\server\share"),
            ("a b", r#""a b""#),
            ("a\tb", "\"a\tb\""),
            ("two\nlines", "\"two\nlines\""),
            (r#"ab"c"#, r#""ab\"c""#),
            (r"C:\Program Files\", r#""C:\Program Files\\""#),
            (r#"a\"b"#, r#""a\\\"b""#),
            (r"a\\b c", r#""a\\b c""#),
            (r#"\""#, r#""\\\"""#),
        ] {
            assert_eq!(quote_argument(argument), quoted, "{argument:?}");
        }
        assert_eq!(
            command_line(&[r"C:\Program Files\x.exe", "a b", "", r"C:\dir with space\"]).unwrap(),
            r#""C:\Program Files\x.exe" "a b" "" "C:\dir with space\\""#
        );
        assert_eq!(command_line(&["", "x"]).unwrap(), r#""" x"#);
        assert_eq!(command_line::<&str>(&[]), Err(CommandLineError::Empty));
        assert_eq!(
            command_line(&["a\"b"]),
            Err(CommandLineError::QuotedProgram)
        );
        assert_eq!(
            command_line(&["p", "a", "b\0"]),
            Err(CommandLineError::Nul(2))
        );
        let long = "x".repeat(COMMAND_LINE_LIMIT - 2);
        assert!(command_line(&["p", long.as_str()]).is_ok());
        assert_eq!(
            command_line(&["p", &format!("{long}x")]),
            Err(CommandLineError::TooLong)
        );
        assert_eq!(
            CommandLineError::Nul(2).to_string(),
            "argument 2 contains NUL"
        );
    }

    #[test]
    fn every_quoted_argument_reads_back_unchanged() {
        let all = tricky(4);
        for argument in &all {
            let line = command_line(&["prog", argument.as_str()]).unwrap();
            assert_eq!(split(&line), ["prog", argument.as_str()], "{line:?}");
        }
        let short = tricky(3);
        for first in &short {
            for second in &short {
                let argv = ["prog", first.as_str(), second.as_str()];
                assert_eq!(split(&command_line(&argv).unwrap()), argv);
            }
        }
        // Program names are never escaped.
        for program in ["C:\\dir\\", "a b\\", "tab\there", ""] {
            let line = command_line(&[program, "x"]).unwrap();
            assert_eq!(split(&line), [program, "x"], "{line:?}");
        }
    }

    #[cfg(windows)]
    mod windows {
        use super::super::*;
        use super::tricky;
        use crate::host::job;
        use std::{
            env,
            path::{Path, PathBuf},
            time::Duration,
        };
        use windows_sys::Win32::{Foundation::LocalFree, UI::Shell::CommandLineToArgvW};

        const LIMIT: Duration = Duration::from_secs(30);

        fn system32(program: &str) -> PathBuf {
            PathBuf::from(env::var_os("SystemRoot").expect("SystemRoot is set"))
                .join("System32")
                .join(program)
        }

        fn strings(argv: &[&str]) -> Vec<String> {
            argv.iter().map(|argument| argument.to_string()).collect()
        }

        /// argv as `CommandLineToArgvW` reads `line`.
        fn split_with_windows(line: &str) -> Vec<String> {
            let wide: Vec<u16> = line.encode_utf16().chain(Some(0)).collect();
            let mut count = 0;
            // SAFETY: `wide` is NUL-terminated; the result is freed below.
            let argv = unsafe { CommandLineToArgvW(wide.as_ptr(), &mut count) };
            assert!(!argv.is_null());
            let arguments = (0..usize::try_from(count).unwrap())
                .map(|index| {
                    // SAFETY: CommandLineToArgvW returned `count` NUL-terminated strings.
                    unsafe {
                        let argument = *argv.add(index);
                        let length = (0..).take_while(|&at| *argument.add(at) != 0).count();
                        String::from_utf16(std::slice::from_raw_parts(argument, length)).unwrap()
                    }
                })
                .collect();
            // SAFETY: the array came from CommandLineToArgvW and is freed once.
            unsafe { LocalFree(argv.cast()) };
            arguments
        }

        #[test]
        fn command_line_to_argv_reads_every_line_back() {
            for argument in tricky(4) {
                let line = command_line(&["prog", argument.as_str()]).unwrap();
                assert_eq!(
                    split_with_windows(&line),
                    ["prog", argument.as_str()],
                    "{line:?}"
                );
            }
            let line =
                command_line(&[r"C:\Program Files\x.exe", r"C:\dir with space\", ""]).unwrap();
            assert_eq!(
                split_with_windows(&line),
                [r"C:\Program Files\x.exe", r"C:\dir with space\", ""]
            );
        }

        fn command<'a>(
            application: &'a Path,
            argv: &'a [String],
            environment: &'a Environment,
            flags: Flags,
        ) -> Command<'a> {
            Command {
                application,
                argv,
                environment,
                directory: None,
                flags,
            }
        }

        #[test]
        fn suspended_processes_keep_their_exact_arguments_until_resumed() {
            let cmd = system32("cmd.exe");
            let environment = Environment::current().unwrap();
            let suspended = Flags {
                suspended: true,
                no_window: true,
                ..Flags::default()
            };
            // A suspended process already has its command line; it never runs here.
            let argv = strings(&[
                &cmd.to_string_lossy(),
                "a b",
                "",
                r"C:\dir with space\",
                r#"say "hi""#,
                r"back\\slash\",
            ]);
            let child = spawn(&command(&cmd, &argv, &environment, suspended)).unwrap();
            let image = crate::platform::describe(child.pid);
            assert_eq!(child.wait(Duration::ZERO).unwrap(), None);
            child.terminate(9).unwrap();
            assert_eq!(child.wait(LIMIT).unwrap(), Some(9));
            assert_eq!(image.and_then(|image| image.argv), Some(argv));

            let argv = strings(&["cmd.exe", "/d", "/c", "exit", "5"]);
            let child = spawn(&Command {
                directory: Some(&env::temp_dir()),
                ..command(&cmd, &argv, &environment, suspended)
            })
            .unwrap();
            assert_eq!(child.wait(Duration::from_millis(200)).unwrap(), None);
            child.resume().unwrap();
            assert_eq!(child.wait(LIMIT).unwrap(), Some(5));
        }

        #[test]
        fn spawning_refuses_shared_consoles_relative_paths_and_nul() {
            let environment = Environment::current().unwrap();
            let argv = strings(&["cmd.exe", "/d", "/c", "exit"]);
            let refused = |command: Command| spawn(&command).unwrap_err().to_string();
            let cmd = system32("cmd.exe");
            let hidden = Flags {
                no_window: true,
                ..Flags::default()
            };
            assert_eq!(
                refused(command(&cmd, &argv, &environment, Flags::default())),
                "a process needs a console of its own"
            );
            assert_eq!(
                refused(command(Path::new("cmd.exe"), &argv, &environment, hidden)),
                "the program must be an absolute path"
            );
            assert_eq!(
                refused(Command {
                    directory: Some(Path::new("relative")),
                    ..command(&cmd, &argv, &environment, hidden)
                }),
                "the working directory must be an absolute path"
            );
            let nul = strings(&["cmd.exe", "a\0b"]);
            assert_eq!(
                refused(command(&cmd, &nul, &environment, hidden)),
                "argument 1 contains NUL"
            );
        }

        #[test]
        fn the_launcher_breaks_away_or_names_the_job_that_forbids_it() {
            let cmd = system32("cmd.exe");
            let environment = Environment::current().unwrap();
            let argv = strings(&["cmd.exe", "/d", "/c", "exit", "3"]);
            // CI may run this inside a job of its own; either outcome is valid.
            match spawn_detached(&command(&cmd, &argv, &environment, Flags::default())) {
                Ok(child) => assert_eq!(child.wait(LIMIT).unwrap(), Some(3)),
                Err(LaunchError::BreakawayForbidden) => {
                    assert_eq!(job::current_job_allows_breakaway().unwrap(), Some(false));
                }
                Err(error) => panic!("{error}"),
            }
        }
    }
}
