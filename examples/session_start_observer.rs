//! Fixture-only hook interposition. Never install this as the stable CLI or a
//! supervisor. A separate copy named hgs[.exe] reads observer.json beside it.
//! It runs one standalone, phase-sealed real hgs and keeps bounded observations.
//! Streams retain their separate byte order; cross-stream timing/order and
//! fault-path equivalence to a direct native pipe are not promised.
use base64::{engine::general_purpose::STANDARD, Engine};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::{
    env, fs,
    io::{self, Read, Write},
    path::{Path, PathBuf},
    process::{self, Command, ExitStatus, Stdio},
    thread,
    time::{SystemTime, UNIX_EPOCH},
};

const CAPTURE: usize = 64 * 1024;
const CALLS: usize = 256;

fn bounded(path: &Path, limit: usize) -> io::Result<Vec<u8>> {
    let mut data = Vec::new();
    fs::File::open(path)?.take((limit + 1) as u64).read_to_end(&mut data)?;
    if data.len() > limit {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "observer input bound"));
    }
    Ok(data)
}

fn hash(path: &Path) -> io::Result<String> {
    let mut file = fs::File::open(path)?;
    let mut digest = Sha256::new();
    let mut bytes = [0; 8192];
    loop {
        let count = file.read(&mut bytes)?;
        if count == 0 { break; }
        digest.update(&bytes[..count]);
    }
    Ok(format!("{:x}", digest.finalize()))
}

fn record(path: &Path, value: &Value) -> io::Result<()> {
    let mut options = fs::OpenOptions::new();
    options.write(true).create_new(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.mode(0o600);
    }
    let mut file = options.open(path)?;
    serde_json::to_writer(&mut file, value)?;
    file.write_all(b"\n")?;
    file.sync_all()
}

#[derive(Default)]
struct Stream {
    captured: Vec<u8>,
    bytes: u64,
    forwarded: u64,
    error: bool,
    digest: Sha256,
}

impl Stream {
    fn observation(self) -> Value {
        json!({"bytes":self.bytes,"forwarded":self.forwarded,"error":self.error,
               "overflow":self.bytes > CAPTURE as u64,"base64":STANDARD.encode(self.captured),
               "sha256":format!("{:x}",self.digest.finalize())})
    }
}

fn pump(mut input: impl Read, mut output: impl Write) -> Stream {
    let mut result = Stream::default();
    let mut bytes = [0; 8192];
    let mut writable = true;
    loop {
        let count = match input.read(&mut bytes) {
            Ok(0) => break,
            Ok(count) => count,
            Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
            Err(_) => { result.error = true; break; }
        };
        result.bytes += count as u64;
        result.digest.update(&bytes[..count]);
        let keep = count.min(CAPTURE - result.captured.len());
        result.captured.extend_from_slice(&bytes[..keep]);
        // The capture ceiling never truncates the forwarded stream. After a
        // broken destination, drain the child but mark this observation unknown.
        let mut offset = 0;
        while writable && offset < count {
            match output.write(&bytes[offset..count]) {
                Ok(0) => { writable = false; result.error = true; }
                Ok(written) => { offset += written; result.forwarded += written as u64; }
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                Err(_) => { writable = false; result.error = true; }
            }
        }
    }
    if output.flush().is_err() { result.error = true; }
    result
}

#[cfg(target_os = "linux")]
fn identity(child: &process::Child) -> Option<Value> {
    let path = PathBuf::from(format!("/proc/{}", child.id()));
    let data = bounded(&path.join("stat"), 4096).ok()?;
    let text = std::str::from_utf8(&data).ok()?;
    let fields: Vec<_> = text.rsplit_once(')')?.1.split_whitespace().collect();
    let birth = fields.get(19)?.parse::<u64>().ok()?;
    let image = fs::read_link(path.join("exe")).ok()?;
    Some(json!({"pid":child.id(),"birth":birth.to_string(),"image":image}))
}

