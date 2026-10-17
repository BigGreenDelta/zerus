//! Job objects for the session host (D3, D4, D9). The host joins a kill-on-close
//! job of its own before its first session, so its death never orphans an agent.
//! Each session gets a job nested inside that one, without kill-on-close and
//! without breakaway, which its supervisor joins while still suspended.
//!
//! `platform::windows` uses this type too: for the CLI's own job (`Limits::CLI`)
//! and for the job of each child the CLI starts (`Limits::CHILD`).
use super::spawn::Child;
use std::{
    io, mem,
    os::windows::io::{AsHandle, AsRawHandle, BorrowedHandle, FromRawHandle, OwnedHandle},
    ptr, thread,
    time::{Duration, Instant},
};
use windows_sys::Win32::{
    Foundation::{ERROR_MORE_DATA, HANDLE},
    System::{
        JobObjects::{
            AssignProcessToJobObject, CreateJobObjectW, IsProcessInJob,
            JobObjectBasicProcessIdList, JobObjectExtendedLimitInformation,
            QueryInformationJobObject, SetInformationJobObject, TerminateJobObject,
            JOBOBJECT_BASIC_LIMIT_INFORMATION, JOBOBJECT_BASIC_PROCESS_ID_LIST,
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION, JOB_OBJECT_LIMIT_BREAKAWAY_OK,
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK,
        },
        Threading::GetCurrentProcess,
    },
    UI::WindowsAndMessaging::{GetShellWindow, GetWindowThreadProcessId},
};

/// What a job allows its processes.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct Limits {
    /// JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: closing the job's last handle ends every
    /// process in it and in the jobs nested inside it.
    pub kill_on_close: bool,
    /// JOB_OBJECT_LIMIT_BREAKAWAY_OK: a process may start a child outside the job
    /// with CREATE_BREAKAWAY_FROM_JOB.
    pub breakaway_ok: bool,
    /// JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK: every child starts outside the job.
    pub silent_breakaway_ok: bool,
}

impl Limits {
    /// The host's own job (D3): the host's death ends every session, and nothing
    /// in it can leave (Git for Windows' MSYS runtime breaks away whenever a job
    /// lets it).
    pub(crate) const HOST: Self = Self {
        kill_on_close: true,
        breakaway_ok: false,
        silent_breakaway_ok: false,
    };
    /// A session's job (D9), nested in the host's: ended on purpose, never left.
    pub(crate) const SESSION: Self = Self {
        kill_on_close: false,
        breakaway_ok: false,
        silent_breakaway_ok: false,
    };
    /// The CLI's own job (D4, `platform::prepare_environment`): what hgs.exe
    /// leaves behind ends with it, and a session host started with
    /// CREATE_BREAKAWAY_FROM_JOB leaves it.
    pub(crate) const CLI: Self = Self {
        kill_on_close: true,
        breakaway_ok: true,
        silent_breakaway_ok: false,
    };
    /// The job of one child the CLI starts (D4, `platform::spawn` and the
    /// helpers built on it), nested in the CLI's: it ends with the call that
    /// owns it, and since it forbids breakaway, nothing the child starts can
    /// leave through the CLI's permission.
    pub(crate) const CHILD: Self = Self {
        kill_on_close: true,
        breakaway_ok: false,
        silent_breakaway_ok: false,
    };

    fn flags(self) -> u32 {
        [
            (self.kill_on_close, JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE),
            (self.breakaway_ok, JOB_OBJECT_LIMIT_BREAKAWAY_OK),
            (
                self.silent_breakaway_ok,
                JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK,
            ),
        ]
        .into_iter()
        .filter(|(set, _)| *set)
        .fold(0, |all, (_, flag)| all | flag)
    }
}

/// A job object this process created. Its handle closes when this is dropped,
/// which ends its processes when it kills on close.
#[derive(Debug)]
pub(crate) struct Job(OwnedHandle);

impl Job {
    pub(crate) fn new(limits: Limits) -> io::Result<Self> {
        // SAFETY: no security attributes (the handle is not inheritable) and no name.
        let handle = unsafe { CreateJobObjectW(ptr::null(), ptr::null()) };
        if handle.is_null() {
            return Err(io::Error::last_os_error());
        }
        // SAFETY: CreateJobObjectW opened this handle and only this Job owns it.
        let job = Self(unsafe { OwnedHandle::from_raw_handle(handle) });
        job.set_limits(limits)?;
        Ok(job)
    }

