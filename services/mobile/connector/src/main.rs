//! `zerus-mobile-connector --config PATH [--allow-insecure-localhost]`, or
//! `--check [--json]` for a read-only report. Log lines follow Python's
//! `%(levelname)s: %(message)s` on stderr.
use std::process::ExitCode;
use zerus_mobile_connector::runtime::log;
use zerus_mobile_connector::{check, cli, config, start, STARTUP_FAILURE};

fn main() -> ExitCode {
    if std::env::args_os().nth(1).as_deref() == Some(std::ffi::OsStr::new("--workspace")) {
        use std::io::Read;
        let args: Vec<_> = std::env::args_os().collect();
        let result = (|| -> Result<serde_json::Value, String> {
            if args.len() != 5 || args[3] != "--config" {
                return Err("expected --workspace ACTION --config PATH".into());
            }
            let mut bytes = Vec::new();
            std::io::stdin()
                .take(65_537)
                .read_to_end(&mut bytes)
                .map_err(|_| "Workspace input is unavailable")?;
            if bytes.len() > 65_536 {
                return Err("Workspace input exceeds its limit".into());
            }
            let input = if bytes.is_empty() {
                serde_json::json!({})
            } else {
                serde_json::from_slice(&bytes).map_err(|_| "Invalid workspace input")?
            };
            let runtime = tokio::runtime::Builder::new_current_thread()
                .enable_all()
                .build()
                .map_err(|_| "Workspace operation could not start")?;
            runtime
                .block_on(zerus_mobile_connector::workspace::manage(
                    &args[2].to_string_lossy(),
                    std::path::Path::new(&args[4]),
                    input,
                    false,
                ))
                .map_err(|error| error.message().to_owned())
        })();
        return match result {
            Ok(value) => {
                println!("{value}");
                ExitCode::SUCCESS
            }
            Err(error) => {
                println!("{}", serde_json::json!({"ok":false,"error":error}));
                ExitCode::from(1)
            }
        };
    }
    if std::env::args_os().nth(1).as_deref() == Some(std::ffi::OsStr::new("--admit-revision")) {
        let args: Vec<_> = std::env::args_os().collect();
        let admitted = (|| {
            if args.len() != 5 || args[3] != "--config" {
                return None;
            }
            let revision = args[2].to_str()?;
            if revision.len() != 64 || !revision.bytes().all(|b| b.is_ascii_hexdigit()) {
                return None;
            }
            let environment = config::Environment::current().ok()?;
            zerus_mobile_connector::start_bound(
                std::path::Path::new(&args[4]),
                false,
                &environment,
                zerus_mobile_connector::now(),
                Some(revision),
            )
            .ok()
        })();
        return match admitted {
            Some(started) => ExitCode::from(zerus_mobile_connector::run(started)),
            None => {
                log("ERROR", STARTUP_FAILURE);
                ExitCode::from(1)
            }
        };
    }
    #[cfg(windows)]
    if std::env::args_os().nth(1).as_deref() == Some(std::ffi::OsStr::new("--manage")) {
        use std::io::Read;
        let args: Vec<_> = std::env::args_os().collect();
        let result =
            (|| -> zerus_mobile_connector::windows_management::Result<serde_json::Value> {
                if args.len() != 5 || args[3] != "--config" {
                    return Err("expected --manage ACTION --config PATH".into());
                }
                let mut bytes = Vec::new();
                std::io::stdin().take(65_537).read_to_end(&mut bytes)?;
                if bytes.len() > 65_536 {
                    return Err("management input exceeds its limit".into());
                }
                let input = if bytes.is_empty() {
                    serde_json::json!({})
                } else {
                    serde_json::from_slice(&bytes)?
                };
                zerus_mobile_connector::windows_management::manage(
                    &args[2].to_string_lossy(),
                    std::path::Path::new(&args[4]),
                    &input,
                )
            })();
        return match result {
            Ok(value) => {
                println!("{value}");
                ExitCode::SUCCESS
            }
            Err(_) => {
                println!(
                    "{}",
                    serde_json::json!({"ok":false,"error":"Connector operation was not confirmed. Reload status; verify private configuration with --check before retrying."})
                );
                ExitCode::from(1)
            }
        };
    }
    let command = match cli::parse(std::env::args_os().skip(1)) {
        Ok(command) => command,
        Err(message) => {
            eprintln!("{}\nzerus-mobile-connector: error: {message}", cli::USAGE);
            return ExitCode::from(2);
        }
    };
    match command {
        cli::Command::Help => println!("{}\n\n{}", cli::USAGE, cli::HELP),
        cli::Command::Version => println!("zerus-mobile-connector {}", env!("CARGO_PKG_VERSION")),
        cli::Command::Check {
            config,
            allow_insecure_localhost,
            json,
        } => {
            return ExitCode::from(check::run(&config, allow_insecure_localhost, json) as u8);
        }
        cli::Command::Run {
            config,
            allow_insecure_localhost,
        } => {
            let started = config::Environment::current()
                .map_err(|_| ())
                .and_then(|environment| {
                    start(
                        &config,
                        allow_insecure_localhost,
                        &environment,
                        zerus_mobile_connector::now(),
                    )
                    .map_err(|_| ())
                });
            let Ok(started) = started else {
                log("ERROR", STARTUP_FAILURE);
                return ExitCode::from(1);
            };
            for warning in &started.warnings {
                log("WARNING", warning);
            }
            return ExitCode::from(zerus_mobile_connector::run(started));
        }
    }
    ExitCode::SUCCESS
}