#[cfg(all(unix, not(target_os = "linux")))]
fn identity(child: &process::Child) -> Option<Value> {
    // A read-only process query, not a native-agent capability probe. The real
    // hook still waits for stdin, which is forwarded only after this snapshot.
    let output = Command::new("/bin/ps")
        .args(["-ww", "-p", &child.id().to_string(), "-o", "lstart=,comm="])
        .env("LC_ALL", "C").env("TZ", "UTC").output().ok()?;
    if !output.status.success() || output.stdout.len() > 1024 { return None; }
    let text = std::str::from_utf8(&output.stdout).ok()?.trim();
    let fields: Vec<_> = text.split_whitespace().collect();
    if fields.len() < 6 { return None; }
    Some(json!({"pid":child.id(),"birth":fields[..5].join(" "),"image":fields[5..].join(" ")}))
}

#[cfg(windows)]
fn identity(child: &process::Child) -> Option<Value> {
    use std::os::windows::io::AsRawHandle;
    use windows_sys::Win32::{Foundation::FILETIME, System::Threading::{GetProcessTimes, QueryFullProcessImageNameW}};
    let handle = child.as_raw_handle();
    let [mut created, mut exited, mut kernel, mut user]: [FILETIME; 4] = unsafe { std::mem::zeroed() };
    let mut image = [0u16; 32768];
    let mut size = image.len() as u32;
    let result = unsafe {
        GetProcessTimes(handle, &mut created, &mut exited, &mut kernel, &mut user) != 0
            && QueryFullProcessImageNameW(handle, 0, image.as_mut_ptr(), &mut size) != 0
    };
    if !result { return None; }
    let birth = (u64::from(created.dwHighDateTime) << 32) | u64::from(created.dwLowDateTime);
    Some(json!({"pid":child.id(),"birth":birth.to_string(),"image":String::from_utf16(&image[..size as usize]).ok()?}))
}

fn exit_like(status: ExitStatus) -> ! {
    #[cfg(unix)]
    {
        use std::os::unix::process::ExitStatusExt;
        if let Some(signal) = status.signal() {
            // Reproduce the child's termination on this observer only. Never
            // signal a numeric child PID or any other process.
            unsafe { libc::signal(signal, libc::SIG_DFL); libc::raise(signal); }
            process::exit(128 + signal);
        }
    }
    process::exit(status.code().unwrap_or(1));
}

fn run() -> io::Result<ExitStatus> {
    let executable = env::current_exe()?;
    let config: Value = serde_json::from_slice(&bounded(
        &executable.parent().ok_or_else(|| io::Error::other("observer folder"))?.join("observer.json"), 8192)?)?;
    let subject = PathBuf::from(config["subject"].as_str().ok_or_else(|| io::Error::other("observer subject"))?);
    let records = PathBuf::from(config["records"].as_str().ok_or_else(|| io::Error::other("observer records"))?);
    let arguments: Vec<_> = env::args_os().skip(1).collect();
    let strings: Vec<_> = arguments.iter().map(|v| v.to_str()).collect();
    let hook = strings == [Some("__state"), Some("hook")];
    let channel = strings == [Some("__state"), Some("answer-channel")];
    if !subject.is_absolute() || !records.is_absolute() || (!hook && !channel)
        || hash(&subject)? != config["sha256"].as_str().unwrap_or("") {
        return Err(io::Error::other("observer input identity"));
    }
    if channel {
        // A retained real answer-channel is preferred; this defensive delegate
        // neither observes its payload nor synthesizes an answer or receipt.
        return Command::new(&subject).args(arguments).status();
    }
    let mut recording_error = false;
    let folder = records.join(uuid::Uuid::new_v4().to_string());
    let mut entries = fs::read_dir(&records)?;
    if entries.by_ref().take(CALLS + 1).count() >= CALLS {
        recording_error = true;
        let _ = record(&records.join("overflow.json"), &json!({"unknown":"call bound"}));
    } else if fs::create_dir(&folder).is_err() {
        recording_error = true;
    }
    #[cfg(unix)]
    if !recording_error {
        use std::os::unix::fs::PermissionsExt;
        if fs::set_permissions(&folder, fs::Permissions::from_mode(0o700)).is_err() { recording_error = true; }
    }
    if record(&folder.join("begin.json"), &json!({
        "observer_pid":process::id(),"phase":config["phase"],"source":config["source"],
        "subject":subject,"expected_sha256":config["sha256"],"argv":["__state","hook"],
        "cwd":env::current_dir()?,"run":env::var("HGS_RUN_ID").ok(),
        "name":env::var("HGS_SESSION").ok(),
        "started_unix_ns":SystemTime::now().duration_since(UNIX_EPOCH).ok().map(|v|v.as_nanos().to_string())
    })).is_err() { recording_error = true; }
    let mut child = Command::new(&subject).args(arguments).stdin(Stdio::piped())
        .stdout(Stdio::piped()).stderr(Stdio::piped()).spawn()?;
    let child_identity = identity(&child);
    let input = child.stdin.take().ok_or_else(|| io::Error::other("observer stdin"))?;
    let output = child.stdout.take().ok_or_else(|| io::Error::other("observer stdout"))?;
    let error = child.stderr.take().ok_or_else(|| io::Error::other("observer stderr"))?;
    let input = thread::spawn(move || pump(io::stdin().lock(), input));
    let output = thread::spawn(move || pump(output, io::stdout().lock()));
    let error = thread::spawn(move || pump(error, io::stderr().lock()));
    let status = child.wait()?;
    let input = input.join().map_err(|_| io::Error::other("observer stdin thread"))?.observation();
    let output = output.join().map_err(|_| io::Error::other("observer stdout thread"))?.observation();
    let error = error.join().map_err(|_| io::Error::other("observer stderr thread"))?.observation();
    let after = hash(&subject).ok();
    // Missing/incomplete evidence fails the case, never changes the child's
    // exit status, retries the hook, or rolls back its suppression metadata.
    let _ = record(&folder.join("result.json"), &json!({
        "child":child_identity,"exit_code":status.code(),
        "signal":signal_of(&status),"stdin":input,"stdout":output,"stderr":error,
        "subject_sha256_after":after,"recording_error":recording_error
    }));
    Ok(status)
}

