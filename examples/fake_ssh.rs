//! A stand-in for the OpenSSH client in hgs integration tests, injected through
//! `HGS_SSH`. It never opens a connection and is never installed or packaged.
//! A single executable (no script interpreter) keeps argv, stdin and exit codes
//! exact on Windows, where hgs runs it like ssh.exe.
//!
//! Environment:
//! - `FAKE_SSH_LOG`: append each argv (without the program) as a JSON array line.
//! - `FAKE_SSH_OFFLINE_DIR`: a file named like the destination alias there makes
//!   the peer unreachable: "fixture offline" on stderr, exit 255.
//! - `FAKE_SSH_STREAMS`: append what stdin, stdout and stderr are as a JSON
//!   object line, such as `{"stdin":"pipe","stdout":"console","stderr":"file"}`
//!   (Windows; `console`, `pipe`, `synchronous pipe`, `file`, `device`, `none`).
//! - `FAKE_SSH_LOCAL_COMMAND=1`: the connection came up: expand the `%` tokens
//!   of a `-o LocalCommand=...` as OpenSSH does (an unknown token is fatal, exit
//!   255) and run it with the platform shell (cmd.exe on Windows, as Windows
//!   OpenSSH does; sh elsewhere) first.
//! - `FAKE_SSH_MODE`:
//!   - `canned` (default): save stdin to `FAKE_SSH_STDIN` (if set), write the
//!     bytes of the files named by `FAKE_SSH_STDOUT` and `FAKE_SSH_STDERR`, then
//!     exit with `FAKE_SSH_EXIT` (default 0). With `FAKE_SSH_HELPER_PID`, first
//!     leave a helper running that holds stdout and stderr open for 60 seconds
//!     without writing (as a connection multiplexer may), and write its process
//!     ID there.
//!   - `loopback`: the remote command must be `~/.local/bin/hgs ARGS...` in POSIX
//!     shell quoting. Run `FAKE_SSH_HGS` with ARGS, `HGS_CONFIG_DIR` set to
//!     `FAKE_SSH_ROOT/<alias>` and `HGS_STATE_DIR` to `.../<alias>/state`, with
//!     stdin, stdout and stderr passed through and its exit code returned.
//!   - `sleep`: write its process ID to `FAKE_SSH_PID` (if set), wait
//!     `FAKE_SSH_SECONDS` (default 30) seconds, then exit 0.
//!   - `bundle`: save stdin to `FAKE_SSH_BUNDLE`, print a setup report, exit 0.
//!
//! On Windows it refuses (exit 254) the standard streams Windows OpenSSH fails
//! on, so hgs must never pass them: a synchronous pipe (it never exits after
//! using one), a character device such as NUL or a file with data as input (the
//! peer never sees its end; `-G` reads none), and a file with data as output (it
//! writes from offset 0, over what `>>` keeps).
use std::{
    env,
    fs::{self, OpenOptions},
    io::{self, Read, Write},
    path::PathBuf,
    process::{exit, Command, Stdio},
    thread,
    time::Duration,
};

/// OpenSSH client options that take a value (from its getopt string).
const WITH_VALUE: &str = "BDEFIJLOPQRSWbceilmopw";

struct Invocation {
    options: Vec<String>,
    destination: String,
    command: Vec<String>,
}

fn parse(args: &[String]) -> Invocation {
    let mut options = Vec::new();
    let mut index = 0;
    while let Some(arg) = args.get(index) {
        if arg == "--" {
            index += 1;
            break;
        }
        if !arg.starts_with('-') || arg.len() < 2 {
            break;
        }
        for (position, flag) in arg.char_indices().skip(1) {
            if WITH_VALUE.contains(flag) {
                let attached = &arg[position + flag.len_utf8()..];
                let value = if attached.is_empty() {
                    index += 1;
                    args.get(index).cloned().unwrap_or_default()
                } else {
                    attached.to_owned()
                };
                if flag == 'o' {
                    options.push(value);
                }
                break;
            }
        }
        index += 1;
    }
    Invocation {
        options,
        destination: args.get(index).cloned().unwrap_or_default(),
        command: args
            .get(index + 1..)
            .map(<[String]>::to_vec)
            .unwrap_or_default(),
    }
}

