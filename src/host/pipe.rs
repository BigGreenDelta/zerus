//! Named pipes of the session host (spec D3, D5): a listener that always keeps an
//! instance open for the next client, client authentication by impersonation, and
//! a client connect that verifies the server before anything is sent.
//!
//! Every handle is overlapped and every read, write and connect waits on an event
//! of its own. Synchronous pipe handles serialize I/O per file object: a thread
//! blocked reading would hold up another thread's write, and full-duplex use would
//! deadlock (the OpenSSH relays met the same; see `overlapped_pipe` in
//! `platform::windows`).
use super::endpoint::pipe_sddl;
use super::protocol::{ErrorBody, ErrorCode, Integrity, ELEVATED_HOST};
use super::token::{Identity, Token};
use std::{
    fmt,
    io::{self, Read, Write},
    os::windows::io::{AsRawHandle, FromRawHandle, OwnedHandle},
    sync::{
        atomic::{AtomicU64, Ordering},
        Arc, Mutex, PoisonError,
    },
    time::{Duration, Instant},
};
use windows_sys::{
    core::BOOL,
    Win32::{
        Foundation::{
            GetLastError, LocalFree, ERROR_ACCESS_DENIED, ERROR_BROKEN_PIPE, ERROR_FILE_NOT_FOUND,
            ERROR_IO_PENDING, ERROR_NO_DATA, ERROR_OPERATION_ABORTED, ERROR_PIPE_BUSY,
            ERROR_PIPE_CONNECTED, ERROR_PIPE_NOT_CONNECTED, ERROR_SEM_TIMEOUT, GENERIC_READ,
            GENERIC_WRITE, HANDLE, INVALID_HANDLE_VALUE, WAIT_FAILED, WAIT_OBJECT_0, WAIT_TIMEOUT,
        },
        Security::{
            Authorization::{
                ConvertStringSecurityDescriptorToSecurityDescriptorW, SDDL_REVISION_1,
            },
            RevertToSelf, PSECURITY_DESCRIPTOR, SECURITY_ATTRIBUTES,
        },
        Storage::FileSystem::{
            CreateFileW, ReadFile, WriteFile, FILE_FLAG_FIRST_PIPE_INSTANCE, FILE_FLAG_OVERLAPPED,
            OPEN_EXISTING, PIPE_ACCESS_DUPLEX, SECURITY_IDENTIFICATION, SECURITY_SQOS_PRESENT,
        },
        System::{
            Pipes::{
                ConnectNamedPipe, CreateNamedPipeW, DisconnectNamedPipe,
                GetNamedPipeClientProcessId, GetNamedPipeServerProcessId,
                ImpersonateNamedPipeClient, WaitNamedPipeW, PIPE_READMODE_BYTE,
                PIPE_REJECT_REMOTE_CLIENTS, PIPE_TYPE_BYTE, PIPE_UNLIMITED_INSTANCES, PIPE_WAIT,
            },
            Threading::{
                CreateEventW, OpenProcess, SetEvent, WaitForMultipleObjects, WaitForSingleObject,
                INFINITE, PROCESS_QUERY_LIMITED_INFORMATION,
            },
            IO::{CancelIoEx, GetOverlappedResult, OVERLAPPED},
        },
    },
};

/// The advisory size of each direction's pipe buffer.
const BUFFER: u32 = 64 * 1024;

fn wide(text: &str) -> Vec<u16> {
    text.encode_utf16().chain(Some(0)).collect()
}

fn raw(handle: &OwnedHandle) -> HANDLE {
    handle.as_raw_handle() as HANDLE
}

/// A manual-reset event.
struct Event(OwnedHandle);

impl Event {
    fn new() -> io::Result<Self> {
        // SAFETY: no security attributes and no name; the call creates a new object.
        let event = unsafe { CreateEventW(std::ptr::null(), 1, 0, std::ptr::null()) };
        if event.is_null() {
            return Err(io::Error::last_os_error());
        }
        // SAFETY: CreateEventW returned a handle that nothing else owns.
        Ok(Self(unsafe { OwnedHandle::from_raw_handle(event) }))
    }

    fn raw(&self) -> HANDLE {
        raw(&self.0)
    }

    fn set(&self) {
        // SAFETY: the event is open while `self` lives.
        unsafe { SetEvent(self.raw()) };
    }

    fn is_set(&self) -> bool {
        // SAFETY: the event is open; a zero timeout only polls it.
        unsafe { WaitForSingleObject(self.raw(), 0) == WAIT_OBJECT_0 }
    }
}

/// How an overlapped operation ended.
enum Outcome {
    /// It transferred this many bytes (a connect transfers none).
    Done(u32),
    /// It failed with this Win32 error.
    Failed(u32),
    TimedOut,
    /// The stream or listener was shut down first.
    Closed,
}

fn wait_millis(timeout: Option<Duration>) -> u32 {
    timeout.map_or(INFINITE, |timeout| {
        u32::try_from(timeout.as_millis())
            .unwrap_or(INFINITE - 1)
            .clamp(1, INFINITE - 1)
    })
}

