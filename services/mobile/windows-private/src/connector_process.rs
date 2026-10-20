//! One detached connector, with only its own NUL handle inherited.
//! Stable Rust's Command cannot restrict Windows handle inheritance. Keep the
//! Win32 boundary here; callers get only an owned handle and a readiness probe.
use std::ffi::{OsStr, OsString};
use std::fs::OpenOptions;
use std::io;
use std::mem;
use std::os::windows::ffi::OsStrExt;
use std::os::windows::io::{AsRawHandle, FromRawHandle, OwnedHandle};
use std::path::Path;
use std::ptr;
use windows_sys::Win32::{
    Foundation::{DuplicateHandle, DUPLICATE_SAME_ACCESS, HANDLE, WAIT_OBJECT_0, WAIT_TIMEOUT},
    System::Threading::{
        CreateProcessW, DeleteProcThreadAttributeList, GetCurrentProcess, GetExitCodeProcess,
        InitializeProcThreadAttributeList, UpdateProcThreadAttribute, WaitForSingleObject,
        CREATE_BREAKAWAY_FROM_JOB, CREATE_NEW_PROCESS_GROUP, CREATE_NO_WINDOW,
        CREATE_UNICODE_ENVIRONMENT, EXTENDED_STARTUPINFO_PRESENT, LPPROC_THREAD_ATTRIBUTE_LIST,
        PROCESS_INFORMATION, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, STARTF_USESTDHANDLES,
        STARTUPINFOEXW,
    },
};

pub struct ConnectorChild(OwnedHandle);
impl ConnectorChild {
    /// Probe only the process handle returned by our own launch, never a PID.
    pub fn try_wait(&self) -> io::Result<Option<u32>> {
        // SAFETY: the owned process handle remains open throughout both calls.
        match unsafe { WaitForSingleObject(self.0.as_raw_handle() as HANDLE, 0) } {
            WAIT_TIMEOUT => Ok(None),
            WAIT_OBJECT_0 => {
                let mut code = 0;
                if unsafe { GetExitCodeProcess(self.0.as_raw_handle() as HANDLE, &mut code) } == 0 {
                    return Err(io::Error::last_os_error());
                }
                Ok(Some(code))
            }
            _ => Err(io::Error::last_os_error()),
        }
    }
}

fn invalid() -> io::Error {
    io::Error::new(
        io::ErrorKind::InvalidInput,
        "invalid fixed connector launch",
    )
}
fn wide(value: &OsStr) -> io::Result<Vec<u16>> {
    let units: Vec<_> = value.encode_wide().collect();
    if units.contains(&0) {
        return Err(invalid());
    }
    Ok(units)
}
// The CRT quoting rule used by hgs host::spawn, applied to raw UTF-16 so Windows
// paths/environment values do not acquire a lossy Unicode conversion.
fn quote(value: &[u16]) -> Vec<u16> {
    if !value.is_empty() && !value.iter().any(|v| [9, 10, 11, 32, 34].contains(v)) {
        return value.to_vec();
    }
    let mut out = vec![34];
    let mut slashes = 0;
    for &unit in value {
        if unit == 92 {
            slashes += 1;
            continue;
        }
        out.extend(std::iter::repeat_n(
            92,
            if unit == 34 { slashes * 2 + 1 } else { slashes },
        ));
        out.push(unit);
        slashes = 0;
    }
    out.extend(std::iter::repeat_n(92, slashes * 2));
    out.push(34);
    out
}
fn command_line(application: &Path, revision: &str, config: &Path) -> io::Result<Vec<u16>> {
    if !application.is_absolute()
        || !config.is_absolute()
        || revision.len() != 64
        || !revision.bytes().all(|b| b.is_ascii_hexdigit())
    {
        return Err(invalid());
    }
    let program = wide(application.as_os_str())?;
    if program.contains(&34) {
        return Err(invalid());
    }
    // argv[0] uses the program-name rule, rather than the argument escape rule.
    let mut line = vec![34];
    line.extend(program);
    line.push(34);
    for arg in [
        OsStr::new("--admit-revision"),
        OsStr::new(revision),
        OsStr::new("--config"),
        config.as_os_str(),
    ] {
        line.push(32);
        line.extend(quote(&wide(arg)?));
    }
    if line.len() > 32_766 {
        return Err(invalid());
    }
    line.push(0);
    Ok(line)
}
// Same single-unit uppercase ordering as host::spawn::Environment.
fn fold(unit: u16) -> u16 {
    let Some(ch) = char::from_u32(u32::from(unit)) else {
        return unit;
    };
    let mut upper = ch.to_uppercase();
    match (upper.next(), upper.next()) {
        (Some(ch), None) => u16::try_from(u32::from(ch)).unwrap_or(unit),
        _ => unit,
    }
}
fn environment(
    entries: impl IntoIterator<Item = (OsString, OsString)>,
    removed: &[&str],
) -> io::Result<Vec<u16>> {
    let mut entries: Vec<_> = entries
        .into_iter()
        .map(|(name, value)| {
            let name = wide(&name)?;
            let value = wide(&value)?;
            Ok((name, value))
        })
        .collect::<io::Result<_>>()?;
    entries.retain(|(name, _)| {
        !removed.iter().any(|key| {
            name.iter()
                .copied()
                .map(fold)
                .eq(key.encode_utf16().map(fold))
        })
    });
    entries.sort_by_cached_key(|(name, _)| name.iter().copied().map(fold).collect::<Vec<_>>());
    let mut block = Vec::new();
    for (name, value) in entries {
        block.extend(name);
        block.push(61);
        block.extend(value);
        block.push(0);
    }
    if block.is_empty() {
        block.push(0);
    }
    block.push(0);
    Ok(block)
}

