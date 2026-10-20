//! Small Windows boundary. Callers remain safe Rust. Existing unsafe entries
//! are refused; ACLs are applied at creation, never repaired after opening.
#![cfg(windows)]
mod connector_process;
pub use connector_process::{spawn_connector, ConnectorChild};
use std::fs::{File, OpenOptions};
use std::io;
use std::os::windows::{ffi::OsStrExt, fs::OpenOptionsExt, io::AsRawHandle};
use std::path::Path;
use windows_sys::Win32::{
    Foundation::{CloseHandle, LocalFree, ERROR_ALREADY_EXISTS, ERROR_SUCCESS, HANDLE},
    Security::{
        Authorization::{
            ConvertSidToStringSidW, ConvertStringSecurityDescriptorToSecurityDescriptorW,
            GetSecurityInfo, SE_FILE_OBJECT,
        },
        GetAce, GetLengthSid, GetTokenInformation, IsWellKnownSid, TokenUser,
        WinBuiltinAdministratorsSid, WinLocalSystemSid, ACCESS_ALLOWED_ACE, ACE_HEADER, ACL,
        DACL_SECURITY_INFORMATION, INHERIT_ONLY_ACE, OWNER_SECURITY_INFORMATION,
        PSECURITY_DESCRIPTOR, PSID, SECURITY_ATTRIBUTES, TOKEN_QUERY, TOKEN_USER,
    },
    Storage::FileSystem::{
        CreateDirectoryW, GetFileInformationByHandle, BY_HANDLE_FILE_INFORMATION,
        FILE_ATTRIBUTE_DIRECTORY, FILE_ATTRIBUTE_REPARSE_POINT, FILE_FLAG_BACKUP_SEMANTICS,
        FILE_FLAG_OPEN_REPARSE_POINT, FILE_READ_ATTRIBUTES, FILE_SHARE_DELETE, FILE_SHARE_READ,
        FILE_SHARE_WRITE,
    },
    System::Threading::{GetCurrentProcess, OpenProcessToken},
};

const READ_CONTROL: u32 = 0x00020000;
fn refused() -> io::Error {
    io::Error::other("owned private Windows file or directory required")
}
fn wide(path: &Path) -> Vec<u16> {
    path.as_os_str().encode_wide().chain(Some(0)).collect()
}
struct Allocation(*mut core::ffi::c_void);
impl Drop for Allocation {
    fn drop(&mut self) {
        unsafe {
            LocalFree(self.0);
        }
    }
}
struct Handle(HANDLE);
impl Drop for Handle {
    fn drop(&mut self) {
        unsafe {
            CloseHandle(self.0);
        }
    }
}

fn user() -> io::Result<Vec<u8>> {
    let mut raw = std::ptr::null_mut();
    // SAFETY: valid process handle and writable output token handle.
    if unsafe { OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut raw) } == 0 {
        return Err(io::Error::last_os_error());
    }
    let token = Handle(raw);
    let mut size = 0;
    unsafe {
        GetTokenInformation(token.0, TokenUser, std::ptr::null_mut(), 0, &mut size);
    }
    let mut data = vec![0u64; (size as usize).div_ceil(8)];
    if size == 0
        || unsafe {
            GetTokenInformation(
                token.0,
                TokenUser,
                data.as_mut_ptr().cast(),
                size,
                &mut size,
            )
        } == 0
    {
        return Err(io::Error::last_os_error());
    }
    // SAFETY: GetTokenInformation returned a TOKEN_USER and its SID in data.
    let sid = unsafe { (*(data.as_ptr() as *const TOKEN_USER)).User.Sid };
    Ok(unsafe { sid_bytes(sid) }.to_vec())
}
unsafe fn sid_bytes<'a>(sid: PSID) -> &'a [u8] {
    // SAFETY: caller provides a SID from a successful Windows security call.
    unsafe { std::slice::from_raw_parts(sid.cast(), GetLengthSid(sid) as usize) }
}
fn descriptor() -> io::Result<Allocation> {
    let user = user()?;
    let mut text = std::ptr::null_mut();
    if unsafe { ConvertSidToStringSidW(user.as_ptr() as PSID, &mut text) } == 0 {
        return Err(io::Error::last_os_error());
    }
    let _text = Allocation(text.cast());
    let mut len = 0;
    while unsafe { *text.add(len) } != 0 {
        len += 1;
    }
    let sid = String::from_utf16(unsafe { std::slice::from_raw_parts(text, len) })
        .map_err(|_| refused())?;
    let sddl: Vec<u16> = format!("O:{sid}D:P(A;OICI;FA;;;{sid})(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)")
        .encode_utf16()
        .chain(Some(0))
        .collect();
    let mut sd = std::ptr::null_mut();
    if unsafe {
        ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.as_ptr(),
            1,
            &mut sd,
            std::ptr::null_mut(),
        )
    } == 0
    {
        return Err(io::Error::last_os_error());
    }
    Ok(Allocation(sd))
}