/// The result of the completed operation of `overlapped`.
fn finished(handle: HANDLE, overlapped: &OVERLAPPED) -> Outcome {
    let mut count = 0;
    // SAFETY: the operation of `overlapped` on `handle` has completed; no waiting.
    if unsafe { GetOverlappedResult(handle, overlapped, &mut count, 0) } != 0 {
        Outcome::Done(count)
    } else {
        // SAFETY: plain call right after the failure.
        Outcome::Failed(unsafe { GetLastError() })
    }
}

/// Run one overlapped operation on `handle`: `start` begins it with the
/// OVERLAPPED it is given. Waits until it completes, `timeout` passes or `closed`
/// is set; then cancels it and waits again until the system is done with the
/// OVERLAPPED, which lives on this stack frame.
fn overlapped(
    handle: HANDLE,
    closed: &Event,
    timeout: Option<Duration>,
    start: impl FnOnce(*mut OVERLAPPED) -> BOOL,
) -> io::Result<Outcome> {
    if closed.is_set() {
        return Ok(Outcome::Closed);
    }
    let event = Event::new()?;
    let mut overlapped = OVERLAPPED {
        hEvent: event.raw(),
        ..Default::default()
    };
    if start(&mut overlapped) != 0 {
        return Ok(finished(handle, &overlapped));
    }
    // SAFETY: plain call right after the failed start.
    let error = unsafe { GetLastError() };
    if error != ERROR_IO_PENDING {
        // Nothing is pending.
        return Ok(Outcome::Failed(error));
    }
    let events = [event.raw(), closed.raw()];
    // SAFETY: both events stay open during the wait.
    let wait = unsafe { WaitForMultipleObjects(2, events.as_ptr(), 0, wait_millis(timeout)) };
    if wait == WAIT_OBJECT_0 {
        return Ok(finished(handle, &overlapped));
    }
    let wait_error = (wait == WAIT_FAILED).then(io::Error::last_os_error);
    // SAFETY: `overlapped` belongs to the pending operation on `handle`.
    unsafe { CancelIoEx(handle, &overlapped) };
    let mut count = 0;
    // SAFETY: as above; waiting, the call returns only once the operation ended,
    // so the system no longer uses `overlapped` afterwards.
    if unsafe { GetOverlappedResult(handle, &overlapped, &mut count, 1) } != 0 {
        // It completed before the cancellation took effect.
        return Ok(Outcome::Done(count));
    }
    // SAFETY: plain call right after the failure.
    let error = unsafe { GetLastError() };
    if let Some(wait_error) = wait_error {
        return Err(wait_error);
    }
    Ok(if error != ERROR_OPERATION_ABORTED {
        Outcome::Failed(error)
    } else if wait == WAIT_TIMEOUT {
        Outcome::TimedOut
    } else {
        Outcome::Closed
    })
}

struct Shared {
    handle: OwnedHandle,
    /// Set by `shutdown`: wakes every wait and refuses later ones.
    closed: Event,
    /// Milliseconds, 0 for none.
    read_timeout: AtomicU64,
    write_timeout: AtomicU64,
}

/// A connected pipe end, server or client. Clones from `try_clone` share the
/// handle, its timeouts and `shutdown`, so one thread can block reading while
/// another writes. The handle closes with the last clone; data already written
/// stays readable for the peer, which then reads end of file.
pub struct PipeStream(Arc<Shared>);

fn store_timeout(slot: &AtomicU64, timeout: Option<Duration>) -> io::Result<()> {
    let millis = match timeout {
        None => 0,
        Some(timeout) if timeout.is_zero() => {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "a zero timeout is not allowed",
            ))
        }
        Some(timeout) => u64::try_from(timeout.as_millis())
            .unwrap_or(u64::MAX)
            .max(1),
    };
    slot.store(millis, Ordering::Relaxed);
    Ok(())
}

fn load_timeout(slot: &AtomicU64) -> Option<Duration> {
    match slot.load(Ordering::Relaxed) {
        0 => None,
        millis => Some(Duration::from_millis(millis)),
    }
}

impl PipeStream {
    fn new(handle: OwnedHandle) -> io::Result<Self> {
        Ok(Self(Arc::new(Shared {
            handle,
            closed: Event::new()?,
            read_timeout: AtomicU64::new(0),
            write_timeout: AtomicU64::new(0),
        })))
    }

    fn raw(&self) -> HANDLE {
        raw(&self.0.handle)
    }

    /// Another handle on this stream, for a second thread.
    pub fn try_clone(&self) -> io::Result<Self> {
        Ok(Self(self.0.clone()))
    }

    /// How long a read waits for data; None waits for ever. A read that times out
    /// fails with `TimedOut`, and a frame it was part of is lost.
    pub fn set_read_timeout(&self, timeout: Option<Duration>) -> io::Result<()> {
        store_timeout(&self.0.read_timeout, timeout)
    }

    /// How long a write waits for the peer to make room; None waits for ever. A
    /// write that times out may have sent part of its bytes, so the connection
    /// is out of step and must be closed.
    pub fn set_write_timeout(&self, timeout: Option<Duration>) -> io::Result<()> {
        store_timeout(&self.0.write_timeout, timeout)
    }

    /// Wake every thread waiting on this stream: reads end (end of file) and
    /// writes fail with `BrokenPipe`, now and later. The handle stays open until
    /// the last clone drops.
    pub fn shutdown(&self) {
        self.0.closed.set();
    }