    /// Replace the job's limits with `limits`.
    pub(crate) fn set_limits(&self, limits: Limits) -> io::Result<()> {
        let information = JOBOBJECT_EXTENDED_LIMIT_INFORMATION {
            BasicLimitInformation: JOBOBJECT_BASIC_LIMIT_INFORMATION {
                LimitFlags: limits.flags(),
                ..JOBOBJECT_BASIC_LIMIT_INFORMATION::default()
            },
            ..JOBOBJECT_EXTENDED_LIMIT_INFORMATION::default()
        };
        // SAFETY: the handle is an open job with all access, and the buffer is a
        // whole JOBOBJECT_EXTENDED_LIMIT_INFORMATION of the size given.
        if unsafe {
            SetInformationJobObject(
                self.handle(),
                JobObjectExtendedLimitInformation,
                (&information as *const JOBOBJECT_EXTENDED_LIMIT_INFORMATION).cast(),
                mem::size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
            )
        } == 0
        {
            return Err(io::Error::last_os_error());
        }
        Ok(())
    }

    /// Put `process` into the job. A process already in another job joins this
    /// one as a nested job; never assign a process to a job that already sits in
    /// that process's own job tree (D9: later assignments then fail).
    pub(crate) fn assign(&self, process: BorrowedHandle<'_>) -> io::Result<()> {
        // SAFETY: both handles are open; the process handle carries
        // PROCESS_SET_QUOTA and PROCESS_TERMINATE (callers pass handles from
        // CreateProcessW or the current-process pseudo-handle).
        if unsafe { AssignProcessToJobObject(self.handle(), process.as_raw_handle()) } == 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(())
    }

    /// Put a child started suspended into the job and only then let it run, so
    /// that everything it starts is in the job too (D9). A child that cannot join
    /// or resume is ended before it has run.
    pub(crate) fn adopt_suspended(&self, child: &Child) -> io::Result<()> {
        if let Err(error) = self
            .assign(child.process.as_handle())
            .and_then(|()| child.resume())
        {
            let _ = child.terminate(1);
            return Err(error);
        }
        Ok(())
    }

    /// Whether `process` is in this job or in a job nested inside it.
    pub(crate) fn contains(&self, process: BorrowedHandle<'_>) -> io::Result<bool> {
        let mut result = 0;
        // SAFETY: both handles are open with query access; `result` is writable.
        if unsafe { IsProcessInJob(process.as_raw_handle(), self.handle(), &mut result) } == 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(result != 0)
    }

    /// The IDs of the job's running processes, nested jobs included.
    pub(crate) fn process_ids(&self) -> io::Result<Vec<u32>> {
        process_ids(self.handle())
    }

    /// End every process in the job, nested jobs included, with `exit_code`.
    pub(crate) fn terminate(&self, exit_code: u32) -> io::Result<()> {
        // SAFETY: the handle is an open job with JOB_OBJECT_TERMINATE access.
        if unsafe { TerminateJobObject(self.handle(), exit_code) } == 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(())
    }

    /// Wait at most `timeout` until no process is left in the job; whether that
    /// happened. A job's own signal only reports time limits, so this polls.
    pub(crate) fn wait_empty(&self, timeout: Duration) -> io::Result<bool> {
        // A timeout past the clock's range (Duration::MAX) has no deadline
        // instead of panicking.
        let deadline = Instant::now().checked_add(timeout);
        loop {
            if self.process_ids()?.is_empty() {
                return Ok(true);
            }
            if deadline.is_some_and(|deadline| Instant::now() >= deadline) {
                return Ok(false);
            }
            thread::sleep(Duration::from_millis(10));
        }
    }

    fn handle(&self) -> HANDLE {
        self.0.as_raw_handle()
    }
}

