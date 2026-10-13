//! Who a process or pipe client is (spec D4, D5): the user SID and integrity level
//! of an access token, this process's Windows session and job placement.
//!
//! `platform::windows` keeps similar token code private to its process helpers;
//! the host needs SID strings and integrity levels as well.
use super::protocol::Integrity;
use std::{
    io,
    os::windows::io::{AsRawHandle, FromRawHandle, OwnedHandle},
};
use windows_sys::Win32::{
    Foundation::{LocalFree, ERROR_NO_TOKEN, HANDLE},
    Security::{
        Authorization::ConvertSidToStringSidW, GetLengthSid, GetSidSubAuthority,
        GetSidSubAuthorityCount, GetTokenInformation, TokenIntegrityLevel, TokenUser, PSID,
        TOKEN_INFORMATION_CLASS, TOKEN_MANDATORY_LABEL, TOKEN_QUERY, TOKEN_USER,
    },
    System::{
        JobObjects::IsProcessInJob,
        RemoteDesktop::ProcessIdToSessionId,
        Threading::{
            GetCurrentProcess, GetCurrentProcessId, GetCurrentThread, OpenProcessToken,
            OpenThreadToken,
        },
    },
};

/// A security identifier in its binary form.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Sid(Vec<u8>);

impl Sid {
    /// # Safety
    /// `sid` points to a valid SID.
    pub(super) unsafe fn copy(sid: PSID) -> Self {
        // SAFETY: the caller guarantees a valid SID, which is GetLengthSid bytes long.
        Self(
            unsafe { std::slice::from_raw_parts(sid as *const u8, GetLengthSid(sid) as usize) }
                .to_vec(),
        )
    }

    /// LocalSystem (`S-1-5-18`), a SID no test process runs as.
    #[cfg(test)]
    pub(super) fn local_system() -> Self {
        Self(vec![1, 1, 0, 0, 0, 0, 0, 5, 18, 0, 0, 0])
    }

    /// The string form, `S-1-5-21-…` for a local account.
    pub fn text(&self) -> io::Result<String> {
        // A copy in u32 units: a SID's sub-authorities need 4-byte alignment.
        let mut aligned = vec![0u32; self.0.len().div_ceil(4)];
        // SAFETY: `aligned` holds at least `self.0.len()` bytes and does not overlap it.
        unsafe {
            std::ptr::copy_nonoverlapping(
                self.0.as_ptr(),
                aligned.as_mut_ptr().cast(),
                self.0.len(),
            )
        };
        let mut text = std::ptr::null_mut();
        // SAFETY: `aligned` holds a valid SID copied from a token; `text` receives a
        // LocalAlloc string that is freed below.
        if unsafe { ConvertSidToStringSidW(aligned.as_mut_ptr().cast(), &mut text) } == 0 {
            return Err(io::Error::last_os_error());
        }
        // SAFETY: on success `text` is a NUL-terminated wide string.
        let length = (0..).take_while(|&i| unsafe { *text.add(i) } != 0).count();
        // SAFETY: `text` holds `length` initialized units before its terminator.
        let result = String::from_utf16(unsafe { std::slice::from_raw_parts(text, length) })
            .map_err(io::Error::other);
        // SAFETY: `text` was allocated by ConvertSidToStringSidW and is not used again.
        unsafe { LocalFree(text.cast()) };
        result
    }
}

/// An access token opened for queries.
pub struct Token(OwnedHandle);

impl Token {
    /// The primary token of `process`, a handle with query access.
    pub fn of_process(process: HANDLE) -> io::Result<Self> {
        let mut token = std::ptr::null_mut();
        // SAFETY: `process` is a process handle the caller keeps open for the call;
        // `token` receives a new handle on success.
        if unsafe { OpenProcessToken(process, TOKEN_QUERY, &mut token) } == 0 {
            return Err(io::Error::last_os_error());
        }
        // SAFETY: OpenProcessToken returned a handle that nothing else owns.
        Ok(Self(unsafe { OwnedHandle::from_raw_handle(token) }))
    }

    /// This process's primary token.
    pub fn current() -> io::Result<Self> {
        // SAFETY: GetCurrentProcess returns a pseudo handle that needs no closing.
        Self::of_process(unsafe { GetCurrentProcess() })
    }