    /// Check the client of this server end once its hello has been read: it must
    /// run as `host.user`, at `host.integrity` or above. A client that opened the
    /// pipe with dynamic tracking (no `SECURITY_SQOS_PRESENT`) can be impersonated
    /// only after a read (`ERROR_CANNOT_IMPERSONATE` before); `connect` asks for
    /// static tracking, which fixes the identity when the pipe is opened. The
    /// thread reverts to itself before this returns, on every path.
    pub fn authenticate(&self, host: &Identity) -> Result<Client, AuthError> {
        let token = {
            let _impersonation = Impersonation::begin(self.raw()).map_err(AuthError::Io)?;
            Token::of_thread().map_err(AuthError::Io)?
        };
        let token =
            token.ok_or_else(|| AuthError::Io(io::Error::other("the client token is missing")))?;
        let client = token.identity().map_err(AuthError::Io)?;
        if client.user != host.user {
            return Err(AuthError::OtherUser);
        }
        if client.integrity.rid < host.integrity.rid {
            return Err(AuthError::LowerIntegrity(client.integrity));
        }
        let mut pid = 0;
        // SAFETY: a server end of a connected pipe and an out pointer to a local.
        if unsafe { GetNamedPipeClientProcessId(self.raw(), &mut pid) } == 0 {
            return Err(AuthError::Io(io::Error::last_os_error()));
        }
        Ok(Client {
            pid,
            integrity: client.integrity,
        })
    }

    /// The ID of the process serving this client end (`connect` has checked
    /// that it runs as this user).
    pub fn server_pid(&self) -> io::Result<u32> {
        let mut pid = 0;
        // SAFETY: a client end of a connected pipe and an out pointer to a local.
        if unsafe { GetNamedPipeServerProcessId(self.raw(), &mut pid) } == 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(pid)
    }

    /// The server process's ID, once its token shows this process's user.
    fn verify_server(&self) -> Result<u32, ConnectError> {
        let unverified = |error: io::Error| {
            ConnectError::Refused(format!("cannot verify the session host: {error}"))
        };
        let mut pid = 0;
        // SAFETY: a client end of a connected pipe and an out pointer to a local.
        if unsafe { GetNamedPipeServerProcessId(self.raw(), &mut pid) } == 0 {
            return Err(unverified(io::Error::last_os_error()));
        }
        // SAFETY: plain call; a handle is returned only on success.
        let process = unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid) };
        if process.is_null() {
            return Err(unverified(io::Error::last_os_error()));
        }
        // SAFETY: OpenProcess returned a handle that nothing else owns.
        let process = unsafe { OwnedHandle::from_raw_handle(process) };
        let server = Token::of_process(raw(&process))
            .and_then(|token| token.user())
            .map_err(unverified)?;
        let own = Token::current()
            .and_then(|token| token.user())
            .map_err(unverified)?;
        if server != own {
            return Err(ConnectError::Refused(
                "the session host pipe is served by another user's process".into(),
            ));
        }
        Ok(pid)
    }
}

impl Read for &PipeStream {
    fn read(&mut self, buffer: &mut [u8]) -> io::Result<usize> {
        if buffer.is_empty() {
            return Ok(0);
        }
        let handle = self.raw();
        let length = u32::try_from(buffer.len()).unwrap_or(u32::MAX);
        let timeout = load_timeout(&self.0.read_timeout);
        let outcome = overlapped(handle, &self.0.closed, timeout, |overlapped| {
            // SAFETY: `buffer` holds `length` writable bytes and outlives the
            // operation, which `overlapped` waits for.
            unsafe {
                ReadFile(
                    handle,
                    buffer.as_mut_ptr(),
                    length,
                    std::ptr::null_mut(),
                    overlapped,
                )
            }
        })?;
        match outcome {
            Outcome::Done(count) => Ok(count as usize),
            Outcome::Failed(ERROR_BROKEN_PIPE | ERROR_NO_DATA | ERROR_PIPE_NOT_CONNECTED)
            | Outcome::Closed => Ok(0),
            Outcome::Failed(error) => Err(io::Error::from_raw_os_error(error as i32)),
            Outcome::TimedOut => Err(io::ErrorKind::TimedOut.into()),
        }
    }
}

impl Write for &PipeStream {
    fn write(&mut self, buffer: &[u8]) -> io::Result<usize> {
        if buffer.is_empty() {
            return Ok(0);
        }
        let handle = self.raw();
        let length = u32::try_from(buffer.len()).unwrap_or(u32::MAX);
        let timeout = load_timeout(&self.0.write_timeout);
        let outcome = overlapped(handle, &self.0.closed, timeout, |overlapped| {
            // SAFETY: `buffer` holds `length` bytes and outlives the operation,
            // which `overlapped` waits for.
            unsafe {
                WriteFile(
                    handle,
                    buffer.as_ptr(),
                    length,
                    std::ptr::null_mut(),
                    overlapped,
                )
            }
        })?;
        match outcome {
            Outcome::Done(count) => Ok(count as usize),
            Outcome::Failed(error) => Err(io::Error::from_raw_os_error(error as i32)),
            Outcome::TimedOut => Err(io::ErrorKind::TimedOut.into()),
            Outcome::Closed => Err(io::Error::new(
                io::ErrorKind::BrokenPipe,
                "the session host connection was shut down",
            )),
        }
    }

