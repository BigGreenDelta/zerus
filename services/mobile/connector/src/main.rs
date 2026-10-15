//! `zerus-mobile-connector --config PATH [--allow-insecure-localhost]`, or
//! `--check [--json]` for a read-only report. Log lines follow Python's
//! `%(levelname)s: %(message)s` on stderr.
use std::process::ExitCode;
use zerus_mobile_connector::{check, cli, config, start, STARTUP_FAILURE};

/// The relay runtime comes with the next connector slice; until then a valid
/// start stops here, after the journal checks, without contacting the relay.
const NOT_IMPLEMENTED: &str =
    "Connector runtime is not available in this build; keep using the Python connector";

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
                eprintln!("ERROR: {STARTUP_FAILURE}");
                return ExitCode::from(1);
            };
            for warning in &started.warnings {
                eprintln!("WARNING: {warning}");
            }
            started.journal.close();
            eprintln!("ERROR: {NOT_IMPLEMENTED}");
            return ExitCode::from(1);
        }
    }
    ExitCode::SUCCESS
}
