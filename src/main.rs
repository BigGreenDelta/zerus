mod accounts;
mod cli;
mod config;
mod download;
mod machines;
mod macos_session;
mod metrics;
#[cfg(unix)]
mod mobile_peers;
mod native_ui;
mod session_reference;
mod platform;
mod presence;
mod screen;
mod speech;
mod status;
mod state;
mod swarm;

fn main() {
    if std::env::args().nth(1).as_deref() == Some("--macos-session-service") {
        std::process::exit(macos_session::serve());
    }
    let code = match cli::dispatch(std::env::args().skip(1).collect()) {
        Ok(code) => code,
        Err(err) => {
            eprintln!("hgs: {}", err.message);
            err.code
        }
    };
    std::process::exit(code);
}