    /// A completed write is in the pipe already. FlushFileBuffers would wait for
    /// the peer to read everything.
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

impl Read for PipeStream {
    fn read(&mut self, buffer: &mut [u8]) -> io::Result<usize> {
        (&*self).read(buffer)
    }
}

impl Write for PipeStream {
    fn write(&mut self, buffer: &[u8]) -> io::Result<usize> {
        (&*self).write(buffer)
    }

    fn flush(&mut self) -> io::Result<()> {
        (&*self).flush()
    }
}

/// This thread acts as a pipe client until the guard drops.
struct Impersonation(());

impl Impersonation {
    fn begin(pipe: HANDLE) -> io::Result<Self> {
        // SAFETY: `pipe` is a server end the caller keeps open.
        if unsafe { ImpersonateNamedPipeClient(pipe) } == 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(Self(()))
    }
}

impl Drop for Impersonation {
    fn drop(&mut self) {
        // SAFETY: plain call; this thread impersonates since `begin`.
        if unsafe { RevertToSelf() } == 0 {
            // A thread that keeps a client's identity must not run on.
            std::process::abort();
        }
    }
}

/// A client the host accepted.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Client {
    pub pid: u32,
    pub integrity: Integrity,
}

/// Why the host refuses a client.
#[derive(Debug)]
pub enum AuthError {
    /// It runs as another user.
    OtherUser,
    /// It runs below the host's integrity level, at this one.
    LowerIntegrity(Integrity),
    /// Its identity could not be read.
    Io(io::Error),
}

impl AuthError {
    /// The answer to the client's hello before the host closes the connection.
    pub fn refusal(&self) -> ErrorBody {
        let message = match self {
            Self::OtherUser => "the session host belongs to another user".into(),
            Self::LowerIntegrity(_) => ELEVATED_HOST.into(),
            Self::Io(error) => format!("the session host cannot verify this client: {error}"),
        };
        ErrorBody::new(ErrorCode::Refused, message)
    }
}

/// Why `connect` gave no connection. Only `NoHost` means that no host runs (D3);
/// everything else is an error, never an empty listing.
#[derive(Debug)]
pub enum ConnectError {
    /// Nothing serves the name (`ERROR_FILE_NOT_FOUND`).
    NoHost,
    /// Every instance stayed busy until the deadline.
    Busy,
    /// The server is not this user's, or refused this client.
    Refused(String),
    Io(io::Error),
}

impl fmt::Display for ConnectError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            Self::NoHost => f.write_str("no session host runs"),
            Self::Busy => f.write_str("the session host did not accept the connection in time"),
            Self::Refused(reason) => f.write_str(reason),
            Self::Io(error) => write!(f, "cannot connect to the session host: {error}"),
        }
    }
}

impl std::error::Error for ConnectError {}

/// Connect to the host at `name`, waiting while every instance is busy until
/// `deadline`, and return once the server process is known to run as this user.
/// Nothing has been sent yet. The client allows identification only, so a
/// server can check who connects but cannot act as the client.
pub fn connect(name: &str, deadline: Instant) -> Result<PipeStream, ConnectError> {
    let name = wide(name);
    loop {
        // SAFETY: `name` is NUL-terminated; no security attributes or template.
        let handle = unsafe {
            CreateFileW(
                name.as_ptr(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                std::ptr::null(),
                OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                std::ptr::null_mut(),
            )
        };
        if handle != INVALID_HANDLE_VALUE {
            // SAFETY: CreateFileW returned a handle that nothing else owns.
            let stream = PipeStream::new(unsafe { OwnedHandle::from_raw_handle(handle) })
                .map_err(ConnectError::Io)?;
            stream.verify_server()?;
            return Ok(stream);
        }
        // SAFETY: plain call right after the failure.
        match unsafe { GetLastError() } {
            ERROR_FILE_NOT_FOUND => return Err(ConnectError::NoHost),
            // The label of an elevated host keeps lower-integrity clients out
            // before the host sees them; another user could not know the name.
            ERROR_ACCESS_DENIED => return Err(ConnectError::Refused(ELEVATED_HOST.into())),
            ERROR_PIPE_BUSY => {
                let left = deadline.saturating_duration_since(Instant::now());
                if left.is_zero() {
                    return Err(ConnectError::Busy);
                }
                // A timeout of 0 would mean the pipe's default wait.
                // SAFETY: `name` is NUL-terminated.
                if unsafe { WaitNamedPipeW(name.as_ptr(), wait_millis(Some(left))) } == 0 {
                    // SAFETY: plain call right after the failure.
                    match unsafe { GetLastError() } {
                        // Timed out, or the pipe went away: the next attempt tells.
                        ERROR_SEM_TIMEOUT | ERROR_FILE_NOT_FOUND => {}
                        error => {
                            return Err(ConnectError::Io(io::Error::from_raw_os_error(
                                error as i32,
                            )))
                        }
                    }
                }
            }
            error => return Err(ConnectError::Io(io::Error::from_raw_os_error(error as i32))),
        }
    }
}

/// A self-relative security descriptor from LocalAlloc.
struct SecurityDescriptor(PSECURITY_DESCRIPTOR);

// SAFETY: the descriptor is never written after its creation and is freed once,
// when the owner drops it.
unsafe impl Send for SecurityDescriptor {}
// SAFETY: as above; shared use only reads it.
unsafe impl Sync for SecurityDescriptor {}

impl SecurityDescriptor {
    fn from_sddl(sddl: &str) -> io::Result<Self> {
        let sddl = wide(sddl);
        let mut descriptor = std::ptr::null_mut();
        // SAFETY: `sddl` is NUL-terminated; `descriptor` receives a LocalAlloc
        // block that `Drop` frees.
        if unsafe {
            ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.as_ptr(),
                SDDL_REVISION_1,
                &mut descriptor,
                std::ptr::null_mut(),
            )
        } == 0
        {
            return Err(io::Error::last_os_error());
        }
        Ok(Self(descriptor))
    }
}