/// The IDs of the running processes of job `job`, nested jobs included; a
/// null `job` names this process's immediate job.
fn process_ids(job: HANDLE) -> io::Result<Vec<u32>> {
    let word = mem::size_of::<usize>();
    // The header's two counts, then one ULONG_PTR per process.
    let header = mem::offset_of!(JOBOBJECT_BASIC_PROCESS_ID_LIST, ProcessIdList) / word;
    // Small at first, so that the growing path runs whenever a job holds more
    // than one process.
    let mut capacity = 1usize;
    for _ in 0..24 {
        // usize elements keep the list aligned for its ULONG_PTR entries.
        let mut buffer = vec![0usize; header + capacity];
        let bytes = u32::try_from(buffer.len() * word)
            .map_err(|_| io::Error::other("too many processes in a job"))?;
        // SAFETY: the handle is an open job with query access or null (this
        // process's job), and `buffer` is writable for `bytes` bytes and
        // aligned for the list.
        let queried = unsafe {
            QueryInformationJobObject(
                job,
                JobObjectBasicProcessIdList,
                buffer.as_mut_ptr().cast(),
                bytes,
                ptr::null_mut(),
            )
        } != 0;
        if !queried {
            let error = io::Error::last_os_error();
            if error.raw_os_error() != Some(ERROR_MORE_DATA as i32) {
                return Err(error);
            }
        }
        // SAFETY: the buffer is zeroed or holds the header the call filled in,
        // and it is at least as large and as aligned as
        // JOBOBJECT_BASIC_PROCESS_ID_LIST.
        let list = unsafe { &*buffer.as_ptr().cast::<JOBOBJECT_BASIC_PROCESS_ID_LIST>() };
        let (assigned, listed) = (
            list.NumberOfAssignedProcesses as usize,
            list.NumberOfProcessIdsInList as usize,
        );
        // Too small, or more processes joined meanwhile: ask again with room
        // for all of them.
        if !queried || listed < assigned {
            capacity = capacity.saturating_mul(2).max(assigned);
            continue;
        }
        return Ok(buffer[header..header + listed.min(capacity)]
            .iter()
            .map(|&pid| pid as u32)
            .collect());
    }
    Err(io::Error::other("the job's processes kept changing"))
}

/// The IDs of the running processes of this process's immediate job, nested
/// jobs included: for a supervisor, its session's job (D9, D10). None outside
/// any job.
pub(crate) fn own_job_process_ids() -> io::Result<Option<Vec<u32>>> {
    if !in_any_job()? {
        return Ok(None);
    }
    process_ids(ptr::null_mut()).map(Some)
}

/// Whether this process runs in any job (D4).
pub(crate) fn in_any_job() -> io::Result<bool> {
    let mut result = 0;
    // SAFETY: the current-process pseudo-handle is always valid, a null job asks
    // about any job, and `result` is writable.
    if unsafe { IsProcessInJob(GetCurrentProcess(), ptr::null_mut(), &mut result) } == 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(result != 0)
}

/// The process of this desktop's shell window, normally explorer.exe; None
/// without one, as in a service's or an SSH logon's window station.
fn shell_process_id() -> Option<u32> {
    // SAFETY: GetShellWindow takes no arguments and returns null without a shell.
    let window = unsafe { GetShellWindow() };
    if window.is_null() {
        return None;
    }
    let mut pid = 0;
    // SAFETY: `window` is a window handle (a stale one makes the call return 0)
    // and `pid` is writable.
    unsafe { GetWindowThreadProcessId(window, &mut pid) };
    (pid != 0).then_some(pid)
}

/// Whether this desktop's shell runs in this process's immediate job, nested
/// jobs included (D4). Some desktops run every process in a job that forbids
/// breakaway, the shell first; a host there ends no earlier than the desktop.
pub(crate) fn shell_in_own_job() -> io::Result<bool> {
    let Some(shell) = shell_process_id() else {
        return Ok(false);
    };
    Ok(own_job_process_ids()?.is_some_and(|ids| ids.contains(&shell)))
}