    /// The token this thread impersonates, None while it impersonates nobody.
    /// It is opened with this process's own rights: an identification-level
    /// client token cannot be opened under itself.
    pub fn of_thread() -> io::Result<Option<Self>> {
        let mut token = std::ptr::null_mut();
        // SAFETY: GetCurrentThread is a pseudo handle; `token` receives a new handle
        // on success.
        if unsafe { OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, 1, &mut token) } == 0 {
            let error = io::Error::last_os_error();
            if error.raw_os_error() == Some(ERROR_NO_TOKEN as i32) {
                return Ok(None);
            }
            return Err(error);
        }
        // SAFETY: OpenThreadToken returned a handle that nothing else owns.
        Ok(Some(Self(unsafe { OwnedHandle::from_raw_handle(token) })))
    }

    /// The token information of `class`, in a buffer aligned for its structures.
    fn information(&self, class: TOKEN_INFORMATION_CLASS) -> io::Result<Vec<u64>> {
        let handle = self.0.as_raw_handle() as HANDLE;
        let mut size = 0;
        // SAFETY: a size query with no buffer; it fails and reports the size needed.
        unsafe { GetTokenInformation(handle, class, std::ptr::null_mut(), 0, &mut size) };
        if size == 0 {
            return Err(io::Error::last_os_error());
        }
        let mut buffer = vec![0u64; (size as usize).div_ceil(8)];
        // SAFETY: `buffer` holds at least `size` writable bytes.
        if unsafe {
            GetTokenInformation(handle, class, buffer.as_mut_ptr().cast(), size, &mut size)
        } == 0
        {
            return Err(io::Error::last_os_error());
        }
        Ok(buffer)
    }

    pub fn user(&self) -> io::Result<Sid> {
        let buffer = self.information(TokenUser)?;
        // SAFETY: GetTokenInformation(TokenUser) filled `buffer`, aligned for it, with
        // a TOKEN_USER whose SID points into the same buffer.
        Ok(unsafe { Sid::copy((*buffer.as_ptr().cast::<TOKEN_USER>()).User.Sid) })
    }

    pub fn integrity(&self) -> io::Result<Integrity> {
        let buffer = self.information(TokenIntegrityLevel)?;
        // SAFETY: GetTokenInformation(TokenIntegrityLevel) filled `buffer` with a
        // TOKEN_MANDATORY_LABEL whose SID points into the same buffer.
        let sid = unsafe { (*buffer.as_ptr().cast::<TOKEN_MANDATORY_LABEL>()).Label.Sid };
        // SAFETY: `sid` is the valid label SID of the token.
        let count = unsafe { *GetSidSubAuthorityCount(sid) };
        if count == 0 {
            return Err(io::Error::other("the integrity label has no level"));
        }
        // SAFETY: the level is the label's last sub-authority, which exists.
        Ok(Integrity::from_rid(unsafe {
            *GetSidSubAuthority(sid, u32::from(count) - 1)
        }))
    }

    pub fn identity(&self) -> io::Result<Identity> {
        Ok(Identity {
            user: self.user()?,
            integrity: self.integrity()?,
        })
    }
}

/// A token's user and integrity level: what the host compares clients against.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Identity {
    pub user: Sid,
    pub integrity: Integrity,
}

impl Identity {
    /// This process's.
    pub fn current() -> io::Result<Self> {
        Token::current()?.identity()
    }
}

/// This process's user SID in its string form, as the pipe name hashes it.
pub fn user_sid() -> io::Result<String> {
    Token::current()?.user()?.text()
}

/// The Windows session this process runs in.
pub fn windows_session() -> io::Result<u32> {
    let mut session = 0;
    // SAFETY: plain call with an out pointer to a local.
    if unsafe { ProcessIdToSessionId(GetCurrentProcessId(), &mut session) } == 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(session)
}

/// Whether this process is inside any job (D4). A host checks it before it
/// creates its own job.
pub fn in_job() -> io::Result<bool> {
    let mut inside = 0;
    // SAFETY: the pseudo handle of this process, no job (any job counts) and an out
    // pointer to a local.
    if unsafe { IsProcessInJob(GetCurrentProcess(), std::ptr::null_mut(), &mut inside) } == 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(inside != 0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn this_process_has_a_user_level_session_and_job_placement() {
        let identity = Identity::current().unwrap();
        let text = identity.user.text().unwrap();
        // Local and Microsoft accounts are S-1-5-21-…, Entra ID accounts S-1-12-1-….
        assert!(text.starts_with("S-1-"), "{text}");
        assert_eq!(user_sid().unwrap(), text);
        assert_eq!(Sid::local_system().text().unwrap(), "S-1-5-18");
        assert_ne!(identity.user, Sid::local_system());
        assert_eq!(identity, Token::current().unwrap().identity().unwrap());
        // A user process runs at least at low integrity; the name follows the RID.
        assert!(identity.integrity.rid >= Integrity::LOW);
        assert_eq!(
            identity.integrity,
            Integrity::from_rid(identity.integrity.rid)
        );
        windows_session().unwrap();
        // CI runners may start tests inside a job; only the call is checked.
        in_job().unwrap();
        // This thread impersonates nobody.
        assert!(Token::of_thread().unwrap().is_none());
    }
}
