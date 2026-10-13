# Windows support

Status: Phase 1 shipped in nightly builds (approved direction 2026-10-10,
implemented 2026-10-11). Tracking: `zerus-ziq3`. Decisions and deviations made
during the implementation are in
[Implementation notes](#implementation-notes-2026-10-11). Phase 2 has its own spec:
[Windows Phase 2: native session host](2026-10-13-windows-native-host-design.md).

This document is the starting point for Windows support. It records the owner's
decisions, the verified state of the code on Windows, prior art and an ordered list
of first steps. It does not schedule the work. Phase 2 gets its own spec before
anyone implements it.

## Decisions

- **Phased, with a native host as the goal.** Windows gets a native session host
  modelled on Orca (below), not WSL as the primary environment. The work ships in
  two phases:
  1. **Control console.** A native Qt GUI and the `hgs` CLI run on Windows and
     manage remote Linux and macOS machines over the system OpenSSH client. No
     agents run locally on Windows.
  2. **Native host.** Agents (`claude.exe`, `codex`) run on Windows under `hgs`,
     behind a terminal-backend abstraction whose Windows implementation is a
     persistent ConPTY session host.
- **Approach A for `hgs`.** Windows builds the same `hgs` binary with the local
  session host compiled out (`cfg(unix)`). This rules out a separate thin client
  crate (it would duplicate forwarding, configuration and swarm logic) and a GUI
  that calls SSH directly (a second code path for every operation, and no swarm
  store).
- **Phase 1 is done when** `hgs.exe` and `hgs-tray.exe` build and test on Windows
  in hosted CI, the nightly publishes a portable zip produced by `windeployqt`, and
  manual verification on a Windows PC covers Fleet, Activity, sending messages and
  the embedded terminal attached to a remote session. Phase 1 has no installer,
  code signing, winget package or self-update.
- **Development order.** Use a real Windows development PC first, then add hosted
  CI.

## Non-goals for Phase 1

- Local sessions, local accounts and local hooks on Windows.
- WSL as a session host. Orca offers WSL only as an optional extra shell target.
  Zerus may do the same later.
- Windows as an SSH *target* for other peers. Peers do not connect to a Windows
  machine in Phase 1. See Phase 2.
- Using a Windows machine as a mobile gateway (`mobile_peers`, relay settings).
- The DeepSeek native UI from a Windows client. It relies on SSH `ControlPersist`,
  which Windows OpenSSH does not support.
- Installer, signing, winget and automatic updates.

## Verified starting state

Facts checked on 2026-10-10 against commit `5affe5f`.

### Architecture that matters for Windows

- The GUI runs the local `hgs` through `QProcess` (`tray/src/HgsClient.cpp`). A
  remote operation is `hgs @host <command>`, which `cli::remote` turns into
  `ssh <options> host ~/.local/bin/hgs <command>` (`src/cli.rs`). `hgs ls --json`
  polls each peer the same way (`peer_json`).
- Swarm synchronization is initiated by the local machine over SSH
  (`src/swarm/transport.rs`). A Windows client can take part without accepting
  inbound SSH.
- `hgs machine setup` copies a source checkout to the remote machine and builds
  there (`src/machines.rs`).
- Configuration comes from `HOME` (`src/config.rs`). `Config::load` fails when
  `HOME` is unset, which is the normal case on Windows.
- `hgs` sets no `ControlMaster` options itself. Only `src/native_ui.rs` uses
  `ControlPersist`. Windows OpenSSH has no connection multiplexing, so every poll
  of every peer costs a full SSH handshake.

### `cargo check` on Windows

`cargo check --locked` for `x86_64-pc-windows-msvc` (Rust 1.98.1, Visual Studio
2022) fails with 204 errors. Almost all of them are Unix-only APIs:
`OpenOptionsExt::mode` and `custom_flags`, `PermissionsExt::from_mode`,
`MetadataExt::{dev, ino, uid, mtime, mtime_nsec}`, `CommandExt::{process_group,
pre_exec, exec}`, `ExitStatusExt::signal`, `as_raw_fd`, `std::os::fd`, and
`libc::{kill, getuid, geteuid, getppid, setsid, fcntl, sigaction}` with
`O_NONBLOCK`, `O_NOFOLLOW`, `O_NOCTTY`, `SIGKILL` and `SIGHUP`.

Errors by file:

| Errors | File |
| -----: | ---- |
| 18 | `src/state/worktrees.rs`, `src/mobile_peers.rs` (each) |
| 15 | `src/state/input.rs`, `src/state/conversation_history.rs` (each) |
| 14 | `src/state/attachments.rs` |
| 13 | `src/state/supervisor.rs`, `src/platform.rs` (each) |
| 11 | `src/accounts.rs` |
| 10 | `src/state/dsh.rs` |
| 7 | `src/state/storage.rs` |
| 6 | `src/state/recovery.rs`, `src/accounts/inspection.rs` (each) |
| 5 | `src/swarm/store.rs`, `src/state/{usage,processes,processes/history,lifecycle}.rs`, `src/accounts/claude.rs` (each) |
| 4 | `src/machines.rs` |
| 1–3 | `src/state/{journal,goals,codex_questions,attempts}.rs`, `src/state/processes/{control,native}.rs`, `src/cli.rs`, `src/accounts/claude_settings.rs`, `src/swarm/launch_project.rs`, `src/native_ui.rs`, `src/metrics.rs` |

Client modules depend on `state` at only a few entry points: `src/cli.rs` (session
lookup, `state::dispatch`, DeepSeek launch and URLs, `merge_snapshot`,
`startup_failure`), `src/swarm/launch_project.rs` (`with_launch_binding`,
`worktree_catalog`), `src/accounts/{deepseek,inspection}.rs` and
`src/native_ui.rs`. This makes approach A practical.

### tmux coupling (Phase 2 input)

About 60 tmux call sites, concentrated in `src/cli.rs`, `src/platform.rs` and
`src/state/{terminal,lifecycle,input,rename,effort,storage,question_terminal,
interrupt,terminate,fork,clear_context}.rs`. They use a small vocabulary:
`send-keys` (16), `display-message` (14), `capture-pane` (6), `paste-buffer`,
`load-buffer`, `new-session`, `list-sessions` (3 each), and `set-option`,
`list-panes`, `kill-session`, `has-session`, `attach-session`. Process ownership
reads `/proc` in `src/state/processes/control.rs`, `src/metrics.rs`,
`src/state/supervisor.rs` and others.

### GUI portability gaps

The Qt GUI was not built on Windows. Source review found:

- `tray/src/PtyProcess.cpp` uses `forkpty`. The embedded terminal needs a ConPTY
  implementation.
- `tray/src/ComposerDraftStore.cpp` derives the drafts path from
  `QSettings().fileName()`. With the native format on Windows this is a registry
  path, not a file, so drafts would have no valid location.
- `tray/src/RelayConfiguration.h` checks POSIX ownership and modes behind
  `Q_OS_UNIX`.
- `tray/src/MainThreadWatchdog.cpp`, `tray/src/TrayAgent.cpp` and
  `tray/src/UpdateController.cpp` have macOS and Linux branches only. The updater
  knows `pacman`, AUR helpers and the macOS bundle.
- KDE-specific code is already Linux-only in `tray/CMakeLists.txt`. Other
  platforms use `QtTrayIcon`. `TerminalBackend` (Konsole, Terminal.app, clipboard)
  has no Windows implementation.

### CI budget

Actions cache usage was 1.9 GB of the 10 GB limit across 23 caches. A cached Qt
installation for MSVC with WebEngine (roughly 1.5 GB) fits. Standard Windows
runners are free for this public repository.

## Prior art: Orca

[stablyai/orca](https://github.com/stablyai/orca) (MIT, Electron) supports
Windows as a first-class platform. Source reviewed at `f307391` (2026-10-09).

- **Its own terminal daemon instead of tmux, on every OS.** `orcad` (Node and
  node-pty) runs separately from the app. On Windows it listens on a named pipe
  (`\\?\pipe\orca-terminal-host-v<protocol>-<hash>`) and spawns through a bundled
  `conpty.dll`/OpenConsole (`useConptyDll`) rather than the inbox ConPTY. Each
  session keeps a headless xterm emulator, which serves snapshots on reattach and
  scrollback restore after the daemon dies.
- **Job Objects own process trees.** Orca replaced PID-tree walks and
  `taskkill /T` with a Job Object per ConPTY. PID reuse and reparented
  descendants made the walk unreliable and left orphaned agents holding worktrees.
- **Updates do not kill sessions.** On Windows the daemon executable is staged
  into the user data directory, so the NSIS updater, which terminates processes
  running from the install directory, does not touch it. This matches the Zerus
  rule that GUI updates never restart agents.
- **Processes started over SSH die with the session.** Windows OpenSSH places
  session processes in a job that allows breakaway. Orca launches long-lived hosts
  with `CREATE_BREAKAWAY_FROM_JOB`. It once fell back to WMI process creation, but
  removed that: WMI is refused to a standard user's network logon
  (`stablyai/orca@5e9489c0`, `src/shared/windows-breakaway-launcher.ts`).
- **Hooks.** Agent hooks post to an HTTP server on `127.0.0.1`. Claude Code on
  Windows runs hooks through Git Bash, so hook commands branch on `$OSTYPE`. Orca
  measured Kaspersky denying `powershell -WindowStyle Hidden -EncodedCommand` at
  process creation (its issue #16003) and spells only `-NoProfile
  -EncodedCommand`.
- **npm `.cmd` shims are resolved.** A long `cmd.exe /c` line carrying an
  escaped prompt was flagged by Microsoft Defender for Endpoint as obfuscation.
  Orca reads the shim and runs `node.exe <script>` directly. As a side effect,
  multi-line prompts work.
- **WSL is optional.** It is offered when `wsl.exe --status` succeeds. Windows
  paths are translated to `/mnt/<drive>`, and a separate hook relay runs inside
  the guest.
- **Windows can be a remote host.** Orca deploys its relay over SSH with
  PowerShell commands for `win32-x64` and `win32-arm64`.
- **Windows code is spread throughout, not isolated in a port.** About 470
  source files contain `win32` branches, most of them in SSH, IPC and the
  runtime. Releases use an NSIS installer signed through the SignPath Foundation.

## Phase 1 design: control console

### `hgs.exe`

**Compiled out.** `state` (the whole local session host), `mobile_peers` and
`macos_session` become `cfg(unix)`. The client entry points listed above receive
Windows stubs.

**Works on Windows:**

- `--version`, `help`.
- `@host <command>` for every forwarded command, including interactive `a`,
  `resume` and session launch through `ssh -t` with the console inherited.
- `ls [--json]`: a local box with no sessions (`ok: true`, basic metrics) plus
  peer polling.
- `machine`: list, add, edit and probe. `machine setup` stays only if transferring
  the source tree works with the built-in `tar.exe`. Otherwise it fails with a
  clear message.
- `swarm`: local store, `sync`, `join` and conflict resolution.
- `account` and `project` when forwarded to `@host`. The local account catalog
  is empty.

**Local session commands** (`a`, `kill`, `send`, `inspect`, a launch without
`@host`, and the rest) exit with code 2 and the message "Local sessions are not
supported on Windows yet; choose a remote machine."

**Platform layer.** Split `src/platform.rs` into `platform/{mod,unix,windows}.rs`:

- **Home directory:** `HOME`, falling back to `USERPROFILE`. The path layout stays
  the same (`~\.config\hgs`, `~\.local\state\hgs\sessions`), as Claude Code and
  Codex keep `~\.claude` and `~\.codex`. `HGS_CONFIG_DIR` and `HGS_STATE_DIR`
  overrides keep working.
- **Executable lookup** honors `PATHEXT`. Exit codes do not use signals.
- **Interactive waiting** uses no `termios` or signals. `SetConsoleCtrlHandler`
  lets Ctrl+C reach `ssh.exe`.
- **Private files:** one helper applies `0600`/`0700` on Unix and relies on the
  user-profile ACL on Windows. It replaces direct `mode` and `from_mode` calls in
  `swarm/store.rs`, `swarm/launch_project.rs` and `machines.rs`.
- **Atomic replacement** keeps `rename`, with a short bounded retry on
  `EPERM`/`EACCES`/sharing violations, because antivirus scanners and the indexer
  can hold files.
- **Remote command quoting** is unchanged: the remote side is a POSIX shell.

**Child processes.** No `pre_exec`, `setsid` or process groups. If manual testing
shows console windows flashing when the GUI starts `hgs.exe`, which starts
`ssh.exe`, non-interactive children get `CREATE_NO_WINDOW`.

**Compatibility.** Unix builds keep their behavior. Each mechanical replacement
is covered by the existing Linux and macOS tests, run after every step.

### `hgs-tray.exe`

- **Build:** MSVC with Qt 6 Widgets, Network and WebEngineWidgets. No DBus or KDE
  Frameworks. Bundle `hgs.exe` next to `hgs-tray.exe` and prefer it over `PATH`.
- **Settings:** on Windows, call `QSettings::setDefaultFormat(QSettings::IniFormat)`
  before the first `QSettings` use, so the settings file and the
  `ComposerDraftStore` drafts path live under `%APPDATA%`.
- **Embedded terminal:** a Windows `PtyProcess` built on `CreatePseudoConsole`
  (Windows 10 1809+). It runs `hgs.exe @host a <session>`, which runs `ssh.exe
  -t`. Start with the inbox ConPTY. Bundle `conpty.dll` like Orca only if
  rendering or wrapping problems appear.
- **External terminal:** a `TerminalBackend` using Windows Terminal (`wt.exe`),
  with a plain console window as the fallback. "Open SSH terminal" uses the same
  backend.
- **Tray and notifications:** `QtTrayIcon` and `QSystemTrayIcon::showMessage`.
  The taskbar badge and Keep Above use standard Qt facilities, or stay disabled
  if those are insufficient.
- **Updater:** reports the installed version and links to the release page.
  There is no self-update in Phase 1.
- **Relay and mobile settings** are hidden on Windows in Phase 1.
- **Local machine** is presented as a control-only computer without a sessions
  section. The Machines page does not offer to launch local sessions.
- **Watchdog and single instance:** add a Windows branch to
  `MainThreadWatchdog`. `QLocalServer` already uses named pipes on Windows.

### Build, CI and packaging

1. **On the Windows development PC:** `cargo check` and `cargo test` for the
   client subset, then a CMake/Ninja build of the GUI with Qt for MSVC
   (`aqtinstall` or the Qt online installer). Write the steps into
   `CONTRIBUTING.md` as they are verified.
2. **Hosted CI:** a `windows-desktop` job on a standard `windows-2025` runner in
   `.github/workflows/checks.yml`:
   - Rust tests for the client subset;
   - Qt via `aqt install-qt` from a hash-locked aqtinstall closure in its own
     virtual environment (no unhashed PyPI code in the job), cached under the
     existing token rules (write only for exact upstream main pushes and schedules);
   - GUI tests with `QT_QPA_PLATFORM=offscreen`;
   - `windeployqt`, then a zip artifact with bounded retention.
   The job joins the CI gate only after it is stable.
3. **Nightly:** the publisher attaches the verified zip to the immutable nightly
   prerelease under the existing rules: publish only after full CI succeeds, never
   rebuild or overwrite assets, and never make a nightly the latest stable
   release. Stable release publication stays owner-triggered. A Windows asset for
   stable releases is a separate decision.

### Testing and verification

- **Rust unit tests on Windows** cover the platform layer: home fallback,
  `PATHEXT` lookup, POSIX quoting for remote commands, private-file helpers and
  rename retry.
- **Python integration tests** that require tmux remain Unix-only. A Windows
  smoke test runs `hgs.exe @peer ls --json` and a forwarded `inspect` against a
  peer whose `hgs` uses isolated `HGS_STATE_DIR` and `TMUX_TMPDIR`. It never
  touches live sessions.
- **GUI:** existing Qt tests run offscreen. Widget screenshots taken offscreen
  check layout without using the interactive desktop.
- **Manual checks on the development PC:**
  - Fleet, Activity, sending messages and the embedded terminal to a remote
    session;
  - no flashing console windows;
  - no antivirus blocks;
  - polling latency with several peers;
  - non-ASCII user names and a non-UTF-8 system code page.

## Phase 2 outline: native host

Phase 2 needs its own spec. The outline records the direction and the traps
already known.

- **Terminal-backend abstraction:** a trait over the tmux vocabulary above, with
  tmux on Unix and a Windows session host behind it. Questions, input and
  lifecycle code read screens and send input only through the trait.
- **Windows session host:** a Rust daemon owning ConPTY sessions.
  - A named-pipe endpoint versioned by protocol, with endpoint ownership rules
    that never remove an endpoint a host does not own.
  - A per-session screen model (for example the `vt100` crate, or the vendored
    libvterm) answering what `capture-pane` answers today, including the
    bracketed-paste state that tmux 3.7's `bracket_paste_flag` provides.
- **Process ownership:** a Job Object per session instead of `/proc` walks.
  Process inspection uses Toolhelp32 and process handles. File identity uses
  `GetFileInformationByHandle`.
- **Survival:**
  - The host breaks away from the SSH session job (`CREATE_BREAKAWAY_FROM_JOB`;
    WMI is no fallback, see Prior art) when started over SSH.
  - The host executable is staged outside the GUI install directory, so a GUI
    update never stops agents.
  - Like the tmux server today, the host survives GUI restarts.
- **Agents and hooks:**
  - `__state hook` commands must work when Claude Code runs them through Git Bash
    and when other agents run them through `cmd.exe`.
  - Avoid `powershell -WindowStyle Hidden -EncodedCommand`.
  - Resolve npm `.cmd` shims to `node.exe <script>` instead of running
    `cmd.exe /c`.
- **Windows as a remote peer:** OpenSSH on Windows defaults to `cmd.exe` or
  PowerShell, not a POSIX shell, so `~/.local/bin/hgs …` cannot be the remote
  command. Peers need a probed remote platform and a per-platform invocation, as
  Orca does.
- **WSL** can later become an optional extra target, not a replacement for the
  native host.

## Risks and open questions

- **SSH handshake cost.** Without multiplexing, each peer poll is a full
  handshake. Measure in Phase 1. If it is too slow, add a persistent connection,
  for example a long-lived `ssh host hgs <stdio service>`, as separate work.
- **SSH keys and agent.** The Windows `ssh-agent` service is disabled by default.
  `~\.ssh\config` semantics and `Include` paths differ slightly from Unix
  OpenSSH.
- **Console encoding.** On a non-UTF-8 system code page, child-process output
  (for example `wsl.exe`, which writes UTF-16) arrives garbled unless decoded
  explicitly. Treat SSH output as bytes, and set UTF-8 where `hgs` controls the
  console.
- **Antivirus.** Expect third-party antivirus and Microsoft Defender for
  Endpoint on user machines. Avoid command shapes known to be blocked, and test
  with antivirus enabled.
- **Inbox ConPTY rendering** may differ from tmux attach. Keep bundling
  `conpty.dll` as a fallback.
- **Qt WebEngine size** affects the zip and the CI cache. Measure before making
  the Windows job mandatory.

## Development environment

The first playground is a Windows 11 Pro PC on the owner's LAN. It is reachable
over OpenSSH with PowerShell as the default shell and has Visual Studio 2022,
Rust for MSVC, Git for Windows and Claude Code installed. Qt and WSL are not
installed. Access details live in the owner's private infrastructure inventory,
never in this repository or Beads.

Constraints that shape the work:

- The PC is shared. Use its interactive desktop only when the owner has confirmed
  it is free. Until then, verify the GUI offscreen.
- An OpenSSH session opened by an administrator is already elevated.
- Processes started over SSH end with the session unless they break away. Plan
  GUI and host experiments accordingly.
- Disk space is limited, so keep build directories and the Qt installation
  bounded.

## Starting checklist

1. Split `platform` into Unix and Windows modules. Add the home fallback and the
   private-file and rename helpers. Move `swarm/store.rs`,
   `swarm/launch_project.rs` and `machines.rs` to the helpers.
2. Gate `state`, `mobile_peers` and `macos_session` behind `cfg(unix)`. Add the
   Windows guard for local commands and the control-only `ls --json` local box.
3. Get `cargo check` and `cargo test` passing on the Windows PC, and keep the
   Linux and macOS suites green.
4. Run `hgs.exe @peer ls --json`, a forwarded `inspect` and swarm `sync` against
   an isolated peer. Measure polling latency.
5. Build `hgs-tray.exe` with Qt for MSVC. Add `IniFormat` settings, the ConPTY
   `PtyProcess`, the Windows `TerminalBackend` and the control-only local machine.
6. Run the GUI tests and screenshots offscreen. Do the manual desktop check when
   the PC is free.
7. Add the `windows-desktop` CI job, then the nightly zip.
8. Document Windows in `docs/installation.md` and `CONTRIBUTING.md`, and
   describe the Windows job and asset in `docs/ci-and-aur.md`.

## Implementation notes (2026-10-11)

Phase 1 follows the design above. User documentation is in
[installation](../../installation.md#windows), the
[reference](../../reference.md#windows),
[CONTRIBUTING](../../../CONTRIBUTING.md#windows) and
[CI](../../ci-and-aur.md#windows-client-desktop-and-portable-zip). These decisions
refine or change the plan:

- **Qt 6.10.3, not the newest Qt.** aqtinstall 3.3.0 cannot resolve the split
  repository layout of Qt 6.11 and later for Windows. CI installs Qt 6.10.3
  `win64_msvc2022_64` with `aqt install-qt` from a hash-locked closure
  (`scripts/ci/aqt-requirements.txt`) on a pinned CPython 3.13, instead of
  `install-qt-action`.
- **Minimum Windows 10 1903.** The `hgs-tray.exe` manifest sets UTF-8 as the
  process code page, which needs 1903. `hgs.exe` has no manifest yet (Phase 2 adds
  one). The pseudoconsole API (1809) is looked up at run time.
- **`local_sessions` key.** The local element of `hgs ls --json` on Windows is a
  control-only box with `"local_sessions": false`, empty `projects` and `sessions`,
  and metrics without load averages. The key is emitted only when false, so Unix
  output is unchanged. The GUI decides every control-only behavior from this key,
  not from `#ifdef`, so the same tests cover all platforms. The exceptions are the
  swarm worker supervisor and the disabled desktop presence, which depend on the
  platform. Local session commands exit with code 2; DeepSeek Native UI and mobile
  peers also exit 2 with their own messages. `account ls` and `account default`
  without an ID return an empty catalog, and `account copy` works between two
  remote machines. `macos_session` stays compiled: it already stubs non-macOS
  systems. `state` is replaced by fail-closed stubs in `src/state_windows.rs`.
- **`HGS_SSH`.** It overrides the OpenSSH client on every platform; tests inject
  their fake client this way. Windows otherwise uses
  `%SystemRoot%\System32\OpenSSH\ssh.exe`, then `PATH` with `PATHEXT`. The self
  name is `HGS_SELF`, else the lowercase `COMPUTERNAME`.
- **Overlapped pipes for `ssh.exe`.** Windows OpenSSH (9.5p2, inbox) never exits
  after it has used a synchronous pipe for one of its standard streams, even with
  no network. Rust's standard library, Python, PowerShell and the GitHub Actions
  runner create such pipes. It also never ends a remote command's input read from
  `NUL` or a non-empty file, and writes output files from offset 0, over what `>>`
  keeps. `hgs.exe` gives `ssh.exe` overlapped named pipes and relays its own
  stdin, stdout and stderr unless they are a console, waiting for slow readers and
  abandoning a relay only after 5 s idle in a read once `ssh.exe` has exited.
  Unpiped `ssh` input is a pipe that is already closed, and `machine setup` pumps
  its bundle through a pipe. CI steps read stdin from `/dev/null`, and the fake
  client refuses synchronous pipes, `NUL` or a non-empty file as input and output
  files that already hold data, so a regression fails instead of hanging.
- **`LocalCommand` through `cmd.exe`.** Windows OpenSSH runs `LocalCommand` with
  `_wsystem`, which ignores `SHELL`. The interactive link marker is
  `type nul > "%%HGS_LINK_MARKER%%"`, with the path in the ssh child's environment,
  so a `%` in the temporary path is not expanded. Marker folders are named
  `hgs-link-<pid>-<random>`; the next link removes those of ended processes, and a
  closing console removes its own. Unix keeps `touch '<path>'`.
- **Job objects.** `hgs.exe` places itself in a kill-on-close job at startup (best
  effort), so `ssh.exe` and `tar.exe` children die with it. Each ConPTY client, the
  supervised swarm worker and the test fixture shim have their own kill-on-close
  jobs. Interactive waits swallow Ctrl+C and Ctrl+Break with a console handler
  instead of `SetConsoleCtrlHandler(NULL, TRUE)`.
- **Tray-supervised swarm worker.** Windows has no user service manager. On Windows
  the tray runs one hidden `hgs.exe swarm worker` and restarts it with a delay that
  doubles from one second to five minutes. Executable-replacement detection works
  through volume serial number, file index and last-write time.
- **Windows Terminal with a console fallback.** The open modes `auto`, `tab` and
  `window` keep their names and mean Windows Terminal (`wt.exe`); `console` is new.
  When `wt.exe` is missing or would change an argument, `hgs-tray.exe --console-run`
  hosts a console window (`ConsoleHost`) that starts `hgs.exe` with the exact
  argument vector and stays open when it fails. Copied commands use PowerShell
  quoting, with `Start-Process` for arguments that Windows PowerShell 5.1 would
  pass incorrectly.
- **Control-only GUI.** Recovery settings on a control-only computer are saved to
  the first reachable remote machine. The local poll interval is at least 15 s,
  because every poll starts processes. Settings use the INI format, single
  instance uses a per-user pipe name and lock file, and Windows Error Reporting
  restarts the tray after a watchdog abort.
- **Nightly channel for the portable zip.** Only nightly prereleases carry
  `zerus-<version>-windows-x86_64.zip`; it is copied unchanged from the CI run,
  sealed in `SHA256SUMS` and checked again before upload. It is GitHub-only: the
  zerus.dev mirror neither stores it nor lists it in the v1 feed. A portable
  installation defaults Settings → Updates to the nightly channel and has no
  self-update. A stable Windows asset remains a separate decision. The Dev channel
  that arrived meanwhile has no Windows build either: desktop Dev candidates carry
  only the Arch package and source, and a portable installation on Dev never
  reports a dev build as an update.
- **Packaging.** `windeployqt` runs with `--no-opengl-sw`,
  `--no-system-d3d-compiler` and `--no-system-dxc-compiler`, and the ICU DLLs it
  copies from System32 are removed: Windows 10 1903 and later ship
  `D3DCompiler_47.dll`, and packaging and the checker reject the Direct3D and DXC
  compiler DLLs. The zip carries the app-local Visual C++ runtime and the Qt SPDX
  documents. It also carries the text of every license those documents name, from
  `packaging/windows/licenses`. The zip is about 138 MB, about 345 MB unpacked.
- **Python test fixtures and a native shim.** The GUI tests' shell fixtures were
  ported to Python for every platform. On Windows, `zerus-test-helper.exe` (static
  runtime, kill-on-close job) starts them with their command line unchanged and
  stands in for `/bin/sh`. Three session-window tests that need local folders are
  skipped on Windows. `tests/test_windows_client.py` drives `hgs.exe` against
  `examples/fake_ssh.rs`.
- **Machine setup from Windows.** `hgs machine setup` stays available. It packs the
  source checkout with System32 `tar.exe` (bsdtar; GNU tar extracts the result
  unchanged) and refuses a checkout whose `install.sh` or `hgs_state.py` has CRLF
  line endings. `.gitattributes` keeps `*.sh` and `*.py` in LF.
- **CI gate.** The `windows-desktop` job joined the CI gate and the nightly at
  once, not after a stability period as planned. Dev candidates of main pushes
  follow that gate too, so a Windows failure also holds back Dev. Clippy runs as
  `cargo +1.85.0 clippy -D warnings` over the Windows target. On the development
  PC, the parallel Qt suites showed load-sensitive timing failures that pass
  serially. Watch the first hosted runs.
- **Measured latency.** One `hgs.exe @peer ls --json` took about 150 ms over the
  LAN (median of ten, full handshake each time; the remote listing itself took
  13 ms). No persistent connection is needed for Phase 1.

Verified on the development PC against a disposable Linux peer:

- machine profiles, polling, forwarded commands and swarm join and sync;
- the Windows swarm worker;
- the packaged zip, started from an isolated extraction;
- a ConPTY attach built from `PtyProcess_win.cpp` running the real `hgs.exe` and
  `ssh.exe`: input, resize, Ctrl+C reaching the remote shell, and stop.

Not yet verified:

- the interactive desktop: Windows Terminal tabs, tray notifications, the taskbar
  badge and the embedded terminal inside the running app;
- a hosted `windows-2025` run.