fn json_string(text: &str) -> String {
    let mut out = String::from("\"");
    for c in text.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

/// POSIX shell words: single quotes, double quotes and backslash escapes.
fn shell_words(text: &str) -> Result<Vec<String>, String> {
    let mut words = Vec::new();
    let mut word = String::new();
    let mut started = false;
    let mut chars = text.chars();
    while let Some(c) = chars.next() {
        match c {
            '\'' => {
                started = true;
                loop {
                    match chars.next() {
                        Some('\'') => break,
                        Some(c) => word.push(c),
                        None => return Err("unterminated single quote".into()),
                    }
                }
            }
            '"' => {
                started = true;
                loop {
                    match chars.next() {
                        Some('"') => break,
                        Some('\\') => match chars.next() {
                            Some(c @ ('$' | '`' | '"' | '\\')) => word.push(c),
                            Some('\n') => {}
                            Some(c) => {
                                word.push('\\');
                                word.push(c);
                            }
                            None => return Err("unterminated escape".into()),
                        },
                        Some(c) => word.push(c),
                        None => return Err("unterminated double quote".into()),
                    }
                }
            }
            '\\' => {
                started = true;
                match chars.next() {
                    Some('\n') => {}
                    Some(c) => word.push(c),
                    None => return Err("unterminated escape".into()),
                }
            }
            c if c.is_whitespace() => {
                if started {
                    words.push(std::mem::take(&mut word));
                    started = false;
                }
            }
            c => {
                started = true;
                word.push(c);
            }
        }
    }
    if started {
        words.push(word);
    }
    Ok(words)
}

fn variable(name: &str) -> Option<String> {
    env::var(name).ok().filter(|value| !value.is_empty())
}

/// OpenSSH `percent_expand` for LocalCommand: `%%` is `%`, the client tokens get
/// fixture values and any other token fails.
fn percent_expand(command: &str, destination: &str) -> Result<String, String> {
    let mut expanded = String::new();
    let mut chars = command.chars();
    while let Some(c) = chars.next() {
        if c != '%' {
            expanded.push(c);
            continue;
        }
        match chars.next() {
            Some('%') => expanded.push('%'),
            Some('h' | 'k' | 'n') => expanded.push_str(destination),
            Some('C') => expanded.push_str("0123456789abcdef0123456789abcdef01234567"),
            Some('d') => expanded.push_str(
                &variable("USERPROFILE")
                    .or_else(|| variable("HOME"))
                    .unwrap_or_default(),
            ),
            Some('i') => expanded.push_str("1000"),
            Some('j') => {}
            Some('L' | 'l') => expanded.push_str("localhost"),
            Some('p') => expanded.push_str("22"),
            Some('r' | 'u') => expanded.push_str("user"),
            Some('T') => expanded.push_str("NONE"),
            Some(other) => return Err(format!("unknown key %{other} | percent_expand: failed")),
            None => return Err("invalid format | percent_expand: failed".into()),
        }
    }
    Ok(expanded)
}

fn fail(message: &str) -> ! {
    eprintln!("fake_ssh: {message}");
    exit(254)
}

fn append(path: &str, line: &str) {
    OpenOptions::new()
        .create(true)
        .append(true)
        .open(path)
        .and_then(|mut file| file.write_all(line.as_bytes()))
        .unwrap_or_else(|error| fail(&format!("{path}: {error}")));
}

/// Windows OpenSSH passes the command line to cmd.exe unchanged.
#[cfg(windows)]
fn shell(command: &str) -> Command {
    use std::os::windows::process::CommandExt;
    let mut shell = Command::new("cmd.exe");
    shell.arg("/c").raw_arg(command);
    shell
}
#[cfg(not(windows))]
fn shell(command: &str) -> Command {
    let mut shell = Command::new("/bin/sh");
    shell.arg("-c").arg(command);
    shell
}

/// What a standard stream is, as far as Windows OpenSSH cares.
#[derive(Clone, Copy, PartialEq)]
#[cfg_attr(not(windows), allow(dead_code))]
enum Stream {
    Console,
    Pipe { synchronous: bool },
    File { size: u64 },
    Device,
    Absent,
}

impl Stream {
    fn name(self) -> &'static str {
        match self {
            Stream::Console => "console",
            Stream::Pipe { synchronous: false } => "pipe",
            Stream::Pipe { synchronous: true } => "synchronous pipe",
            Stream::File { .. } => "file",
            Stream::Device => "device",
            Stream::Absent => "none",
        }
    }
}

