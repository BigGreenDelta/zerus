//! `zerus-mobile-connector --config PATH [--allow-insecure-localhost]`, or
//! `--check [--json]` for a read-only report. Log lines follow Python's
//! `%(levelname)s: %(message)s` on stderr.
use std::process::ExitCode;
use zerus_mobile_connector::runtime::log;
use zerus_mobile_connector::{check, cli, config, start, STARTUP_FAILURE};

fn main() -> ExitCode {
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
