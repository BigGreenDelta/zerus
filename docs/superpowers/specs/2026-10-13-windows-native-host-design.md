# Windows Phase 2: native session host

Status: approved direction, implementation starting, 2026-10-13. Tracking:
`zerus-ziq3.7` under the Windows epic `zerus-ziq3`. Phase 1 (the control console)
is described in [Windows support](2026-10-10-windows-support-design.md).

The owner asked on 2026-10-13 to plan and implement Phase 2 and delegated the
design choices. This document records those choices, the evidence behind them,
the spikes that must confirm the riskiest assumptions on a real Windows PC, and the
order of work. Facts were checked against `885eb7d` with file:line evidence held in
the research notes summarized below.

## Goal

Agents (`claude`, `codex`, `kimi` and untracked commands) run natively on a Windows
computer under `hgs.exe`, with the same session model as Unix: a session survives
GUI restarts, GUI updates, terminal disconnects and the end of the SSH connection
that started it; it can be listed, inspected, sent to, attached, paused, resumed,
forked, renamed and terminated with the same CLI and GUI operations.

Unix behaviour stays byte-for-byte compatible: the same tmux argv, the same dry-run
strings, the same JSON, the same files.

## Non-goals for Phase 2

These stay out and get their own work later (Phase 3 and beyond):

- Windows as an SSH *target* of other peers (remote shell detection, per-shell
  invocation, Windows paths in the shared swarm catalog, `machine setup` for
  Windows peers).
- Windows as a mobile gateway (connector journal, `mobile_peers`, relay settings,
  desktop presence for phone alerts).
- DeepSeek (`dsh`) and its Native UI, voice-message workers.
- Installer, code signing, autostart at logon, winget, WSL as a target.

## Decisions

### D1. One binary, a hidden host entry

The host is `hgs.exe __host serve`, the same executable as the CLI, in the style of
`--macos-session-service` (`src/main.rs:26-28`). A separate binary would duplicate
the protocol, configuration and identity code.

### D2. Explicit enablement and staged images

Local sessions on Windows are off until the user enables them, from the GUI
("Enable local sessions" on the Machines page for this computer) or with
`hgs session-host install`. This keeps home-directory writes explicit, as for
`zerus-setup` on Linux and macOS. Enabling stages two things:

1. **The stable CLI** `%USERPROFILE%\.local\bin\hgs.exe`, the Windows form of the
   Unix `~/.local/bin/hgs` contract. Agent hooks and session recipes embed this
   path (`src/state/hooks.rs:136-137`, `src/state/lifecycle.rs:540,565`), so it must
   not live in the portable folder that a GUI update replaces. An update of the
   stable CLI renames the old file aside and moves the new one in; a running image is
   never overwritten.