/// Whether this process's immediate job lets it start a child outside the job;
/// None outside any job (D4).
pub(crate) fn current_job_allows_breakaway() -> io::Result<Option<bool>> {
    if !in_any_job()? {
        return Ok(None);
    }
    let mut information = JOBOBJECT_EXTENDED_LIMIT_INFORMATION::default();
    // SAFETY: a null job names this process's immediate job, and the buffer is a
    // writable JOBOBJECT_EXTENDED_LIMIT_INFORMATION of the size given.
    if unsafe {
        QueryInformationJobObject(
            ptr::null_mut(),
            JobObjectExtendedLimitInformation,
            (&mut information as *mut JOBOBJECT_EXTENDED_LIMIT_INFORMATION).cast(),
            mem::size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
            ptr::null_mut(),
        )
    } == 0
    {
        return Err(io::Error::last_os_error());
    }
    let allowing = JOB_OBJECT_LIMIT_BREAKAWAY_OK | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
    Ok(Some(
        information.BasicLimitInformation.LimitFlags & allowing != 0,
    ))
}

/// The host's own job and where the host stood before it joined it.
#[derive(Debug)]
pub(crate) struct HostJob {
    /// Keep it for the host's whole life: its handle closes when the host exits
    /// and ends every session then. Dropping it earlier ends the host too.
    pub job: Job,
    /// The host already ran in a job: breakaway stopped below a job that forbids
    /// it. The host reports this in its hello, and the launcher refuses such a
    /// host (D4) unless `desktop_job`.
    pub was_in_job: bool,
    /// That job also holds the shell of this desktop: the host then ends no
    /// earlier than the desktop, whose processes all run in that job (D4).
    pub desktop_job: bool,
}

