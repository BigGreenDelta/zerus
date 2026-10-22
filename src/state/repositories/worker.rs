//! Unix Git has a deadline owner independent of the disposable GUI/SSH wrapper.
//! The wrapper holds the worker's stdin open. EOF cancels only its Git group;
//! Git receives /dev/null, so it cannot inherit and retain that lifeline.
use super::*;
use std::io::{Read, Write};
use std::os::unix::process::CommandExt;
use std::process::Stdio;

fn image_proof(identity: crate::platform::FileIdentity) -> String {
    format!(
        "{}:{}:{}:{}",
        identity.device, identity.inode, identity.modified, identity.modified_nanos
    )
}

fn admit_image(
    running: Option<crate::platform::FileIdentity>,
    candidate: Option<crate::platform::FileIdentity>,
) -> Result<String> {
    let running = running.ok_or("Cannot verify the running hgs image")?;
    let candidate = candidate.ok_or("Cannot verify the Git worker executable")?;
    if running != candidate {
        return Err("hgs changed during acquisition; reopen the dialog".into());
    }
    Ok(image_proof(running))
}

fn verify_image(expected: &str, actual: Option<crate::platform::FileIdentity>) -> Result<()> {
    let actual = actual.ok_or("Cannot verify the running Git worker image")?;
    if image_proof(actual) != expected {
        return Err("Git worker image changed before launch; reopen the dialog".into());
    }
    Ok(())
}

pub(super) fn run(
    directory: &directory::Directory,
    args: &[&str],
    seconds: u64,
) -> Result<Vec<u8>> {
    #[cfg(target_os = "linux")]
    let executable = PathBuf::from("/proc/self/exe");
    #[cfg(not(target_os = "linux"))]
    let executable = std::env::current_exe().map_err(|e| e.to_string())?;
    let proof = admit_image(
        crate::platform::image_identity(),
        crate::platform::file_identity(&executable),
    )?;
    let mut command = Command::new(executable);
    command
        .args([
            "__state",
            "repository-git-worker",
            &seconds.to_string(),
            &proof,
        ])
        .args(args)
        .process_group(0)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null());
    directory.anchor(&mut command)?;
    let mut child = crate::platform::spawn_in_turn(&mut command)
        .map_err(|e| format!("Cannot start Git worker: {e}"))?;
    let lifeline = child.stdin.take();
    let mut encoded = Vec::new();
    // Git's two 64 KiB byte arrays / escaped error fit within this envelope.
    let read = child
        .stdout
        .take()
        .unwrap()
        .take(524_289)
        .read_to_end(&mut encoded);
    drop(lifeline); // Also cancel on a broken/oversized worker result.
    let status = child
        .wait()
        .map_err(|e| format!("Cannot wait for Git worker: {e}"))?;
    read.map_err(|e| format!("Cannot read Git worker: {e}"))?;
    if !status.success() || encoded.len() > 524_288 {
        return Err("Git worker interrupted; inspect the destination before continuing".into());
    }
    serde_json::from_slice::<Result<Vec<u8>>>(&encoded)
        .map_err(|_| "Invalid Git worker result".to_string())?
}

pub(super) fn dispatch(args: &[String]) -> Result<i32> {
    let seconds: u64 = args
        .first()
        .ok_or("Missing Git deadline")?
        .parse()
        .map_err(|_| "Invalid Git deadline")?;
    if !(1..=600).contains(&seconds) || args.len() < 3 {
        return Err("Invalid Git worker arguments".into());
    }
    // The executable pathname can change after the parent's check. Bind this
    // actual running image before any Git work, including on macOS upgrades.
    // Unknown image evidence never counts as equality.
    verify_image(&args[1], crate::platform::image_identity())?;
    // Refuse invocation without a wrapper-owned pipe. This is an internal ABI,
    // never an alternative public route for unbounded interactive Git.
    unsafe {
        let mut stat: libc::stat = std::mem::zeroed();
        if libc::fstat(0, &mut stat) != 0 || stat.st_mode & libc::S_IFMT != libc::S_IFIFO {
            return Err("Git worker requires a wrapper lifeline".into());
        }
        let flags = libc::fcntl(0, libc::F_GETFL);
        if flags < 0 || libc::fcntl(0, libc::F_SETFL, flags | libc::O_NONBLOCK) < 0 {
            return Err("Cannot watch Git wrapper lifetime".into());
        }
    }
    let cancelled = || unsafe {
        let mut byte = 0u8;
        match libc::read(0, (&mut byte as *mut u8).cast(), 1) {
            0 => true,
            -1 => !matches!(
                std::io::Error::last_os_error().kind(),
                std::io::ErrorKind::WouldBlock | std::io::ErrorKind::Interrupted
            ),
            _ => true, // The protocol carries no input.
        }
    };
    let refs: Vec<&str> = args[2..].iter().map(String::as_str).collect();
    let result = run_git(None, &refs, seconds, &cancelled);
    // All Git work is finished before writing. A dead reader cannot leave an
    // operation running while this worker blocks or exits on a broken pipe.
    let encoded = serde_json::to_vec(&result).map_err(|e| e.to_string())?;
    std::io::stdout()
        .write_all(&encoded)
        .map_err(|e| e.to_string())?;
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn identity() -> crate::platform::FileIdentity {
        crate::platform::FileIdentity {
            device: 1,
            inode: 2,
            modified: 3,
            modified_nanos: 4,
        }
    }

    #[test]
    fn image_admission_requires_both_known_identities() {
        let id = identity();
        assert!(admit_image(None, None).is_err());
        assert!(admit_image(Some(id), None).is_err());
        assert!(admit_image(None, Some(id)).is_err());
        assert_eq!(admit_image(Some(id), Some(id)).unwrap(), image_proof(id));
    }

    #[test]
    fn image_admission_compares_the_complete_identity() {
        let id = identity();
        for other in [
            crate::platform::FileIdentity { device: 5, ..id },
            crate::platform::FileIdentity { inode: 5, ..id },
            crate::platform::FileIdentity { modified: 5, ..id },
            crate::platform::FileIdentity {
                modified_nanos: 5,
                ..id
            },
        ] {
            assert!(admit_image(Some(id), Some(other)).is_err());
        }
    }

    #[test]
    fn worker_rechecks_known_actual_image_before_git() {
        let id = identity();
        let proof = image_proof(id);
        assert!(verify_image("", None).is_err());
        assert!(verify_image(&proof, None).is_err());
        assert!(verify_image(
            &proof,
            Some(crate::platform::FileIdentity { inode: 9, ..id })
        )
        .is_err());
        verify_image(&proof, Some(id)).unwrap();
    }
}