2. **A content-addressed host image** `%USERPROFILE%\.local\share\hgs\host\<sha256-16>\`
   holding `hgs.exe` and, when bundled, `conpty.dll` and `OpenConsole.exe` (D6). A
   running host is never touched by an update. Unused images are removed by the next
   install when no host runs from them.

`hgs session-host status` reports the staged versions and the running host;
`hgs session-host stop` stops an idle host and refuses while sessions run. The
command name avoids `host`, which today launches the `host` program as a command.

`local_sessions` in `hgs ls --json` means "this computer is enabled for local
sessions", never "the host is running": saved sessions live in the state
directory, and a host crash must not turn the computer back into a control console.
On Windows the local box also carries `local_host` with `state` (`not-installed`,
`stopped`, `running`), `pid`, `protocol` and `conpty` (`bundled` or `inbox`). Unix
output is unchanged.

### D3. Host lifetime: like the tmux server

- The host starts on demand from a launch, resume or fork, never from a listing or an
  attach. A listing with no host reports no live sessions, as tmux does with no
  server.
- The host exits after its last session ends and no client is attached, after a
  short linger. The next launch starts the newest staged image. This is how upgrades
  reach the host without ever restarting live agents.
- The host owns its sessions: a host crash ends them, exactly as a tmux server crash
  does. The host joins its own kill-on-close job before the first session so its
  death never orphans agents (Orca does the same).
- The host has a hidden console (`CREATE_NO_WINDOW`), so console children it starts
  outside a pseudoconsole (`git`, probes) never flash a window, and a hidden
  top-level window receives `WM_QUERYENDSESSION`/`WM_ENDSESSION` to record a clean
  shutdown at logoff.

### D4. Breaking away from the launcher's job

The launcher (`hgs.exe` started by the GUI, a terminal or sshd) starts the host with
`CreateProcessW`, `CREATE_BREAKAWAY_FROM_JOB | CREATE_NO_WINDOW |
CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT`, no inherited handles (an
explicit empty handle list), then checks with `IsProcessInJob` that the host is not
in the launcher's job and fails closed with a clear message otherwise.

- `hgs.exe`'s own kill-on-close job (`src/platform/windows.rs:63-83`) gains
  `JOB_OBJECT_LIMIT_BREAKAWAY_OK`. Its `ssh.exe` and `tar.exe` children never ask
  for breakaway, so they still die with it.
- There is no WMI fallback. Orca, which the Phase 1 spec cited for one, removed it:
  WMI process creation is refused to a standard user's network logon
  (`stablyai/orca@5e9489c0`, `src/shared/windows-breakaway-launcher.ts:1-8`).
- Win32-OpenSSH places a session's processes in a job that allows breakaway, so a
  host started over SSH survives the end of the SSH session (spike S4 confirms).

### D5. Endpoint and protocol

**Name.** `\\.\pipe\hgs-host-<hex>` where `<hex>` is the first 24 hex digits of
SHA-256 over the user SID, the token elevation type and the case-folded canonical
state directory. Isolated tests (`HGS_STATE_DIR`) therefore never reach a live host,
an elevated SSH session and the desktop never share a host by accident, and `HOME`
versus `USERPROFILE` spellings of one directory map to one host. The name carries no
protocol version: there is one host per user and state directory, and versions are
negotiated (below).

**Security.** The first instance is created with `FILE_FLAG_FIRST_PIPE_INSTANCE`
and `PIPE_REJECT_REMOTE_CLIENTS` and a protected DACL granting access only to the
user SID (`D:P(A;;GA;;;<user>)`). The client checks the server before sending
anything: `GetNamedPipeServerProcessId`, then the process token user must equal its
own SID. The host checks each client the same way with
`GetNamedPipeClientProcessId`. A name squatted by another user is refused, never
used.

**Framing.** Byte-mode pipe, frames of `u32` little-endian length (at most 1 MiB),
one kind byte and a payload: `0` JSON request or response, `1` stream bytes, `2`
stream control JSON (resize, detach, exit, notice).

**Versions.** The hello carries the client's protocol range; the reply carries the
host identity (`pid`, creation time, image path, image hash, protocol, capability
list). Protocol 1 is the first. Changes are additive within a major version, and a
CLI keeps speaking every major version a running host may still use. This mirrors
how a newer hgs keeps working with an older running tmux server.

**Requests** cover the tmux vocabulary used today (62 call sites, about 70
invocations, `src/state/**`, `src/cli.rs`, `src/platform/unix.rs`): create session,
list sessions and panes with all fields in one answer, resolve a target, pane info,
capture (plain or styled), send keys, paste (bracketed or not), set and read the run
marker and session environment, rename, kill, signal, attach, list clients, and host
status. `list` is a single round trip, because hooks call `live()` on every event
(`src/state/hooks.rs:283`) and every `ls --json` reads the screen of each session
with a pending question.

### D6. ConPTY: bundled pair first, inbox as degraded fallback

- The host loads `conpty.dll` by absolute path from its image folder with
  `LoadLibraryExW` and uses `ConptyCreatePseudoConsole`; `conpty.dll` finds the
  matched `OpenConsole.exe` next to itself. The pair comes from the pinned NuGet
  package `Microsoft.Windows.Console.ConPTY` (MIT), verified by SHA-256 in the
  packaging script, and ships in the portable zip with its license text.
- Without the pair the host falls back to the inbox `CreatePseudoConsole` and reports
  `conpty: inbox`. Inbox ConPTY on Windows 10 is the older snapshot renderer, and its
  forwarding of mode changes such as bracketed paste is unverified; features gated
  on that evidence then fail closed, as they do on Unix without it.
- Never pass `PSEUDOCONSOLE_INHERIT_CURSOR` (it deadlocks unless the caller answers
  a cursor query). `ClosePseudoConsole` runs off the reader thread; use
  `ReleasePseudoConsole` when exported.
- Spike S1 decides whether the bundled pair is required or optional.

### D7. Screen model and capture

Each session feeds its pseudoconsole output into a `vt100` 0.16 parser (MIT,
MSRV 1.70; adds `vte`, `arrayvec`, `unicode-width` to the lockfile). hgs owns the
serializer that answers what `tmux capture-pane -p` and `-p -e` answer today:

- visible screen only, every row, trailing spaces trimmed;
- one character per wide glyph while the cursor counts cells;
- SGR only, carried across rows the way tmux does, `\e[0m` when an attribute is
  removed, 256-colour and truecolour forms as tmux writes them; OSC 8 is not kept
  (consumers only strip it);
- pane facts: size, cursor, alternate screen, bracketed paste (DECSET 2004),
  application cursor keys, mouse modes.

`vt100`'s own `rows_formatted` is not used: it emits erase and cursor sequences that
the parsers reject (`src/state/question_terminal.rs:38-80`). A differential test on
Linux feeds the same recorded byte streams to tmux 3.7 and to the model and compares
both capture forms byte for byte; it runs in the existing Linux CLI job. The screen
model is platform-independent code, so its tests also run on Linux and macOS.

On attach the host repaints the client from the model (`vt100` state and mode
formatting is suitable here) and then streams live output.

### D8. Launch without a login shell

On Unix the pane runs `$SHELL -lic 'exec env … "$0" --run "$@"' hgs 0 argv` so the
login PATH applies and the supervisor becomes the pane's root process. On Windows:

- the creating client (the `hgs.exe` that asks for a session) sends its whole
  environment block plus the session overrides (`HGS_*`, account homes, `TERM`), and
  the host starts `hgs.exe --run <argv>` from the staged stable CLI with exactly that
  block. A long-lived host never hands out its own stale `PATH`;
- `--run` keeps its role: it is the supervisor, the pane's root process
  (`pane_pid`), it installs hooks, writes the record and starts the agent;
- the supervisor is created suspended, assigned to the session job (D9), then
  resumed;
- the environment adds `TERM=xterm-256color` and removes `COLORTERM`, and sets
  `FORCE_COLOR=2` for Node agents only if spike S1 shows they otherwise emit
  truecolour: the question parsers match exact 256-colour indices
  (`src/state/claude_question.rs:140,156`);
- `HGS_HOST_PANE` and `HGS_HOST_SESSION` replace `TMUX_PANE` and the session name
  lookup (`src/state/lifecycle.rs:95-117`); the nested guard checks them as it
  checks `$TMUX` today.

**Agent resolution.** `PATHEXT` lookup (`platform::which`) finds the program. An npm
shim (`.cmd`) is resolved to `node.exe <script>` only when it matches the exact npm
shim shape, all or nothing, because a long `cmd.exe /c` line carrying a prompt is
scored as obfuscation by Defender for Endpoint (Orca's evidence) and breaks
multi-line arguments. Native executables (`claude.exe`, Kimi's single binary,
Codex's vendor `codex.exe` reached through `codex.js`) run directly. Windows PATH
preparation adds `%USERPROFILE%\.local\bin` and `%APPDATA%\npm` when present, the
counterpart of `platform/unix.rs:22-43`. The `sh` command means PowerShell
(`pwsh.exe`, else `powershell.exe -NoLogo`) and stays untracked.

### D9. Process ownership and identity

- **Jobs.** Each session has its own job, without `KILL_ON_JOB_CLOSE` and without
  `BREAKAWAY_OK` (Git Bash children would otherwise escape it). The host's own
  kill-on-close job (D3) contains all session jobs. A supervisor started by the host
  does not create its own kill-on-close job.
- **Identity.** `process_start` on Windows is the creation time as decimal 100 ns
  ticks, compared for equality like the Unix `lstart` text; records are per machine.
  A pinned process is an open handle (`SYNCHRONIZE | PROCESS_TERMINATE |
  PROCESS_QUERY_LIMITED_INFORMATION`), which also blocks PID reuse. Parent links
  from Toolhelp32 are trusted only when the parent was created before the child.
- **IDs.** Sessions and panes keep the textual forms `$N` and `%N` with host-unique
  counters, because records, rename transactions and question hashes store them and
  validate their shape. The terminal binding hashes the host PID and creation time,
  as it hashes the tmux server's today.
- **Exit evidence.** NTSTATUS crash codes and `STATUS_CONTROL_C_EXIT` map to a
  completion reason without changing Unix values; the `--run` fallback test
  (`src/cli.rs:445`) treats them as a crash, not as "nothing to resume".

### D10. Input evidence without termios

On Unix, sends require the pane's tty to have `ICANON` and `ECHO` off, the agent to be
the foreground process group and not a shell (`src/state/input.rs:541-574`), plus
bracketed paste on. Windows evidence for "the agent owns raw input":

- the screen model reports bracketed paste on (TUI agents enable it only while their
  editor is active);
- the session's tracked agent process is alive and belongs to the session job;
- the existing agent-specific composer checks pass unchanged.

The backend returns these as one `input_state` answer. Spike S2 checks for false
positives (an agent's shell escape, a crashed agent leaving a shell).

### D11. Stopping, pausing and terminating

- **Pause** (Unix: `SIGTERM` to the exact agent PID, never a forced kill) sends
  `CTRL_BREAK_EVENT` to the agent's process group. The supervisor starts the agent
  with `CREATE_NEW_PROCESS_GROUP` for this; agents read Ctrl+C as input in raw mode,
  so typing is unaffected. The host delivers the event through a short-lived helper,
  `hgs.exe __console-signal <pid> break`, that attaches to the session console. Pause
  never force-kills on Windows either.
- **Terminate** (Unix: `kill-session`, the hangup reaches the agent) closes the
  pseudoconsole, which sends `CTRL_CLOSE_EVENT` to every process on it, waits up to
  10 s as `src/state/terminate.rs:63` does, then terminates the session job.
- **Supervisor** handles console control events instead of signals: it ignores
  Ctrl+C and Ctrl+Break, and treats close, logoff and shutdown as hangup.
- Spike S5 records how each agent reacts and that resume works afterwards.

### D12. Hooks on Windows

Hook bytes, once written, are kept (`src/state/hooks.rs:152-162,211-221`), so the
first Windows form must be final:

- the command names the stable CLI by its absolute path with forward slashes in
  double quotes, followed by `__state hook`, which Git Bash, `cmd.exe` and a direct
  `CreateProcess` all parse the same way;
- Claude Code uses its exec form (command plus `args`, no shell) when the installed
  version supports it, else that shell form under Git Bash or PowerShell;
- Codex uses its Windows command override when present;
- the Kimi TOML block is found with CRLF-tolerant markers, so a CRLF `config.toml`
  never gets a second block;
- never `cmd.exe /c` with a prompt, never `powershell -WindowStyle Hidden
  -EncodedCommand` (Kaspersky blocks it at process creation, Orca issue 16003).

Spike S6 fixes the exact form per agent version.

### D13. Attach

- `hgs.exe a <session>` connects to the host, switches its console to virtual
  terminal input and output, receives a repaint and then relays bytes both ways.
  Console size changes are forwarded. `Ctrl+B d` and `Ctrl+B Ctrl+D` detach, as with
  the default tmux prefix and the suggested `tmux.conf`.
- Multiple clients may attach. The size follows the most recent client, as with
  `window-size latest`; with no client the last size stays.
- `--existing --run-id` never creates or restores a session and refuses a changed
  run, as today.
- Host death during an attach restores the terminal modes and prints the message the
  tmux path prints when the server dies.
- The GUI's embedded terminal keeps running `hgs.exe a NAME --existing --run-id`
  inside its own pseudoconsole. This doubles the ConPTY layer, but needs no GUI
  protocol change; a direct pipe transport is a later optimization if rendering
  suffers.
- `clients` in `ls --json` lists opaque client IDs on Windows (no tty paths).

### D14. Workers

The host supervises the recovery worker (`hgs.exe recovery worker`, from the stable
CLI) with a delay that doubles from 1 s to 5 min, because recovery belongs to the
session lifecycle and Windows has no user service manager. The swarm worker stays
tray-supervised as in Phase 1. The voice worker is out of scope; voice sends to a
local Windows session are refused with a clear message.

### D15. Paths and files

- `platform::canonical` returns canonical paths without the `\\?\` prefix; every
  stored or compared path uses it (38 `canonicalize` sites in `src/state`).
- Comparisons of local paths on Windows are case-insensitive; session names derive
  from the canonical (on-disk) casing of the folder.
- `hgs.exe` gets an application manifest (UTF-8 code page, `longPathAware`), as
  `hgs-tray.exe` already has; today only the GUI has one.
- `.gitattributes` keeps `tests/fixtures/*.txt` and `src/state/data/*.txt`
  byte-exact (`-text`), so CRLF checkouts never change the fixtures that
  `include_str!` embeds.
- Windows local folders stay in the local project store. They are never published
  to the shared swarm catalog, which accepts only `/` paths on every current peer
  (`src/swarm/catalog.rs:78`); a catalog kind for Windows paths is Phase 3.

### D16. GUI

- Machines page: "Enable local sessions" for this computer when `local_host.state`
  is `not-installed`, running `hgs.exe session-host install`; status of the host.
- Path model per machine from `metrics.platform`: drive and UNC roots accepted for
  the local Windows machine wherever `startsWith('/')` gates folders today
  (`SessionOrganization.cpp:24,176,187`, `ProjectsDialog.cpp:351`,
  `NewSessionDialog.cpp:27,122,462`, `SessionsWindow.cpp:2229,2954,4132,4184,4274`,
  `SessionCommands.h:109-113`). Stored organization JSON stays readable by Unix GUIs.
- Terminal file drops quote for the target platform; the local folder shell opens
  PowerShell in that folder through Windows Terminal.
- New session lists only agents the machine supports (no `dsh` on Windows).
- Mobile connection settings stay hidden on Windows independently of
  `local_sessions` (today they are coupled in `SettingsPage::setLocalSessions`).
- Status text no longer names tmux ("the session continues on <machine>").
- Local polling keeps a platform minimum interval on Windows until the host can push
  changes.

## Spikes before the dependent milestones

Run on the Windows development PC with disposable state and isolated endpoints,
never touching live sessions. Each spike records its results in Beads and adjusts
this document.

| Spike | Question | Blocks |
|---|---|---|
| S1 | With inbox and bundled ConPTY, real Claude, Codex and Kimi: are `?2004h`, `?1049h` and cursor visible to the host? Are cursor and device queries answered with no client attached? Which colours and glyphs do the agents use with `TERM=xterm-256color`, with and without `FORCE_COLOR`? | D6, D7, D8 |
| S2 | Bracketed paste markers, a lone ESC followed by text, Ctrl+C, Ctrl+S and Enter 180 ms after a paste reach each agent intact through the host. Input evidence (D10) has no false positive with an agent's shell escape or an exited agent. | D10, M5 |
| S3 | Record Windows screens of every dialog the parsers read (trust, permission, approval, questions, effort and model pickers, update, resume, clear) for the parser fixtures. | M5 |
| S4 | Breakaway works from `hgs.exe` started by the GUI, by a console and by sshd; the host and its agents survive GUI exit, GUI update and SSH logout; credentials of agents started by a host that sshd started (DPAPI, Credential Manager). | D4, M4 |
| S5 | Each agent's reaction to `CTRL_BREAK_EVENT` and `CTRL_CLOSE_EVENT`; resume after both. | D11 |
| S6 | Hook forms per agent version: Claude exec form and Git Bash form, Codex Windows command, Kimi command shell. Measure hook latency with Defender on. | D12 |
| S7 | Startup cost of `hgs.exe` and hooks under real-time scanning; whether SQLite journal writes stay within the 5 s hook timeout. | M5 |

## Milestones

Each milestone lands with its tests; Unix stays byte-for-byte compatible after every
one.

- **M0 Spikes** S1-S7 (above).
- **M1 Unix-safe groundwork.**
  1. Guardrails: a Linux type-check of the Windows MSVC target
     (`scripts/ci/check-windows-target.sh`, a fake C compiler for build scripts) and
     a golden suite that records every tmux argv the CLI and state emit.
  2. Shared-helper runtime traps fixed with no Unix change: directory sync, rename
     retry, lock contention, `home()`, `canonical`, executable check, append-mode
     lock files.
  3. Platform abstractions: process (presence, start time, table, describe, current
     directories, pinned handles), file identity and private files, control (exit
     info, shutdown guard, detached spawn, bounded runner), directories.
  4. `TerminalBackend` trait with the tmux implementation; every call site moves
     behind it; tty and `ps` checks become `input_state`.
  5. `state` compiles on Windows with an unavailable backend; `state_windows.rs` is
     removed; the 265 `state` unit tests run in the Windows CI job.
- **M2 Screen model** with the differential tests (D7).
- **M3 Session host**: endpoint, protocol 1, ConPTY sessions, jobs, staging,
  breakaway start, `session-host` command (D1-D6, D9).
- **M4 Windows lifecycle**: host backend client, launch (D8), supervisor and
  signals (D11), identity, input evidence (D10), hooks (D12), recovery worker (D14).
- **M5 Attach client and listings** (D13, D2's `local_host`).
- **M6 GUI** (D16).
- **M7 Tests, CI, packaging and docs**: a native `examples/fake_agent.rs` (raw input
  recorder, fixture renderer, mode toggles, hook emitter); one backend contract
  suite run against tmux on Unix and against the host on Windows; host survival
  tests (start inside a kill-on-close job emulating sshd's); real-agent Windows
  suites (keyless Claude against the local fake API, pinned win32 Codex); the
  ConPTY pair in the zip and its checker; installation, reference, CI and
  CONTRIBUTING updates.

Local sessions on Windows are labelled a preview in the GUI and documentation until
the real-agent Windows suites pass in CI. A skipped native suite is not
compatibility evidence.

## Risks

- **Screen fidelity.** Parsers fail closed if serialization, glyphs or colours
  differ. The differential test and recorded Windows fixtures come before parsers
  are trusted on Windows.
- **Input through ConPTY.** Bracketed paste, ESC timing and control keys are
  unverified; S2 gates sending on Windows.
- **Antivirus and enterprise policy.** Unsigned executables staged in the user
  profile can be blocked by AppLocker, WDAC or Defender ASR rules; every hook starts
  `hgs.exe`. The host reports blocked starts clearly; signing is a later decision.
- **Logoff.** Windows ends a user's processes at logoff, so sessions end there, as
  user tmux servers do without lingering.
- **Elevation.** An administrator's SSH session is elevated. Its host is separate
  (D5) and runs agents elevated; the documentation says so.
- **Call volume.** Each hook and listing becomes a pipe round trip instead of tmux
  process launches; the host answers listings in one request.
- **Upgrade compatibility.** A newer CLI must keep talking to an older running host
  until its sessions end; protocol majors are retired only after no released host
  uses them.