impl Drop for SecurityDescriptor {
    fn drop(&mut self) {
        // SAFETY: LocalAlloc memory that nothing uses any more.
        unsafe { LocalFree(self.0) };
    }
}

/// Why `bind` created no listener.
#[derive(Debug)]
pub enum BindError {
    /// An instance of the name exists: another host serves it, or an exiting host
    /// still holds it (a launcher retries until that host's process is gone).
    InUse,
    Io(io::Error),
}

impl fmt::Display for BindError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            Self::InUse => f.write_str("another session host serves this pipe"),
            Self::Io(error) => write!(f, "cannot create the session host pipe: {error}"),
        }
    }
}

impl std::error::Error for BindError {}

/// The server side of the host pipe: byte-mode duplex instances that refuse
/// remote clients, protected by a DACL that grants access to the host's user
/// alone and, for an elevated host, a high mandatory label (`pipe_sddl`).
pub struct PipeListener {
    name: Vec<u16>,
    security: SecurityDescriptor,
    /// The instance waiting for the next client while no `accept` holds it.
    idle: Mutex<Option<OwnedHandle>>,
    closed: Event,
}

impl PipeListener {
    /// Create the first instance of `name` for a host running as `host`. The name
    /// must not exist yet, so nobody else can have prepared it.
    pub fn bind(name: &str, host: &Identity) -> Result<Self, BindError> {
        let user = host.user.text().map_err(BindError::Io)?;
        let security = SecurityDescriptor::from_sddl(&pipe_sddl(&user, host.integrity.rid))
            .map_err(BindError::Io)?;
        let name = wide(name);
        let first = create_instance(&name, &security, true).map_err(|error| {
            // FILE_FLAG_FIRST_PIPE_INSTANCE fails with ERROR_ACCESS_DENIED while the
            // name exists.
            if error.raw_os_error() == Some(ERROR_ACCESS_DENIED as i32) {
                BindError::InUse
            } else {
                BindError::Io(error)
            }
        })?;
        Ok(Self {
            name,
            security,
            idle: Mutex::new(Some(first)),
            closed: Event::new().map_err(BindError::Io)?,
        })
    }

    /// Wait for the next client. Another instance is listening before the
    /// connected one is returned, so a client never finds the name missing while
    /// the host runs.
    pub fn accept(&self) -> io::Result<PipeStream> {
        let taken = self
            .idle
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .take();
        let instance = match taken {
            Some(instance) => instance,
            None if self.closed.is_set() => return Err(listener_closed()),
            None => create_instance(&self.name, &self.security, false)?,
        };
        let handle = raw(&instance);
        loop {
            let outcome = overlapped(handle, &self.closed, None, |overlapped| {
                // SAFETY: `handle` is a listening instance; `overlapped` waits for
                // the operation.
                unsafe { ConnectNamedPipe(handle, overlapped) }
            })?;
            match outcome {
                // A client that connected before this call counts as connected.
                Outcome::Done(_) | Outcome::Failed(ERROR_PIPE_CONNECTED) => break,
                // It connected and left already: serve the next one.
                Outcome::Failed(ERROR_NO_DATA) => {
                    // SAFETY: `handle` is an instance this listener owns.
                    unsafe { DisconnectNamedPipe(handle) };
                }
                Outcome::Closed => return Err(listener_closed()),
                Outcome::Failed(error) => return Err(io::Error::from_raw_os_error(error as i32)),
                Outcome::TimedOut => return Err(io::ErrorKind::TimedOut.into()),
            }
        }
        let mut idle = self.idle.lock().unwrap_or_else(PoisonError::into_inner);
        // `close` sets the event before it takes the lock, so either this sees
        // it or `close` removes the new instance.
        if idle.is_none() && !self.closed.is_set() {
            // Should this fail, the next `accept` creates one; until then clients
            // wait as for a busy pipe.
            *idle = create_instance(&self.name, &self.security, false).ok();
        }
        drop(idle);
        PipeStream::new(instance)
    }

    /// Stop listening: a waiting `accept` returns, later ones fail and the name
    /// disappears once the connected instances close too. An idle host closes
    /// its listener before it decides to exit (D3).
    pub fn close(&self) {
        self.closed.set();
        drop(
            self.idle
                .lock()
                .unwrap_or_else(PoisonError::into_inner)
                .take(),
        );
    }

    pub fn is_closed(&self) -> bool {
        self.closed.is_set()
    }
}

fn listener_closed() -> io::Error {
    io::Error::new(
        io::ErrorKind::NotConnected,
        "the session host stopped listening",
    )
}

