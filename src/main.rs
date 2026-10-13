mod accounts;
mod cli;
mod config;
mod download;
mod host;
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
    // The Windows session host and its tools, before `cli::dispatch` puts
    // hgs.exe into its own kill-on-close job (spec D1, D4).
    #[cfg(windows)]
    if std::env::args().nth(1).as_deref() == Some("__host") {
        let args: Vec<String> = std::env::args().skip(2).collect();
        std::process::exit(host::run(&args));
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
