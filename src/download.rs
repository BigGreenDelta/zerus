//! Explicit remote file downloads. SFTP preserves path bytes; local publication
//! is atomic and never exposes a partial transfer as the requested file.
use crate::{
    cli::{Error, Result},
    config::Config,
    platform,
};
use serde_json::json;
use std::{
    fs,
    io::{self, Read},
    path::Path,
    process::{Command, Stdio},
    sync::mpsc,
    thread,
    time::{Duration, Instant},
};

pub fn dispatch(config: &Config, host: &str, args: &[String], dry: bool) -> Result<i32> {
    if dry {
        return Err(Error::new(1, "Downloads do not support --dry-run"));
    }
    if host.is_empty()
        || config.is_self(host)
        || !config.peers.iter().any(|p| p == host)
        || host.starts_with('-')
        || !host
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || b"_.-".contains(&c))
    {
        return Err(Error::new(
            1,
            "Choose a configured remote machine for this download.",
        ));
    }
    let (mut path, mut output) = (None, None);
    for pair in args.chunks(2) {
        if pair.len() != 2 || pair[1].is_empty() {
            return Err(usage());
        }
        match pair[0].as_str() {
            "--path" if path.is_none() => path = Some(pair[1].as_str()),
            "--output" if output.is_none() => output = Some(pair[1].as_str()),
            _ => return Err(usage()),
        }
    }
    let (Some(path), Some(output)) = (path, output) else {
        return Err(usage());
    };
    if !(path.starts_with('/') || path.starts_with("~/"))
        || path.len() > 32768
        || path.chars().any(char::is_control)
    {
        return Err(Error::new(
            1,
            "The remote file needs an absolute path or a path starting with ~/ .",
        ));
    }
    let destination = Path::new(output);
    if destination.file_name().is_none() || destination.is_dir() {
        return Err(Error::new(1, "Choose a local file destination."));
    }
    let parent = destination
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    let temporary = tempfile::NamedTempFile::new_in(parent)
        .map_err(|e| Error::new(1, format!("Cannot prepare the local download: {e}")))?;
    let ssh = platform::ssh_program();
    // Keep Windows on the same OpenSSH distribution as its selected ssh.exe.
    let sibling = Path::new(&ssh).with_file_name(if cfg!(windows) { "scp.exe" } else { "scp" });
    let mut command = Command::new(if sibling.is_file() {
        sibling
    } else {
        "scp".into()
    });
    // -s explicitly selects SFTP. Never fall back to legacy SCP's remote shell
    // interpretation of filenames, and never interpolate paths into a shell.
    command.args(["-s", "-q", "-B", "-S"]).arg(ssh).args([
        "-o",
        "BatchMode=yes",
        "-o",
        "StrictHostKeyChecking=yes",
        "-o",
        "ConnectTimeout=8",
        "-o",
        "ServerAliveInterval=10",
        "-o",
        "ServerAliveCountMax=2",
    ]);
    let options = crate::machines::ssh_options(config, host);
    for pair in options.chunks_exact(2) {
        match pair[0].as_str() {
            "-l" => {
                command.args(["-o", &format!("User={}", pair[1])]);
            }
            "-p" => {
                command.args(["-P", &pair[1]]);
            }
            _ => {
                command.args(pair);
            }
        }
    }
    command
        .arg("--")
        .arg(format!("{host}:{}", sftp_path(path)))
        .arg(temporary.path());
    transfer(command, Duration::from_secs(120))?;
    platform::make_private(temporary.as_file())?;
    temporary.as_file().sync_all()?;
    let bytes = fs::metadata(temporary.path())?.len();
    platform::persist(temporary, destination)
        .map_err(|e| Error::new(1, format!("Cannot save the downloaded file: {}", e.error)))?;
    println!(
        "{}",
        json!({"host":host,"path":path,"local_path":output,"bytes":bytes})
    );
    Ok(0)
}

fn usage() -> Error {
    Error::new(
        1,
        "usage: hgs @host download --path REMOTE_PATH --output LOCAL_FILE",
    )
}

fn sftp_path(path: &str) -> String {
    let mut literal = String::new();
    for ch in path.chars() {
        if matches!(ch, '\\' | '*' | '?' | '[' | ']') {
            literal.push('\\');
        }
        literal.push(ch);
    }
    literal
}

fn transfer(mut command: Command, timeout: Duration) -> Result<()> {
    command.stdin(Stdio::null()).stdout(Stdio::null());
    platform::own_process_group(&mut command);
    let mut ssh = platform::spawn_ssh(&mut command, false, false, true).map_err(|e| {
        Error::new(
            1,
            format!("Cannot start the file transfer (OpenSSH scp is required): {e}"),
        )
    })?;
    let mut errors = ssh.stderr.take().unwrap();
    let (sender, receiver) = mpsc::channel();
    thread::spawn(move || {
        let mut text = Vec::new();
        let _ = errors.by_ref().take(4096).read_to_end(&mut text);
        let _ = io::copy(&mut errors, &mut io::sink());
        let _ = sender.send(text);
    });
    let deadline = Instant::now() + timeout;
    let result = loop {
        match ssh.child.try_wait() {
            Ok(Some(status)) => break Ok(status),
            Err(e) => break Err(Error::from(e)),
            _ if Instant::now() >= deadline => {
                break Err(Error::new(
                    1,
                    "Download timed out. The local destination was not changed.",
                ))
            }
            _ => thread::sleep(Duration::from_millis(20)),
        }
    };
    if result.is_err() {
        platform::kill_process_group(&mut ssh.child);
        let _ = ssh.child.wait();
    }
    let error = receiver
        .recv_timeout(Duration::from_secs(1))
        .unwrap_or_default();
    if result?.success() {
        return Ok(());
    }
    let detail = String::from_utf8_lossy(&error);
    let detail = detail.trim();
    Err(Error::new(
        1,
        if detail.is_empty() {
            "The file could not be downloaded. Check the path and SSH connection.".into()
        } else {
            detail.to_owned()
        },
    ))
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;
    #[test]
    fn sftp_globs_remain_literal_file_names() {
        assert_eq!(
            sftp_path("/files/report [1]*?.txt"),
            "/files/report \\[1\\]\\*\\?.txt"
        );
        assert_eq!(sftp_path("~/file\\name"), "~/file\\\\name");
    }
    #[test]
    fn timeout_reaps_owned_transfer_and_reports_failure() {
        let mut command = Command::new("sh");
        command.args(["-c", "sleep 30 & wait"]);
        let started = Instant::now();
        let error = transfer(command, Duration::from_millis(100)).unwrap_err();
        assert!(error.message.contains("timed out"));
        assert!(started.elapsed() < Duration::from_secs(3));
    }
}