struct Attributes(Vec<usize>);
impl Attributes {
    fn new() -> io::Result<Self> {
        let mut size = 0;
        // SAFETY: the first call requests the required size only.
        unsafe {
            InitializeProcThreadAttributeList(ptr::null_mut(), 1, 0, &mut size);
        }
        if size == 0 {
            return Err(io::Error::last_os_error());
        }
        let mut storage = vec![0usize; size.div_ceil(mem::size_of::<usize>())];
        // SAFETY: aligned storage contains at least the requested byte count.
        if unsafe {
            InitializeProcThreadAttributeList(storage.as_mut_ptr().cast(), 1, 0, &mut size)
        } == 0
        {
            return Err(io::Error::last_os_error());
        }
        Ok(Self(storage))
    }
    fn pointer(&mut self) -> LPPROC_THREAD_ATTRIBUTE_LIST {
        self.0.as_mut_ptr().cast()
    }
}
impl Drop for Attributes {
    fn drop(&mut self) {
        // SAFETY: initialized in new(); storage remains live until after delete.
        unsafe {
            DeleteProcThreadAttributeList(self.pointer());
        }
    }
}

/// Only the connector's fixed revision-admission command can use this boundary.
/// No inherited manager/GUI pipe, file, process or job handle reaches the child.
pub fn spawn_connector(
    application: &Path,
    revision: &str,
    config: &Path,
    removed: &[&str],
) -> io::Result<ConnectorChild> {
    let mut line = command_line(application, revision, config)?;
    let mut application = wide(application.as_os_str())?;
    application.push(0);
    let environment = environment(std::env::vars_os(), removed)?;
    let nul_file = OpenOptions::new().read(true).write(true).open(r"\\.\NUL")?;
    let mut raw_nul = ptr::null_mut();
    // SAFETY: duplicate only this newly opened NUL handle, never mutate a parent
    // std handle. The successful duplicate is owned immediately below.
    if unsafe {
        DuplicateHandle(
            GetCurrentProcess(),
            nul_file.as_raw_handle() as HANDLE,
            GetCurrentProcess(),
            &mut raw_nul,
            0,
            1,
            DUPLICATE_SAME_ACCESS,
        )
    } == 0
    {
        return Err(io::Error::last_os_error());
    }
    let nul = unsafe { OwnedHandle::from_raw_handle(raw_nul) };
    let handles = [nul.as_raw_handle() as HANDLE];
    let mut attributes = Attributes::new()?;
    // SAFETY: the one-entry handle array and its owned NUL handle outlive the
    // attribute list. No manager stdio or other inheritable handle is listed.
    if unsafe {
        UpdateProcThreadAttribute(
            attributes.pointer(),
            0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST as usize,
            handles.as_ptr().cast(),
            mem::size_of_val(&handles),
            ptr::null_mut(),
            ptr::null_mut(),
        )
    } == 0
    {
        return Err(io::Error::last_os_error());
    }
    let startup = STARTUPINFOEXW {
        StartupInfo: windows_sys::Win32::System::Threading::STARTUPINFOW {
            cb: mem::size_of::<STARTUPINFOEXW>() as u32,
            dwFlags: STARTF_USESTDHANDLES,
            hStdInput: handles[0],
            hStdOutput: handles[0],
            hStdError: handles[0],
            ..Default::default()
        },
        lpAttributeList: attributes.pointer(),
    };
    let mut information = PROCESS_INFORMATION::default();
    // SAFETY: every pointer/buffer/handle lives through the call. The extended
    // attribute allowlist restricts TRUE inheritance to our owned NUL handle.
    let created = unsafe {
        CreateProcessW(
            application.as_ptr(),
            line.as_mut_ptr(),
            ptr::null(),
            ptr::null(),
            1,
            CREATE_BREAKAWAY_FROM_JOB
                | CREATE_NEW_PROCESS_GROUP
                | CREATE_NO_WINDOW
                | CREATE_UNICODE_ENVIRONMENT
                | EXTENDED_STARTUPINFO_PRESENT,
            environment.as_ptr().cast(),
            ptr::null(),
            (&startup as *const STARTUPINFOEXW).cast(),
            &mut information,
        )
    };
    if created == 0 {
        return Err(io::Error::last_os_error());
    }
    // SAFETY: CreateProcessW transferred these two new handles to this caller.
    let process = unsafe { OwnedHandle::from_raw_handle(information.hProcess) };
    let thread = unsafe { OwnedHandle::from_raw_handle(information.hThread) };
    drop(thread);
    drop(attributes); // its referenced handle array/NUL remain alive until here
    Ok(ConnectorChild(process))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn exact_admission_argv_quotes_windows_paths_without_loss() {
        let line = command_line(
            Path::new(r"C:\Program Files\Zerus\connector.exe"),
            &"a".repeat(64),
            Path::new(r"\\server\share\Δ folder\connector.json"),
        )
        .unwrap();
        assert_eq!(String::from_utf16(&line[..line.len()-1]).unwrap(),
            format!("\"C:\\Program Files\\Zerus\\connector.exe\" --admit-revision {} --config \"\\\\server\\share\\Δ folder\\connector.json\"", "a".repeat(64)));
        assert_eq!(
            quote(&"C:\\folder with space\\".encode_utf16().collect::<Vec<_>>()),
            "\"C:\\folder with space\\\\\""
                .encode_utf16()
                .collect::<Vec<_>>()
        );
        assert!(command_line(
            Path::new("relative.exe"),
            &"a".repeat(64),
            Path::new(r"C:\c.json")
        )
        .is_err());
        assert!(command_line(Path::new(r"C:\c.exe"), "bad", Path::new(r"C:\c.json")).is_err());
    }
    #[test]
    fn environment_retains_values_and_drive_entries_but_removes_only_exact_context_keys() {
        let entries = [
            ("Path", "C:\\Δ"),
            ("hgs_session", "private"),
            ("HGS_SESSION_EXTRA", "kept"),
            ("=C:", "C:\\folder"),
            ("EMPTY", ""),
        ]
        .map(|(k, v)| (OsString::from(k), OsString::from(v)));
        let block = environment(entries, &["HGS_SESSION"]).unwrap();
        assert_eq!(
            String::from_utf16(&block).unwrap(),
            "=C:=C:\\folder\0EMPTY=\0HGS_SESSION_EXTRA=kept\0Path=C:\\Δ\0\0"
        );
        assert_eq!(environment([], &[]).unwrap(), vec![0, 0]);
    }
}