/// Validate the opened object, including its real ACL rather than Unix mode bits.
pub fn verify(file: &File, directory: bool) -> io::Result<()> {
    let handle = file.as_raw_handle() as HANDLE;
    let mut info: BY_HANDLE_FILE_INFORMATION = unsafe { std::mem::zeroed() };
    if unsafe { GetFileInformationByHandle(handle, &mut info) } == 0 {
        return Err(io::Error::last_os_error());
    }
    if info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT != 0
        || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY != 0) != directory
        || (!directory && info.nNumberOfLinks != 1)
    {
        return Err(refused());
    }
    let mut owner: PSID = std::ptr::null_mut();
    let mut acl: *mut ACL = std::ptr::null_mut();
    let mut sd: PSECURITY_DESCRIPTOR = std::ptr::null_mut();
    let code = unsafe {
        GetSecurityInfo(
            handle,
            SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            &mut owner,
            std::ptr::null_mut(),
            &mut acl,
            std::ptr::null_mut(),
            &mut sd,
        )
    };
    if code != ERROR_SUCCESS {
        return Err(io::Error::from_raw_os_error(code as i32));
    }
    let _sd = Allocation(sd);
    let user = user()?;
    if owner.is_null() || acl.is_null() || unsafe { sid_bytes(owner) } != user {
        return Err(refused());
    }
    for index in 0..u32::from(unsafe { (*acl).AceCount }) {
        let mut ace = std::ptr::null_mut();
        if unsafe { GetAce(acl, index, &mut ace) } == 0 {
            return Err(io::Error::last_os_error());
        }
        let header = unsafe { &*(ace as *const ACE_HEADER) };
        // An inherit-only allow is inert on a file, but a directory can pass
        // it to SQLite sidecars created with the default security descriptor.
        // Check its recipient before admitting the directory for any write.
        if (!directory && u32::from(header.AceFlags) & INHERIT_ONLY_ACE != 0) || header.AceType == 1
        {
            continue;
        }
        if header.AceType != 0 {
            return Err(refused());
        }
        let sid =
            unsafe { std::ptr::addr_of!((*(ace as *const ACCESS_ALLOWED_ACE)).SidStart) } as PSID;
        if !unsafe {
            sid_bytes(sid) == user
                || IsWellKnownSid(sid, WinLocalSystemSid) != 0
                || IsWellKnownSid(sid, WinBuiltinAdministratorsSid) != 0
        } {
            return Err(refused());
        }
    }
    Ok(())
}

/// Hold every ancestor without delete sharing. A junction cannot be swapped
/// into the path while a management write or running journal owns this guard.
pub struct DirectoryGuard {
    _handles: Vec<File>,
}
pub fn directory(path: &Path, create: bool) -> io::Result<DirectoryGuard> {
    if !path.is_absolute() {
        return Err(refused());
    }
    let mut current = std::path::PathBuf::new();
    let mut handles = Vec::new();
    for part in path.components() {
        use std::path::Component;
        if matches!(part, Component::ParentDir | Component::CurDir) {
            return Err(refused());
        }
        current.push(part);
        if !current.has_root() {
            continue;
        }
        let open = || {
            OpenOptions::new()
                .access_mode(READ_CONTROL | FILE_READ_ATTRIBUTES)
                .share_mode(FILE_SHARE_READ | FILE_SHARE_WRITE)
                .custom_flags(FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT)
                .open(&current)
        };
        let file = match open() {
            Ok(file) => file,
            Err(e) if create && e.kind() == io::ErrorKind::NotFound => {
                let sd = descriptor()?;
                let sa = SECURITY_ATTRIBUTES {
                    nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32,
                    lpSecurityDescriptor: sd.0,
                    bInheritHandle: 0,
                };
                if unsafe { CreateDirectoryW(wide(&current).as_ptr(), &sa) } == 0
                    && io::Error::last_os_error().raw_os_error()
                        != Some(ERROR_ALREADY_EXISTS as i32)
                {
                    return Err(io::Error::last_os_error());
                }
                open()?
            }
            Err(e) => return Err(e),
        };
        let mut info: BY_HANDLE_FILE_INFORMATION = unsafe { std::mem::zeroed() };
        if unsafe { GetFileInformationByHandle(file.as_raw_handle() as HANDLE, &mut info) } == 0 {
            return Err(io::Error::last_os_error());
        }
        if info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT != 0
            || info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY == 0
        {
            return Err(refused());
        }
        if current == path {
            verify(&file, true)?;
        }
        handles.push(file);
    }
    if handles.is_empty() {
        return Err(refused());
    }
    Ok(DirectoryGuard { _handles: handles })
}
pub fn open_file(path: &Path, write: bool, create: bool) -> io::Result<File> {
    let mut options = OpenOptions::new();
    options
        .read(true)
        .write(write)
        .create(create)
        .share_mode(FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)
        .custom_flags(FILE_FLAG_OPEN_REPARSE_POINT);
    // The parent has a validated protected inheritable DACL. No security
    // descriptor is changed after opening, including for an existing object.
    let file = options.open(path)?;
    verify(&file, false)?;
    Ok(file)
}

/// Atomic publication of a prepared private file on the same volume.
pub fn replace(from: &Path, to: &Path) -> io::Result<()> {
    use windows_sys::Win32::Storage::FileSystem::{
        MoveFileExW, MOVEFILE_REPLACE_EXISTING, MOVEFILE_WRITE_THROUGH,
    };
    if unsafe {
        MoveFileExW(
            wide(from).as_ptr(),
            wide(to).as_ptr(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
        )
    } == 0
    {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}