fn create_instance(
    name: &[u16],
    security: &SecurityDescriptor,
    first: bool,
) -> io::Result<OwnedHandle> {
    let attributes = SECURITY_ATTRIBUTES {
        nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32,
        lpSecurityDescriptor: security.0,
        bInheritHandle: 0,
    };
    let first = if first {
        FILE_FLAG_FIRST_PIPE_INSTANCE
    } else {
        0
    };
    // SAFETY: `name` is NUL-terminated and `attributes` points to a valid
    // descriptor for the duration of the call.
    let handle = unsafe {
        CreateNamedPipeW(
            name.as_ptr(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | first,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES,
            BUFFER,
            BUFFER,
            0,
            &attributes,
        )
    };
    if handle == INVALID_HANDLE_VALUE {
        return Err(io::Error::last_os_error());
    }
    // SAFETY: CreateNamedPipeW returned a handle that nothing else owns.
    Ok(unsafe { OwnedHandle::from_raw_handle(handle) })
}

#[cfg(test)]
mod tests {
    use super::super::protocol::{
        read_frame, write_frame, write_json, Frame, Hello, HelloAnswer, Kind, Role,
    };
    use super::super::token::Sid;
    use super::*;
    use std::{
        sync::{
            atomic::AtomicU32,
            mpsc::{self, Receiver},
        },
        thread,
        time::{SystemTime, UNIX_EPOCH},
    };
    use windows_sys::Win32::{
        Foundation::{ERROR_CANNOT_IMPERSONATE, ERROR_SUCCESS, GENERIC_ALL},
        Security::{
            Authorization::{GetSecurityInfo, SE_KERNEL_OBJECT},
            GetAce, GetSecurityDescriptorControl, ACCESS_ALLOWED_ACE, ACE_HEADER, ACL,
            DACL_SECURITY_INFORMATION, LABEL_SECURITY_INFORMATION, PSID, SE_DACL_PROTECTED,
        },
        Storage::FileSystem::FILE_ALL_ACCESS,
    };

    /// No test waits longer than this for anything.
    const LIMIT: Duration = Duration::from_secs(20);

    /// A pipe name of this test run that no host uses.
    fn unique_name() -> String {
        static NEXT: AtomicU32 = AtomicU32::new(0);
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_or(0, |time| time.subsec_nanos());
        format!(
            r"\\.\pipe\hgs-host-test-{}-{}-{nanos}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        )
    }

    /// Run `work` on its own thread; `finish` waits for it at most `LIMIT`.
    fn spawn<T: Send + 'static>(work: impl FnOnce() -> T + Send + 'static) -> Receiver<T> {
        let (sender, receiver) = mpsc::channel();
        thread::spawn(move || {
            let _ = sender.send(work());
        });
        receiver
    }

    fn finish<T>(receiver: Receiver<T>) -> T {
        receiver
            .recv_timeout(LIMIT)
            .expect("a pipe operation did not finish in time")
    }

    fn limit(stream: &PipeStream) {
        stream.set_read_timeout(Some(LIMIT)).unwrap();
        stream.set_write_timeout(Some(LIMIT)).unwrap();
    }

    /// A connected server and client end. A new instance is listening, so the
    /// client connects before `accept` runs, which then finds it connected.
    fn pair(host: &Identity) -> (PipeStream, PipeStream) {
        let name = unique_name();
        let listener = PipeListener::bind(&name, host).unwrap();
        let client = connect(&name, Instant::now() + LIMIT).unwrap();
        let server = listener.accept().unwrap();
        limit(&server);
        limit(&client);
        (server, client)
    }

    #[test]
    fn frames_flow_both_ways_while_both_sides_block_reading() {
        let host = Identity::current().unwrap();
        let name = unique_name();
        let listener = PipeListener::bind(&name, &host).unwrap();
        // Several frames are larger than the pipe buffers.
        let payloads: Vec<Vec<u8>> = (0..24)
            .map(|i| vec![i as u8; (i * 47_000) % 300_000 + 1])
            .collect();
        let expected = payloads.clone();

        let server = spawn(move || {
            let stream = listener.accept().unwrap();
            limit(&stream);
            let hello = read_frame(&mut &stream).unwrap().unwrap();
            assert_eq!(hello.json::<Hello>().unwrap().pid, std::process::id());
            let client = stream.authenticate(&host).unwrap();
            assert_eq!(client.pid, std::process::id());
            assert_eq!(client.integrity, host.integrity);
            assert!(Token::of_thread().unwrap().is_none(), "not reverted");
            // Echo from a second thread while this one keeps reading.
            let writer = stream.try_clone().unwrap();
            let (sender, frames) = mpsc::channel::<Frame>();
            let echo = thread::spawn(move || {
                for frame in frames {
                    write_frame(&mut &writer, frame.kind, &frame.payload).unwrap();
                }
            });
            let mut count = 0;
            while let Some(frame) = read_frame(&mut &stream).unwrap() {
                sender.send(frame).unwrap();
                count += 1;
            }
            drop(sender);
            echo.join().unwrap();
            count
        });
        let client = spawn(move || {
            let stream = connect(&name, Instant::now() + LIMIT).unwrap();
            limit(&stream);
            // The reader blocks before anything is written in either direction.
            let reader = stream.try_clone().unwrap();
            let count = payloads.len();
            let echoed = thread::spawn(move || {
                (0..count)
                    .map(|_| read_frame(&mut &reader).unwrap().unwrap().payload)
                    .collect::<Vec<_>>()
            });
            thread::sleep(Duration::from_millis(100));
            write_json(&mut &stream, Kind::Json, &Hello::new(Role::Cli)).unwrap();
            for payload in &payloads {
                write_frame(&mut &stream, Kind::Stream, payload).unwrap();
            }
            echoed.join().unwrap()
        });
        assert_eq!(finish(client), expected);
        assert_eq!(finish(server), expected.len());
    }

    #[test]
    fn a_name_has_one_listener_at_a_time() {
        let host = Identity::current().unwrap();
        let name = unique_name();
        let first = PipeListener::bind(&name, &host).unwrap();
        assert!(matches!(
            PipeListener::bind(&name, &host),
            Err(BindError::InUse)
        ));
        drop(first);
        PipeListener::bind(&name, &host).unwrap();
    }

    #[test]
    fn a_name_nobody_serves_means_no_host() {
        let deadline = Instant::now() + LIMIT;
        assert!(matches!(
            connect(&unique_name(), deadline),
            Err(ConnectError::NoHost)
        ));
        assert!(Instant::now() < deadline, "no waiting");
    }

    #[test]
    fn the_server_check_accepts_this_users_host() {
        let host = Identity::current().unwrap();
        let (_server, client) = pair(&host);
        assert_eq!(client.verify_server().unwrap(), std::process::id());
        assert_eq!(client.server_pid().unwrap(), std::process::id());
    }

    #[test]
    fn closing_stops_listening_and_wakes_accept() {
        let host = Identity::current().unwrap();
        let name = unique_name();
        let listener = Arc::new(PipeListener::bind(&name, &host).unwrap());
        let waiting = {
            let listener = listener.clone();
            spawn(move || listener.accept().map(drop))
        };
        thread::sleep(Duration::from_millis(100));
        listener.close();
        assert!(finish(waiting).is_err());
        assert!(listener.is_closed());
        assert!(listener.accept().is_err());
        assert!(matches!(
            connect(&name, Instant::now() + LIMIT),
            Err(ConnectError::NoHost)
        ));
    }

    #[test]
    fn a_closed_listener_keeps_its_connections() {
        let host = Identity::current().unwrap();
        let name = unique_name();
        let listener = PipeListener::bind(&name, &host).unwrap();
        let client = connect(&name, Instant::now() + LIMIT).unwrap();
        let server = listener.accept().unwrap();
        limit(&server);
        limit(&client);
        listener.close();
        // The connected instance keeps the name, but nothing listens on it.
        assert!(matches!(
            connect(&name, Instant::now() + Duration::from_millis(200)),
            Err(ConnectError::Busy)
        ));
        write_frame(&mut &server, Kind::Json, b"{}").unwrap();
        assert_eq!(read_frame(&mut &client).unwrap().unwrap().payload, b"{}");
        drop(client);
        assert!(read_frame(&mut &server).unwrap().is_none(), "end of file");
    }

    #[test]
    fn the_pipe_grants_access_to_this_user_alone() {
        let host = Identity::current().unwrap();
        let (_server, client) = pair(&host);
        let high = host.integrity.rid >= Integrity::HIGH;
        let mut dacl: *mut ACL = std::ptr::null_mut();
        let mut sacl: *mut ACL = std::ptr::null_mut();
        let mut descriptor = std::ptr::null_mut();
        let information =
            DACL_SECURITY_INFORMATION | if high { LABEL_SECURITY_INFORMATION } else { 0 };
        // SAFETY: an open pipe handle with READ_CONTROL and out pointers to locals.
        let code = unsafe {
            GetSecurityInfo(
                client.raw(),
                SE_KERNEL_OBJECT,
                information,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                &mut dacl,
                &mut sacl,
                &mut descriptor,
            )
        };
        assert_eq!(code, ERROR_SUCCESS);
        let descriptor = SecurityDescriptor(descriptor);
        let (mut control, mut revision) = (0, 0);
        // SAFETY: a descriptor GetSecurityInfo returned.
        assert_ne!(
            unsafe { GetSecurityDescriptorControl(descriptor.0, &mut control, &mut revision) },
            0
        );
        assert_ne!(control & SE_DACL_PROTECTED, 0, "inherited ACEs are blocked");

        let entries = aces(dacl);
        assert_eq!(entries.len(), 1, "one ACE");
        let (kind, mask, sid) = &entries[0];
        assert_eq!(*kind, 0, "ACCESS_ALLOWED_ACE_TYPE");
        assert_eq!(*sid, host.user);
        assert!(
            *mask == GENERIC_ALL || mask & FILE_ALL_ACCESS == FILE_ALL_ACCESS,
            "{mask:#x}"
        );
        if high {
            let labels = aces(sacl);
            assert_eq!(labels.len(), 1, "one mandatory label");
            let (kind, mask, sid) = &labels[0];
            assert_eq!(*kind, 0x11, "SYSTEM_MANDATORY_LABEL_ACE_TYPE");
            assert_eq!(*mask, 3, "no write up, no read up");
            assert_eq!(sid.text().unwrap(), "S-1-16-12288");
        }
    }

    /// Type, mask and SID of every ACE of `acl`; access and mandatory label ACEs
    /// share this layout.
    fn aces(acl: *const ACL) -> Vec<(u8, u32, Sid)> {
        if acl.is_null() {
            return Vec::new();
        }
        // SAFETY: `acl` is a valid ACL of a descriptor that the caller keeps.
        let count = unsafe { (*acl).AceCount };
        (0..u32::from(count))
            .map(|index| {
                let mut ace = std::ptr::null_mut();
                // SAFETY: `index` is below the ACL's ACE count.
                assert_ne!(unsafe { GetAce(acl, index, &mut ace) }, 0);
                let ace = ace as *const ACCESS_ALLOWED_ACE;
                // SAFETY: GetAce returned an ACE of this ACL with this layout.
                unsafe {
                    let header: ACE_HEADER = (*ace).Header;
                    let sid = std::ptr::addr_of!((*ace).SidStart) as PSID;
                    (header.AceType, (*ace).Mask, Sid::copy(sid))
                }
            })
            .collect()
    }

    #[test]
    fn authentication_reverts_on_every_path() {
        let host = Identity::current().unwrap();
        let (server, client) = pair(&host);
        // Nothing read yet. With `connect`'s static tracking the pipe file system
        // captured the client when it opened the pipe, so it may be identified
        // already; with dynamic tracking it could not be before a read. The host
        // reads the hello first either way, and the thread reverts on both paths.
        match server.authenticate(&host) {
            Ok(early) => assert_eq!(early.pid, std::process::id()),
            Err(error) => assert!(
                matches!(&error, AuthError::Io(e) if e.raw_os_error() == Some(ERROR_CANNOT_IMPERSONATE as i32)),
                "{error:?}"
            ),
        }
        assert!(Token::of_thread().unwrap().is_none());

        write_json(&mut &client, Kind::Json, &Hello::new(Role::Cli)).unwrap();
        read_frame(&mut &server).unwrap().unwrap();
        {
            let _impersonation = Impersonation::begin(server.raw()).unwrap();
            assert!(Token::of_thread().unwrap().is_some());
        }
        assert!(Token::of_thread().unwrap().is_none());

        // Refusals revert as well.
        let elevated = Identity {
            user: host.user.clone(),
            integrity: Integrity::from_rid(host.integrity.rid + 0x1000),
        };
        let error = server.authenticate(&elevated).unwrap_err();
        assert!(matches!(error, AuthError::LowerIntegrity(ref level) if *level == host.integrity));
        assert_eq!(error.refusal().message, ELEVATED_HOST);
        assert!(Token::of_thread().unwrap().is_none());
        let stranger = Identity {
            user: Sid::local_system(),
            integrity: host.integrity.clone(),
        };
        let error = server.authenticate(&stranger).unwrap_err();
        assert!(matches!(error, AuthError::OtherUser));
        assert_eq!(error.refusal().code, ErrorCode::Refused);
        assert!(Token::of_thread().unwrap().is_none());

        let accepted = server.authenticate(&host).unwrap();
        assert_eq!(accepted.pid, std::process::id());
        assert!(Token::of_thread().unwrap().is_none());

        // The host answers the hello; the client reads the answer.
        let answer = HelloAnswer::Refused(error.refusal());
        write_json(&mut &server, Kind::Json, &answer).unwrap();
        let frame = read_frame(&mut &client).unwrap().unwrap();
        assert_eq!(frame.json::<HelloAnswer>().unwrap(), answer);
    }

    #[test]
    fn shutdown_wakes_a_blocked_reader() {
        let host = Identity::current().unwrap();
        let (server, client) = pair(&host);
        let reader = client.try_clone().unwrap();
        let read = spawn(move || (&reader).read(&mut [0; 16]).unwrap());
        thread::sleep(Duration::from_millis(100));
        client.shutdown();
        assert_eq!(finish(read), 0, "end of file");
        let error = (&client).write(b"x").unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::BrokenPipe);
        // The peer sees end of file once the last clone has closed the handle.
        drop(client);
        assert!(read_frame(&mut &server).unwrap().is_none());
        let error = (&server).write(b"x").unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::BrokenPipe, "{error:?}");
    }

    #[test]
    fn a_read_times_out_and_the_stream_still_works() {
        let host = Identity::current().unwrap();
        let (server, client) = pair(&host);
        assert!(client.set_read_timeout(Some(Duration::ZERO)).is_err());
        client
            .set_read_timeout(Some(Duration::from_millis(50)))
            .unwrap();
        let error = (&client).read(&mut [0; 4]).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::TimedOut);
        client.set_read_timeout(Some(LIMIT)).unwrap();
        write_frame(&mut &server, Kind::Json, b"{}").unwrap();
        assert_eq!(read_frame(&mut &client).unwrap().unwrap().payload, b"{}");
        client.set_read_timeout(None).unwrap();
        // Owned streams read and write too.
        let (mut server, mut client) = (server, client);
        client.write_all(b"ping").unwrap();
        client.flush().unwrap();
        let mut buffer = [0; 4];
        server.read_exact(&mut buffer).unwrap();
        assert_eq!(&buffer, b"ping");
    }
}