fn signal_of(status: &ExitStatus) -> Option<i32> {
    #[cfg(unix)]
    {
        use std::os::unix::process::ExitStatusExt;
        status.signal()
    }
    #[cfg(not(unix))]
    { let _ = status; None }
}

fn main() {
    match run() {
        Ok(status) => exit_like(status),
        Err(_) => { eprintln!("fixture hook observer: unknown observation"); process::exit(125); }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn capture_ceiling_never_truncates_forwarding() {
        let source = vec![0xa5; CAPTURE + 8193];
        let mut forwarded = Vec::new();
        let observed = pump(&source[..], &mut forwarded);
        assert_eq!(forwarded, source);
        assert_eq!(observed.captured.len(), CAPTURE);
        assert_eq!(observed.bytes, source.len() as u64);
        assert_eq!(observed.forwarded, source.len() as u64);
        assert!(!observed.error);
        assert_eq!(observed.observation()["overflow"], true);
    }

    #[test]
    fn short_writes_preserve_each_stream_order() {
        struct Short(Vec<u8>);
        impl Write for Short {
            fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
                let count = bytes.len().min(3);
                self.0.extend_from_slice(&bytes[..count]);
                Ok(count)
            }
            fn flush(&mut self) -> io::Result<()> { Ok(()) }
        }
        let mut forwarded = Short(Vec::new());
        let observed = pump(&b"\0one\r\ntwo\xff"[..], &mut forwarded);
        assert_eq!(forwarded.0, b"\0one\r\ntwo\xff");
        assert_eq!(observed.bytes, observed.forwarded);
        assert!(!observed.error);
    }

    #[test]
    fn failed_forwarding_is_unknown_while_child_output_is_drained() {
        struct Broken;
        impl Write for Broken {
            fn write(&mut self, _: &[u8]) -> io::Result<usize> { Err(io::ErrorKind::BrokenPipe.into()) }
            fn flush(&mut self) -> io::Result<()> { Ok(()) }
        }
        let observed = pump(&b"actual child output"[..], Broken);
        assert!(observed.error);
        assert_eq!(observed.bytes, 19);
        assert_eq!(observed.forwarded, 0);
        assert_eq!(observed.captured, b"actual child output");
    }

    #[test]
    fn separate_stream_observations_do_not_claim_cross_stream_order() {
        let mut first = Vec::new();
        let mut second = Vec::new();
        let out = pump(&b"stdout"[..], &mut first).observation();
        let err = pump(&b"stderr"[..], &mut second).observation();
        assert_eq!(first, b"stdout");
        assert_eq!(second, b"stderr");
        assert_ne!(out["sha256"], err["sha256"]);
    }
}