/// D3, D4: note whether this process (the host) already runs in a job, then put it
/// into a new kill-on-close job of its own, which nothing can leave. The host must
/// fail closed when this fails.
pub(crate) fn join_host_job() -> io::Result<HostJob> {
    let was_in_job = in_any_job()?;
    // Asked before the host's own job exists: the immediate job is still the one
    // breakaway stopped at.
    let desktop_job = was_in_job && shell_in_own_job()?;
    let job = Job::new(Limits::HOST)?;
    // SAFETY: GetCurrentProcess returns the current-process pseudo-handle, which
    // is always valid and which BorrowedHandle allows.
    job.assign(unsafe { BorrowedHandle::borrow_raw(GetCurrentProcess()) })?;
    Ok(HostJob {
        job,
        was_in_job,
        desktop_job,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::host::spawn::{spawn, spawn_detached, Command, Environment, Flags, LaunchError};
    use std::{env, fs, path::PathBuf};

    const LIMIT: Duration = Duration::from_secs(30);
    /// Where `host_job_child` reports, when a parent test runs it.
    const REPORT: &str = "HGS_TEST_HOST_JOB_REPORT";

    fn system32(program: &str) -> PathBuf {
        PathBuf::from(env::var_os("SystemRoot").expect("SystemRoot is set"))
            .join("System32")
            .join(program)
    }

    fn strings(argv: &[&str]) -> Vec<String> {
        argv.iter().map(|argument| argument.to_string()).collect()
    }

    /// A hidden `ping` that runs for about half a minute, started suspended.
    fn helper(environment: &Environment) -> Child {
        let ping = system32("PING.EXE");
        let argv = strings(&["ping", "-n", "30", "127.0.0.1"]);
        spawn(&Command {
            application: &ping,
            argv: &argv,
            environment,
            directory: None,
            flags: Flags {
                suspended: true,
                no_window: true,
                ..Flags::default()
            },
        })
        .unwrap()
    }

    #[test]
    fn session_jobs_list_and_end_their_processes() {
        let environment = Environment::current().unwrap();
        let job = Job::new(Limits::SESSION).unwrap();
        let child = helper(&environment);
        job.adopt_suspended(&child).unwrap();
        assert!(job.contains(child.process.as_handle()).unwrap());
        // The console host of its hidden console may be listed too.
        assert!(job.process_ids().unwrap().contains(&child.pid));
        assert!(!job.wait_empty(Duration::from_millis(50)).unwrap());
        job.terminate(7).unwrap();
        assert_eq!(child.wait(LIMIT).unwrap(), Some(7));
        assert!(job.wait_empty(LIMIT).unwrap());

        let other = Job::new(Limits::SESSION).unwrap();
        assert!(!other.contains(child.process.as_handle()).unwrap());
        assert!(other.process_ids().unwrap().is_empty());
        // Nothing in this test changes where the test process itself runs.
        assert_eq!(
            current_job_allows_breakaway().unwrap().is_some(),
            in_any_job().unwrap()
        );
    }

    #[test]
    fn nested_jobs_hold_their_children_and_grow_their_lists() {
        let environment = Environment::current().unwrap();
        // Stands in for the host's job, which holds every session job.
        let outer = Job::new(Limits {
            kill_on_close: true,
            ..Limits::default()
        })
        .unwrap();
        let inner = Job::new(Limits::SESSION).unwrap();
        let children: Vec<Child> = (0..3).map(|_| helper(&environment)).collect();
        for child in &children {
            outer.assign(child.process.as_handle()).unwrap();
            inner.adopt_suspended(child).unwrap();
        }
        // Console hosts of their hidden consoles may be listed too.
        for job in [&outer, &inner] {
            let listed = job.process_ids().unwrap();
            for child in &children {
                assert!(listed.contains(&child.pid), "{listed:?}");
            }
        }
        inner.terminate(3).unwrap();
        for child in &children {
            assert_eq!(child.wait(LIMIT).unwrap(), Some(3));
        }
        assert!(outer.wait_empty(LIMIT).unwrap());
    }

    /// Run by `the_host_job_ends_what_the_host_leaves_behind` in a test process
    /// of its own; it changes the job of the process it runs in.
    #[test]
    #[ignore = "run by the_host_job_ends_what_the_host_leaves_behind"]
    fn host_job_child() {
        let Some(report) = env::var_os(REPORT) else {
            return;
        };
        let note = |line: String| {
            let mut text = fs::read_to_string(&report).unwrap_or_default();
            text.push_str(&line);
            text.push('\n');
            fs::write(&report, text).unwrap();
        };
        let host = join_host_job().unwrap();
        note(format!("was_in_job={}", host.was_in_job));
        note(format!("in_job={}", in_any_job().unwrap()));
        let environment = Environment::current().unwrap();
        let cmd = system32("cmd.exe");
        let argv = strings(&["cmd.exe", "/d", "/c", "exit", "0"]);
        let launched = spawn_detached(&Command {
            application: &cmd,
            argv: &argv,
            environment: &environment,
            directory: None,
            flags: Flags::default(),
        });
        note(format!(
            "breakaway={}",
            match launched {
                Err(LaunchError::BreakawayForbidden) => "forbidden".to_owned(),
                Err(error) => error.to_string(),
                Ok(_) => "allowed".to_owned(),
            }
        ));
        // Left running on purpose: the host job must end it when this exits.
        let child = helper(&environment);
        child.resume().unwrap();
        note(format!(
            "helper_in_job={}",
            host.job.contains(child.process.as_handle()).unwrap()
        ));
        note("done".to_owned());
        // The job closes with this process, as the host's does.
        mem::forget(host);
    }

    #[test]
    fn the_host_job_ends_what_the_host_leaves_behind() {
        let dir = tempfile::tempdir().unwrap();
        let report = dir.path().join("report");
        let mut environment = Environment::current().unwrap();
        environment.set(REPORT, report.to_str().unwrap()).unwrap();
        let exe = env::current_exe().unwrap();
        let argv = strings(&[
            &exe.to_string_lossy(),
            "host::job::tests::host_job_child",
            "--exact",
            "--ignored",
            "--test-threads=1",
        ]);
        // Ends whatever the child leaves behind if this test fails half way.
        let cleanup = Job::new(Limits {
            kill_on_close: true,
            ..Limits::default()
        })
        .unwrap();
        let child = spawn(&Command {
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
        let code = child.wait(Duration::from_secs(120)).unwrap();
        let text = fs::read_to_string(&report).unwrap_or_default();
        assert_eq!(code, Some(0), "{text}");
        // It ran in `cleanup`, so it was in a job before joining its own; its
        // job forbids breakaway and holds the helper.
        assert_eq!(
            text.lines().collect::<Vec<_>>(),
            [
                "was_in_job=true",
                "in_job=true",
                "breakaway=forbidden",
                "helper_in_job=true",
                "done"
            ]
        );
        // The child exited and its job closed: the helper ended with it, long
        // before its half minute.
        assert!(cleanup.wait_empty(Duration::from_secs(10)).unwrap());
    }
}