#[cfg(windows)]
fn streams() -> [(&'static str, Stream); 3] {
    use std::{ffi::c_void, os::windows::io::AsRawHandle};
    #[link(name = "kernel32", kind = "raw-dylib")]
    extern "system" {
        fn GetFileType(file: *mut c_void) -> u32;
        fn GetConsoleMode(console: *mut c_void, mode: *mut u32) -> i32;
        fn GetFileSizeEx(file: *mut c_void, size: *mut i64) -> i32;
    }
    #[link(name = "ntdll", kind = "raw-dylib")]
    extern "system" {
        fn NtQueryInformationFile(
            file: *mut c_void,
            status: *mut [usize; 2],
            information: *mut u32,
            length: u32,
            class: u32,
        ) -> i32;
    }
    const FILE_TYPE_DISK: u32 = 1;
    const FILE_TYPE_CHAR: u32 = 2;
    const FILE_TYPE_PIPE: u32 = 3;
    const FILE_MODE_INFORMATION: u32 = 16;
    const SYNCHRONOUS_IO: u32 = 0x10 | 0x20;
    let kind = |handle: *mut c_void| unsafe {
        let (mut console, mut size, mut status, mut mode) = (0, 0, [0; 2], 0);
        if GetConsoleMode(handle, &mut console) != 0 {
            return Stream::Console;
        }
        match GetFileType(handle) {
            FILE_TYPE_PIPE => Stream::Pipe {
                synchronous: NtQueryInformationFile(
                    handle,
                    &mut status,
                    &mut mode,
                    4,
                    FILE_MODE_INFORMATION,
                ) == 0
                    && mode & SYNCHRONOUS_IO != 0,
            },
            FILE_TYPE_DISK => Stream::File {
                size: if GetFileSizeEx(handle, &mut size) != 0 {
                    size as u64
                } else {
                    0
                },
            },
            FILE_TYPE_CHAR => Stream::Device,
            _ => Stream::Absent,
        }
    };
    [
        ("stdin", kind(io::stdin().as_raw_handle())),
        ("stdout", kind(io::stdout().as_raw_handle())),
        ("stderr", kind(io::stderr().as_raw_handle())),
    ]
}
#[cfg(not(windows))]
fn streams() -> [(&'static str, Stream); 3] {
    [
        ("stdin", Stream::Absent),
        ("stdout", Stream::Absent),
        ("stderr", Stream::Absent),
    ]
}

/// The ways Windows OpenSSH would fail on these streams. `-G` only prints its
/// configuration and reads no input.
fn refusals(streams: &[(&'static str, Stream); 3], reads_input: bool) -> Vec<String> {
    let mut refusals = Vec::new();
    let synchronous: Vec<&str> = streams
        .iter()
        .filter(|(_, stream)| *stream == Stream::Pipe { synchronous: true })
        .map(|(name, _)| *name)
        .collect();
    if !synchronous.is_empty() {
        refusals.push(format!(
            "synchronous pipe for {}; Windows OpenSSH would never exit",
            synchronous.join(", ")
        ));
    }
    if reads_input && matches!(streams[0].1, Stream::File { size: 1.. } | Stream::Device) {
        refusals.push(format!(
            "stdin is a {}; the peer would never see its end through Windows OpenSSH",
            streams[0].1.name()
        ));
    }
    for (name, stream) in &streams[1..] {
        if let Stream::File { size: size @ 1.. } = stream {
            refusals.push(format!(
                "{name} is a file with {size} bytes; Windows OpenSSH would write over them"
            ));
        }
    }
    refusals
}

fn main() {
    let args: Vec<String> = env::args().skip(1).collect();
    let streams = streams();
    if let Some(path) = variable("FAKE_SSH_STREAMS") {
        let fields: Vec<String> = streams
            .iter()
            .map(|(name, stream)| format!("{}:{}", json_string(name), json_string(stream.name())))
            .collect();
        append(&path, &format!("{{{}}}\n", fields.join(",")));
    }
    let refused = refusals(&streams, !args.iter().any(|arg| arg == "-G"));
    if !refused.is_empty() {
        fail(&refused.join("; "));
    }
    if let Some(log) = variable("FAKE_SSH_LOG") {
        let line = format!(
            "[{}]\n",
            args.iter()
                .map(|arg| json_string(arg))
                .collect::<Vec<_>>()
                .join(",")
        );
        append(&log, &line);
    }
    let invocation = parse(&args);
    if let Some(dir) = variable("FAKE_SSH_OFFLINE_DIR") {
        if !invocation.destination.is_empty()
            && PathBuf::from(dir).join(&invocation.destination).exists()
        {
            eprintln!("fixture offline");
            exit(255);
        }
    }
    if variable("FAKE_SSH_LOCAL_COMMAND").as_deref() == Some("1") {
        let enabled = invocation
            .options
            .iter()
            .any(|o| o.eq_ignore_ascii_case("PermitLocalCommand=yes"));
        if let Some(command) = invocation
            .options
            .iter()
            .find_map(|o| o.strip_prefix("LocalCommand="))
        {
            let command =
                percent_expand(command, &invocation.destination).unwrap_or_else(|error| {
                    eprintln!("fake_ssh: {error}");
                    exit(255)
                });
            if enabled {
                if let Err(error) = shell(&command).status() {
                    fail(&format!("LocalCommand: {error}"));
                }
            }
        }
    }
    match variable("FAKE_SSH_MODE").as_deref().unwrap_or("canned") {
        "canned" => {
            if let Some(path) = variable("FAKE_SSH_STDIN") {
                let mut bytes = Vec::new();
                io::stdin()
                    .read_to_end(&mut bytes)
                    .unwrap_or_else(|error| fail(&error.to_string()));
                fs::write(path, bytes).unwrap_or_else(|error| fail(&error.to_string()));
            }
            if let Some(path) = variable("FAKE_SSH_HELPER_PID") {
                // Never waited for: the helper outlives this client on purpose.
                #[allow(clippy::zombie_processes)]
                let helper = Command::new(
                    env::current_exe().unwrap_or_else(|error| fail(&error.to_string())),
                )
                .env("FAKE_SSH_MODE", "sleep")
                .env("FAKE_SSH_SECONDS", "60")
                .env_remove("FAKE_SSH_LOG")
                .env_remove("FAKE_SSH_PID")
                .stdin(Stdio::null())
                .spawn()
                .unwrap_or_else(|error| fail(&format!("helper: {error}")));
                fs::write(path, helper.id().to_string())
                    .unwrap_or_else(|error| fail(&error.to_string()));
            }
            for (name, mut out) in [
                ("FAKE_SSH_STDOUT", Box::new(io::stdout()) as Box<dyn Write>),
                ("FAKE_SSH_STDERR", Box::new(io::stderr())),
            ] {
                if let Some(path) = variable(name) {
                    let bytes =
                        fs::read(&path).unwrap_or_else(|error| fail(&format!("{path}: {error}")));
                    out.write_all(&bytes)
                        .unwrap_or_else(|error| fail(&error.to_string()));
                    out.flush().unwrap_or_else(|error| fail(&error.to_string()));
                }
            }
            exit(variable("FAKE_SSH_EXIT").map_or(0, |code| {
                code.parse().unwrap_or_else(|_| fail("FAKE_SSH_EXIT"))
            }))
        }
        "loopback" => {
            let remote = invocation.command.join(" ");
            let words = shell_words(&remote).unwrap_or_else(|error| fail(&error));
            let Some((program, rest)) = words.split_first() else {
                fail("no remote command")
            };
            if program != "~/.local/bin/hgs" {
                fail(&format!("unexpected remote program {program}"));
            }
            let hgs = variable("FAKE_SSH_HGS").unwrap_or_else(|| fail("FAKE_SSH_HGS is not set"));
            let root = PathBuf::from(
                variable("FAKE_SSH_ROOT").unwrap_or_else(|| fail("FAKE_SSH_ROOT is not set")),
            );
            let node = root.join(&invocation.destination);
            let status = Command::new(hgs)
                .args(rest)
                .env("HGS_CONFIG_DIR", &node)
                .env("HGS_STATE_DIR", node.join("state"))
                .status()
                .unwrap_or_else(|error| fail(&format!("run hgs: {error}")));
            exit(status.code().unwrap_or(255))
        }
        "sleep" => {
            if let Some(path) = variable("FAKE_SSH_PID") {
                fs::write(path, std::process::id().to_string())
                    .unwrap_or_else(|error| fail(&error.to_string()));
            }
            let seconds = variable("FAKE_SSH_SECONDS")
                .and_then(|s| s.parse().ok())
                .unwrap_or(30);
            thread::sleep(Duration::from_secs(seconds));
        }
        "bundle" => {
            let mut bytes = Vec::new();
            io::stdin()
                .read_to_end(&mut bytes)
                .unwrap_or_else(|error| fail(&error.to_string()));
            let path =
                variable("FAKE_SSH_BUNDLE").unwrap_or_else(|| fail("FAKE_SSH_BUNDLE is not set"));
            fs::write(path, bytes).unwrap_or_else(|error| fail(&error.to_string()));
            println!("Setup complete: hgs fixture");
        }
        mode => fail(&format!("unknown mode {mode}")),
    }
}
