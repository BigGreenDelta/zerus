mod accounts;
mod cli;
mod config;
mod machines;
mod macos_session;
mod metrics;
#[cfg(unix)]
mod mobile_peers;
mod native_ui;
// Overmind keeps its store next to the local session host; Windows consoles use @host.
#[cfg(unix)]
mod overmind;
mod session_reference;
mod platform;
mod presence;
// The local session host (tmux, hooks, agent state) runs on Unix. Windows builds
// are control consoles for remote machines and link the stubs in state_windows.rs.
#[cfg(unix)]
mod state;
#[cfg(not(unix))]
#[path = "state_windows.rs"]
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
