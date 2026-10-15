//! The tmux server's own scope on Linux.
//!
//! tmux built with systemd support moves each new pane into a
//! `tmux-spawn-<uuid>.scope` that is `PartOf=` the unit holding the server, so
//! the unit whose process starts the shared server owns every pane: stopping it
//! ends them all, whatever its `KillMode=`. Where hgs runs in a unit of the user
//! service manager (the desktop GUI, the mobile connector, the recovery and
//! swarm workers, a desktop terminal) and no server answers, a tmux command that
//! may start one runs in a transient scope of its own instead, which then holds
//! only tmux: the server and, for an attached launch, its client until it
//! detaches. A running server is never touched: the command then runs as it
//! always has, as it does outside a user unit (an SSH login, macOS), where the
//! user manager or systemd-run is missing, or when systemd-run cannot make the
//! scope.

use crate::platform;
use crate::state::nonempty_env;
use std::ffi::OsString;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

/// systemd-run has `--collect` from systemd 236 and `--expand-environment` from
/// 254; since then it expands `${NAME}` in a scope's command unless told not to.
const COLLECT: u32 = 236;
const EXPAND: u32 = 254;

/// What systemd-run runs in the new scope: it records that the scope exists,
/// then becomes the tmux command. Without the record, tmux never ran there.
const ENTER: &str = r#"true > "$1" || exit 125; shift; exec "$@""#;

/// A transient scope for one tmux command that may start the server.
pub(super) struct Scope {
    version: u32,
    marker: tempfile::TempDir,
}

impl Scope {
    /// A scope for a command that may start the server, or None where that
    /// server would not belong to a user unit of hgs, where `running` finds a
    /// server, or where systemd cannot make a scope.
    pub(super) fn new(running: impl FnOnce() -> bool) -> Option<Scope> {
        let cgroup = fs::read_to_string("/proc/self/cgroup").ok()?;
        user_unit(&cgroup, unsafe { libc::getuid() })?;
        // systemd-run --user finds the manager through XDG_RUNTIME_DIR.
        let manager = Path::new(&nonempty_env("XDG_RUNTIME_DIR")?).join("systemd/private");
        // Without tmux, the command fails as it always has.
        if !manager.exists()
            || !platform::available("systemd-run")
            || !platform::available("tmux")
            || running()
        {
            return None;
        }
        let version = version(&platform::capture("systemd-run", &["--version"])?)?;
        if version < COLLECT {
            return None;
        }
        let marker = tempfile::Builder::new()
            .prefix("hgs-tmux-")
            .tempdir()
            .ok()?;
        Some(Scope { version, marker })
    }

    /// `tmux ARGS` in the scope, with the caller's environment, directory and stdio.
    pub(super) fn command(&self, args: &[String]) -> Command {
        self.command_with("systemd-run", "tmux", args)
    }

    fn command_with(&self, systemd_run: &str, program: &str, args: &[String]) -> Command {
        let unit = format!("hgs-tmux-{}", uuid::Uuid::new_v4());
        let mut command = Command::new(systemd_run);
        command.args(arguments(
            self.version,
            &unit,
            &self.started_path(),
            program,
            args,
        ));
        command
    }

    fn started_path(&self) -> PathBuf {
        self.marker.path().join("started")
    }

    /// Whether the command ran in the scope. Otherwise systemd-run failed
    /// before it started tmux, and the command can run as it always has.
    pub(super) fn started(&self) -> bool {
        self.started_path().is_file()
    }
}

/// The unit of the user service manager that holds a process whose
/// `/proc/PID/cgroup` reads `cgroup`: the first component below the manager's
/// own unit (`user@<uid>.service`) that is not a slice, as tmux finds its server's
/// unit.
fn user_unit(cgroup: &str, uid: u32) -> Option<&str> {
    let manager = format!("user@{uid}.service");
    cgroup.lines().find_map(|line| {
        let mut fields = line.splitn(3, ':');
        let (hierarchy, controllers, path) = (fields.next()?, fields.next()?, fields.next()?);
        // The unified hierarchy, or systemd's own on a legacy one.
        if !((hierarchy == "0" && controllers.is_empty()) || controllers == "name=systemd") {
            return None;
        }
        path.split('/')
            .skip_while(|component| *component != manager)
            .skip(1)
            .find(|component| !component.ends_with(".slice"))
            .filter(|unit| !unit.is_empty())
    })
}

/// The systemd version `systemd-run --version` prints first: "systemd 262 (262-1-arch)".
fn version(text: &str) -> Option<u32> {
    let mut words = text.split_whitespace();
    if words.next()? != "systemd" {
        return None;
    }
    words.next()?.parse().ok()
}

/// The systemd-run arguments that run `program ARGS` in the new scope `unit`
/// and first record at `started` that it does.
fn arguments(
    version: u32,
    unit: &str,
    started: &Path,
    program: &str,
    args: &[String],
) -> Vec<OsString> {
    let mut arguments: Vec<OsString> = ["--user", "--scope", "--quiet", "--collect"]
        .map(OsString::from)
        .into();
    if version >= EXPAND {
        arguments.push("--expand-environment=no".into());
    }
    arguments.extend(
        [
            "--description=tmux server for hgs sessions",
            "--unit",
            unit,
            "--",
            "/bin/sh",
            "-c",
            ENTER,
            "hgs-tmux",
        ]
        .map(OsString::from),
    );
    arguments.push(started.into());
    arguments.push(program.into());
    arguments.extend(args.iter().map(OsString::from));
    arguments
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::PermissionsExt;

    #[test]
    fn the_server_unit_is_the_first_unit_below_the_user_manager() {
        // `{M}` stands for the user manager of uid 1000 below its user slice.
        let unit = |cgroup: &str| {
            let manager = format!("user.slice/user-1000.slice/user@{}.service", 1000);
            user_unit(&cgroup.replace("{M}", &manager), 1000).map(str::to_owned)
        };
        let some = |unit: &str| Some(unit.to_owned());
        // The desktop GUI service, a desktop-launched app and an agent pane.
        assert_eq!(
            unit("0::/{M}/app.slice/hgs-tray.service\n"),
            some("hgs-tray.service")
        );
        assert_eq!(
            unit("0::/{M}/app.slice/app-org.kde.konsole.slice/app-org.kde.konsole-1.scope"),
            some("app-org.kde.konsole-1.scope")
        );
        assert_eq!(
            unit("0::/{M}/app.slice/tmux-spawn-1.scope"),
            some("tmux-spawn-1.scope")
        );
        // A subgroup of a delegated service still belongs to that service.
        assert_eq!(
            unit("0::/{M}/app.slice/example.service/payload"),
            some("example.service")
        );
        // systemd's named hierarchy on a legacy or hybrid layout.
        assert_eq!(
            unit("12:pids:/{M}\n1:name=systemd:/{M}/app.slice/hgs-recovery.service\n"),
            some("hgs-recovery.service")
        );
        // Outside the user manager: an SSH login, a system service, the root
        // group, another user's manager, a slice, a controller hierarchy.
        for outside in [
            "0::/user.slice/user-1000.slice/session-4.scope\n",
            "0::/system.slice/actions.runner.example.service\n",
            "0::/\n",
            "0::/{M}/app.slice\n",
            "4:cpu:/{M}/app.slice/hgs-tray.service\n",
            "",
        ] {
            assert_eq!(unit(outside), None, "{outside:?}");
        }
        let other = format!(
            "0::/user.slice/user-1001.slice/user@{}.service/app.slice/hgs-tray.service",
            1001
        );
        assert_eq!(user_unit(&other, 1000), None);
        assert_eq!(user_unit(&other, 1001), Some("hgs-tray.service"));
    }

    #[test]
    fn versions_come_from_the_first_line() {
        assert_eq!(
            version("systemd 262 (262-1-arch)\n+PAM +AUDIT\n"),
            Some(262)
        );
        assert_eq!(version("systemd 249 (249.11-0ubuntu3.12)\n"), Some(249));
        assert_eq!(version("systemd-run 262\n"), None);
        assert_eq!(version("systemd unknown\n"), None);
        assert_eq!(version(""), None);
    }

    #[test]
    fn the_scope_runs_tmux_unexpanded_after_recording_that_it_started() {
        let started = Path::new("/run/hgs-tmux-x/started");
        let args = [
            "new-session".to_owned(),
            "-d".into(),
            "exec \"$0\" ${HOME}".into(),
        ];
        let text = |version| {
            arguments(version, "hgs-tmux-1", started, "tmux", &args)
                .into_iter()
                .map(|argument| argument.into_string().unwrap())
                .collect::<Vec<_>>()
        };
        let tail = [
            "--description=tmux server for hgs sessions",
            "--unit",
            "hgs-tmux-1",
            "--",
            "/bin/sh",
            "-c",
            ENTER,
            "hgs-tmux",
            "/run/hgs-tmux-x/started",
            "tmux",
            "new-session",
            "-d",
            "exec \"$0\" ${HOME}",
        ];
        let head = ["--user", "--scope", "--quiet", "--collect"];
        assert_eq!(
            text(262),
            [&head[..], &["--expand-environment=no"], &tail].concat()
        );
        assert_eq!(text(254)[4], "--expand-environment=no");
        // Before 254 systemd-run neither knows the option nor expands a scope's command.
        assert_eq!(text(253), [&head[..], &tail].concat());
    }

    /// A stand-in for systemd-run in `directory`: it runs the scope's command,
    /// as systemd-run does once the scope exists, or fails before it.
    fn fake_systemd_run(directory: &Path, scope_made: bool) -> String {
        let path = directory.join(if scope_made { "made" } else { "refused" });
        let body = if scope_made {
            "while [ \"$1\" != -- ]; do shift; done\nshift\nexec \"$@\"\n"
        } else {
            "echo 'Failed to start transient scope unit: fixture' >&2\nexit 1\n"
        };
        fs::write(&path, format!("#!/bin/sh\n{body}")).unwrap();
        fs::set_permissions(&path, fs::Permissions::from_mode(0o755)).unwrap();
        path.to_str().unwrap().to_owned()
    }

    fn scope(version: u32) -> Scope {
        Scope {
            version,
            marker: tempfile::tempdir().unwrap(),
        }
    }

    #[test]
    fn a_command_that_ran_in_the_scope_keeps_its_arguments_and_result() {
        let directory = tempfile::tempdir().unwrap();
        let systemd_run = fake_systemd_run(directory.path(), true);
        let scope = scope(262);
        let args = [
            "-c".to_owned(),
            "printf '%s|' \"$0\" \"$@\"; exit 3".into(),
            "a b".into(),
            "$HOME".into(),
        ];
        let output = scope
            .command_with(&systemd_run, "/bin/sh", &args)
            .output()
            .unwrap();
        assert!(scope.started());
        assert_eq!(output.status.code(), Some(3));
        assert_eq!(String::from_utf8_lossy(&output.stdout), "a b|$HOME|");
    }

    #[test]
    fn a_refused_scope_never_runs_the_command() {
        let directory = tempfile::tempdir().unwrap();
        let systemd_run = fake_systemd_run(directory.path(), false);
        let ran = directory.path().join("ran");
        let scope = scope(262);
        let args = ["-c".to_owned(), format!(": > {}", ran.display())];
        let output = scope
            .command_with(&systemd_run, "/bin/sh", &args)
            .output()
            .unwrap();
        assert!(!output.status.success());
        assert!(!scope.started());
        assert!(!ran.exists());
    }

    #[test]
    fn a_scope_that_cannot_record_its_start_never_runs_the_command() {
        let directory = tempfile::tempdir().unwrap();
        let systemd_run = fake_systemd_run(directory.path(), true);
        let ran = directory.path().join("ran");
        let scope = scope(262);
        fs::remove_dir(scope.marker.path()).unwrap();
        let args = ["-c".to_owned(), format!(": > {}", ran.display())];
        let output = scope
            .command_with(&systemd_run, "/bin/sh", &args)
            .output()
            .unwrap();
        assert_eq!(output.status.code(), Some(125));
        assert!(!scope.started());
        assert!(!ran.exists());
    }
}
