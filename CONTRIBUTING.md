# Contributing

Build requirements are documented in `docs/installation.md`. Keep fixtures synthetic and
machine connection settings local. `AGENTS.md` describes the Beads workflow.

## Proposals and repository labels

A feature PR is a proposal until maintainers accept its product scope. Before
adopting it, establish the use cases, configuration and lifecycle behavior, and
behavior on every supported platform. Optional third-party tools can be useful
extension proposals without belonging in the Zerus core. Deferring one does not
commit the project to implementing it or to a particular extension API.

Keep the existing `bug`, `enhancement`, `documentation` and technical labels.
Use these additional labels to retain product decisions on issues and PRs:

| Label | Meaning |
| --- | --- |
| `proposal` | An idea whose product scope has not been accepted. |
| `deferred` | Retained for future consideration, without an active implementation or release commitment. |
| `extensions` | An optional third-party integration suited to a future plugin or extension. |
| `partially-adopted` | Part of a PR was adopted separately; the closing comment records what happened to the rest. |

Close deferred implementation PRs with a short explanation and retain their
labels, commits and discussion. Use `wontfix` for work the project does not plan
to take, rather than for an idea being kept for later. When a PR mixes a bug fix
and an unaccepted feature, investigate and take the fix separately with author
attribution; explain exactly what was adopted before closing the original PR.
Beads continues to own accepted implementation tasks and blockers.

To revisit retained ideas, search GitHub for
`is:pr is:closed label:proposal label:deferred`; add `label:extensions` to find
plugin candidates. Search `is:pr is:closed label:partially-adopted` to find PRs
whose fixes were taken separately.

## Local checks

```sh
python3 scripts/ci/check-source.py
python3 -m unittest discover -s tests -p 'test_*publication.py'
bash scripts/ci/cli.sh
bash scripts/ci/gui.sh
```

The CLI script clears inherited `HGS_*` session/account context, runs Rust unit
tests and terminal smoke tests, then runs the Python integration test classes in
parallel fresh processes and builds a release binary. Classes larger than twelve
tests run as several jobs unless they share a class fixture, slowest first by
the previous local run, or else by `scripts/ci/python-durations.json`. The
desktop script builds with `BUILD_TESTING=ON` and runs the complete Qt suite
headlessly in parallel (`build` or `test` runs only that half). Each suite
registered with `zerus_add_test` also writes per-function JUnit results to
`artifacts/test-results/qt-functions/`. `test_sessionswindow` runs as ten
shards of its test functions and of the data rows of its slowest functions,
balanced by the durations in `tray/tests/qt-shard-durations.txt`; tests that
file does not list go by a hash of their name, and every shard fails unless
each function and row runs in exactly one shard. `scripts/ci/qt-shard-seed.py`
rebuilds the file from the `qt-functions` results of the CI desktop jobs. Tray
sources compile once, into the `hgs-tray-*` static libraries that the
application and the suites link: a new suite links the lowest library that
covers its code instead of listing `tray/src` files, which fails configuration.
`ZERUS_TEST_JOBS` sets both test parallelisms (defaults: at most 16 Python and
8 Qt jobs, never more than the CPU count; `1` runs them one after another);
`ZERUS_PYTHON_JOBS` overrides it for the Python classes. Test logs and Python
job durations live in ignored `artifacts/test-results/`. Set
`ZERUS_BUILD_JOBS` to limit desktop build parallelism. Zerus tracks the newest
stable Rust: `rust-version` and the toolchain pinned in CI follow the latest
stable release, and raising them is routine maintenance. Run
`cargo +1.99.1 test --locked` for that pinned toolchain.

`check-source.py` keeps the desktop's Unix children from inheriting its descriptors
([ChildProcess.h](tray/src/ChildProcess.h)): in `tray/src` it rejects
`QProcess::startDetached()` and `QProcess::execute()`, and every `QProcess` must go to
`ChildProcess::inheritStdioOnly()`, directly or through a function that passes its
parameter there, in the function that creates it (for a member, a constructor of its
class) and before it starts.
`CHILD_PROCESS_EXEMPT` in the script names the files that may start children
otherwise, each with its reason.

A change to tmux or `ps` call sites must keep Unix behaviour unchanged: compare a
[tmux census](docs/ci-and-aur.md#unix-tmux-census) of the branch with one of main.
`scripts/ci/check-windows-target.sh` type-checks the Windows target from Linux or
macOS.

On macOS the Rust standard library marks a child's pipes close-on-exec only after
creating them, so a child that another thread starts in between inherits them and
holds that exchange's input or output open until it exits. Where threads of one
`hgs` process can spawn at the same time (mobile peer discovery, speech computer
discovery), every such spawn goes through `platform::spawn_in_turn`.

The CLI script also builds `examples/fake_agent.rs`, a native stand-in agent for
terminal tests that need a real process rather than a script, such as those that
will run under the Windows session host. It records every input byte with its
framing (bracketed paste, ESC, Ctrl+C, Enter) and timing, renders fixture byte
streams, sets and resets terminal modes, sends terminal queries and records their
answers, runs hooks with agent-shaped JSON, writes its process ID, start time and
folder when ready, and follows a configured policy for signals and Windows console
events; its header lists the options and the record format. Linked or copied under
an agent's name, it takes its options from `FAKE_AGENT_*` variables and runs the
hooks in that agent's configuration. `tests/test_fake_agent.py` checks it under a
private tmux socket; `HGS_TEST_FAKE_AGENT` names another build.

`tests/test_backend_contract.py` uses it as `claude` for the
[terminal backend contract](docs/ci-and-aur.md#terminal-backend-contract): every
session operation a backend provides, driven and observed only through `hgs`
commands, their JSON and the agent's records. `HGS_TEST_BACKEND` selects `tmux`
(default on Linux and macOS) or `host` (the Windows session host, default on
Windows; see [testing the host](docs/windows-host-testing.md#the-terminal-backend-contract)).
A backend class there provides only its setup and teardown, `running()`,
`describe()`, `alive()` and attach client; extend the contract rather than the
backend classes when a behaviour must hold for both.

On Arch, `bash scripts/ci/arch.sh` also builds and validates the pacman package
as an unprivileged user. It substitutes a local Git source only in its disposable
build directory; it never installs the package or enables services.
Hosted CI runs full Linux CLI, Arch desktop/package, macOS and Windows checks
automatically on pushes to main and PRs targeting main. Manual runs default to full
coverage; explicit suite inputs can narrow a manual/reusable run. Validate workflow
edits locally before pushing. Daily nightlies publish a verified binary to GitHub and
AUR, and the Windows portable zip to GitHub, only after every check passes; stable
releases, which carry the zip their own candidate run tested, remain manually published.
See [CI and Arch publication](docs/ci-and-aur.md) for triggers and publication
requirements.

Native integration tests may need separately installed agent CLIs or a real desktop;
read each suite's setup before running it. Do not run tests against live agent state.
Some platform tests only exist on macOS, Linux or Windows. Record any unavailable
checks.

The OpenCode 2 native suite needs an installed OpenCode 2.0.32 or newer 2.x. Run it
locally on request; full Linux and macOS CI run it with the pinned minimum. See
[OpenCode qualification](docs/opencode.md#qualification).

Color-popup dismissal also needs Cocoa qualification on macOS. These cases post
mouse events through the fixture's native Cocoa window; Qt-injected window events
bypass the native modal-session filter that previously swallowed outside clicks.
The dismissing click must cancel without activating the control behind the popup.

```sh
QT_QPA_PLATFORM=cocoa .ci-build/gui/tests/test_machinespage colorPickerDismissalKeepsTheSavedColor
QT_QPA_PLATFORM=cocoa .ci-build/gui/tests/test_projectsdialog projectColorChangesRequireApply customProjectColorRemainsStaged
QT_QPA_PLATFORM=cocoa .ci-build/gui/tests/test_sessionswindow colorPickerUsesWorkspaceThemeAndCancelsOutside
```

On a KDE Plasma 6 Wayland desktop, the optional native window-layer check uses
its own temporary settings and synthetic windows, verifies KWin's actual state,
and leaves unrelated windows and desktop rules alone:

```sh
QT_QPA_PLATFORM=wayland ZERUS_TEST_KWIN=1 \
  tray/build/tests/test_kwinwindowlayer nativeKWinChangesOnlyOurWindowAndRestores
QT_QPA_PLATFORM=wayland \
  tray/build/tests/test_sessionswindow windowLayerControlsKeepFocusAndLayout
```

The ordinary `kwinwindowlayer` test runs on a private D-Bus session with a fake
compositor and never controls the user's desktop.

For the Rust mobile relay, run `cargo +1.99.1 test --locked` inside
`services/mobile/relay`. The Docker image uses that Rust binary. For the Rust
mobile connector, run `cargo +1.99.1 test --locked` inside
`services/mobile/connector`; [its README](services/mobile/connector/README.md)
names the variables its Python and `hgs` handoff tests need and how the
black-box suites run against its binary. For the Python connector and reference
compatibility fixtures, install `services/mobile` and
`services/mobile/tests/requirements.txt` in a Python 3.11+ virtual environment
and run `python -m unittest discover -s services/mobile/tests -v`.
The relay storage contracts and the connector HTTP suites need PostgreSQL:
without `ZERUS_RELAY_TEST_DATABASE_URL` the contracts use SQLite and the HTTP
suites skip instead of running the Rust relay. Start a disposable server with
one line:

```sh
docker run --rm -d --name zerus-relay-test-postgres -e POSTGRES_HOST_AUTH_METHOD=trust -p 127.0.0.1::5432 postgres:17.11-alpine -c max_connections=300
```

Then follow [the relay's PostgreSQL tests](services/mobile/relay/README.md#postgresql-tests)
for the variables and commands; each test creates and drops its own database.
These tests use synthetic subprocesses and isolated databases, without live native
agents. For Android, run `./gradlew testDebugUnitTest lintDebug assembleDebug` from
`mobile/android` with JDK 17 or 21 and Android SDK 36. Every CI run, PRs included,
also runs `testDebugUnitTest lintRelease` with `-PzerusFirebase=true` and a
synthetic `google-services.json`. Follow
[mobile setup](docs/mobile-deployment.md) for device and notification checks.

Beads issues, history and memories must contain only information suitable for public
source collaboration. An ignored local database still becomes remote data when
`bd dolt push` is run. Review the exported issues and memory before synchronizing.

Quick checks run Gitleaks with its default rules on the working tree and all Git
history of every push and pull request; findings are redacted from the public log
and `gitleaks:allow` comments do not hide them. For release preparation, also run
Gitleaks on the candidate source archive and Beads data, and review binary assets
and dependency notices. The local privacy script catches common accidental machine
paths and keys; it is not a complete secret scanner.

<a id="windows"></a>

## Windows

Windows builds the [portable zip](docs/installation.md#windows): `hgs.exe`, whose
session host runs local sessions once they are enabled, and `hgs-tray.exe`.
You need:

- Visual Studio 2022 with the x64 C++ tools and a Windows SDK; its own CMake
  (3.22 or later is needed for the Windows tests) and Ninja are used;
- Rust for MSVC: stable, and 1.99.1 with clippy;
- Python 3.9 or later for the Qt test fixtures and 3.11 or later for the
  packaging scripts;
- Git for Windows with Git Bash, and 7-Zip;
- Qt 6.10.3 `msvc2022_64` with Qt Multimedia (no Qt WebEngine: the embedded
  DeepSeek Native UI is an optional Linux plugin).
  CI installs the official archives pinned in `packaging/windows/qt.json`, each
  verified before 7-Zip extracts it. The same works locally and makes
  `~/Qt/6.10.3/msvc2022_64` (`--seven-zip` names 7-Zip when `7z` is not on
  `PATH`):

```sh
python scripts/install-qt-windows.py --output ~/Qt
```

Clone with `git clone -c core.autocrlf=false`, as CI does: test fixtures and
scripts must keep LF line endings. Then, from Git Bash in the checkout:

```sh
cargo test --locked </dev/null
cargo +1.99.1 clippy --locked --all-targets -- -D warnings
cargo build --release --locked
cargo build --locked --example fake_ssh
cargo build --locked --example fake_agent
HGS_TEST_BIN="$(cygpath -w "$PWD/target/release/hgs.exe")" \
HGS_TEST_FAKE_SSH="$(cygpath -w "$PWD/target/debug/examples/fake_ssh.exe")" \
  python -m unittest discover -s tests -p test_windows_client.py -v </dev/null
python scripts/fetch-conpty.py --output target/conpty
HGS_TEST_BIN="$(cygpath -w "$PWD/target/release/hgs.exe")" \
HGS_TEST_FAKE_AGENT="$(cygpath -w "$PWD/target/debug/examples/fake_agent.exe")" \
HGS_TEST_CONPTY_DIR="$(cygpath -w "$PWD/target/conpty")" \
  python -m unittest discover -s tests -p 'test_windows_host*.py' -v </dev/null
# Downloads the pinned win32-x64 Claude Code and Codex for their real-agent suites:
HGS_TEST_BIN="$(cygpath -w "$PWD/target/release/hgs.exe")" \
HGS_TEST_CONPTY_DIR="$(cygpath -w "$PWD/target/conpty")" \
  python -X utf8 scripts/ci/agent-compatibility.py </dev/null
export QT_ROOT_DIR="$(cygpath -w ~/Qt/6.10.3/msvc2022_64)"
bash scripts/ci/gui.sh </dev/null
cargo +1.99.1 test --locked --manifest-path services/mobile/connector/Cargo.toml --lib windows_management::tests
cargo +1.99.1 test --locked --manifest-path services/mobile/connector/Cargo.toml --test windows_gateway
cargo +1.99.1 build --release --locked --manifest-path services/mobile/connector/Cargo.toml
HGS_TEST_CONNECTOR="$(cygpath -w "$PWD/services/mobile/connector/target/release/zerus-mobile-connector.exe")" \
HGS_TEST_BIN="$(cygpath -w "$PWD/target/release/hgs.exe")" \
HGS_TEST_FAKE_AGENT="$(cygpath -w "$PWD/target/debug/examples/fake_agent.exe")" \
  python scripts/ci/outside-job.py --custody-only --timeout 900 \
    --log artifacts/test-results/windows-mobile-gateway.log -- \
    python -m unittest discover -s tests -p test_windows_mobile_gateway.py -v </dev/null
python scripts/package-windows.py --cli target/release/hgs.exe \
    --connector services/mobile/connector/target/release/zerus-mobile-connector.exe \
    --gui .ci-build/gui/hgs-tray.exe --conpty target/conpty \
    --output artifacts/zerus-windows-x86_64.zip
python scripts/check-windows-portable.py artifacts/zerus-windows-x86_64.zip --run --selftest
```

`tests/test_windows_client.py` runs `hgs.exe` against `examples/fake_ssh.rs`,
injected through `HGS_SSH`, with `HOME` and `USERPROFILE` in a temporary folder;
it never uses your SSH configuration and skips itself on other systems. The
native session host of Windows Phase 2 has its own isolated recipe and tests,
`tests/test_windows_host.py`, the bundled ConPTY tests in
`tests/test_windows_host_conpty.py`, the survival tests in
`tests/test_windows_host_survival.py` and the real-agent suites in
`tests/test_windows_native_agents.py`, which `scripts/ci/agent-compatibility.py`
runs with the reviewed Claude Code and Codex packages, each against a local fake
API: [testing the Windows session host](docs/windows-host-testing.md).

Kimi's optional native input, question and startup-trust fixtures are
`test_kimi_native.NativeKimi` (two cases),
`test_kimi_questions_native.NativeKimiQuestions` (three) and
`test_kimi_questions_native.NativeKimiTrust` (three). Run them only with an
explicitly reviewed `HGS_KIMI_TEST_BIN` and exact `HGS_KIMI_TEST_SHA256`, a sealed
`HGS_TEST_BIN`, private helper PATH and evidence directory; never point them at
the installed Kimi. Each case runs a private copy, disables automatic updates,
uses its loopback model and platform fixture, proves the child image and private
environment, and retains uncertain cleanup. The macOS cases compile one exec
shim per case, so they require both native and compiler resources. A campaign
over two reviewed Kimi versions is sixteen native cases, not the seventeen R2
and seven question-fixture controls. The latter use stand-ins and loopback
servers and establish fixture behavior only. `test_kimi_trust` and
`test_kimi_cache_hint` additionally execute hgs and private tmux fixtures; they
are synthetic integration tests, not pure controls. Historical native results
do not qualify a newly composed candidate.

The separate [pinned Kimi native workflow](.github/workflows/kimi-native.yml)
runs these sixteen cases on disposable standard Linux and macOS runners for
relevant PR changes or a manual run. It builds hgs once with Rust 1.99.1 and
two jobs, runs the existing pinned tmux in a private prefix, and checks both
official version manifests and all image sizes/SHA-256 pins before native
execution. It receives no provider or publication credentials, uses no cache,
and never installs Kimi globally. Linux x64 and Darwin arm64 are the only
reviewed images; an unexpected runner architecture fails before the build.
Every case keeps its existing image/environment/custody assertions. A timeout
or uncertain cleanup fails, retains the recorded private roots and images as
bounded diagnostics, and never claims that stopping the test leader stopped
its detached agents. Ephemeral VM disposal is the final boundary. Public model
endpoints are not configured; the fixtures' model and platform are loopback
only. This is not a kernel network enclosure or Windows Kimi qualification.
The historical NP50 owner-Mac campaigns are separate and are not replayed.
`scripts/fetch-conpty.py` downloads the ConPTY package pinned in
`packaging/windows/conpty.json` from nuget.org and writes only its checked
`conpty.dll` and `OpenConsole.exe`; `package-windows.py` puts them next to
`hgs.exe` and fetches them itself without `--conpty`. On Windows,
`scripts/ci/gui.sh` re-enters itself in the Visual Studio x64 environment
(`scripts/ci/msvc-env.py`), builds with Ninja and `cl` against `QT_ROOT_DIR` and runs
every Qt suite offscreen. The Qt fixtures are Python scripts that a small native
shim (`zerus-test-helper`) starts, because Windows cannot run scripts directly.
Outside a console, the shim hands each call to a worker of the test program's pool
(`tray/tests/support/fixture-worker.py`), which keeps one warm interpreter and ends
a fixture whose shim is killed; `ZERUS_FIXTURE_POOL=0` starts a Python process per
call instead.
`package-windows.py` finds the app-local Visual C++ runtime with `vswhere`, and
`check-windows-portable.py --run --selftest` starts both executables from an
extracted copy with a minimal environment and enables local sessions in its
disposable profile, which must stage the pinned ConPTY pair. Never start the packaged app with
`QT_QPA_PLATFORM=offscreen`: the zip has no offscreen plugin, and Qt's error box
then hangs the process out of sight.

Windows OpenSSH (`ssh.exe`) never exits after it has used a synchronous pipe for
one of its standard streams, the kind Rust's standard library, Python, PowerShell
and the GitHub Actions runner give a child. It also never ends a remote command's
input read from `NUL` or a non-empty file, and writes an output file from its
start, over what `>>` keeps. Tests and scripts therefore reach `ssh.exe` only
through `hgs.exe`, which gives it a console or overlapped pipes for every stream
that carries data. The fake client refuses a synchronous pipe, `NUL` or a
non-empty file as input (except for `-G`) and an output file that already holds
data (exit 254), so a regression fails the tests instead of hanging them. Git Bash
rewrites arguments that look like POSIX paths when it starts a Windows program: set
`MSYS_NO_PATHCONV=1` before passing remote paths such as `/workspace/orbit` to
`hgs.exe`. A check against a real peer is described in
[the real-peer smoke](docs/ci-and-aur.md#real-peer-smoke-on-a-windows-pc).
`tray/tools/export-windows-icon.py` (Pillow) regenerates the checked-in Windows icon
after artwork changes.

`test_windows_mobile_gateway.py` needs Git for Windows' `openssl` on PATH and
runs outside a job that forbids breakaway, as do the other session-host suites.
Before that fixture, the two `windows_management::tests` library cases exercise
actual management-lock contention and expiry, and the six `windows_gateway`
integration cases exercise private configuration, revision admission, ACLs and
independent ownership of temporary fixture roots.
They must execute on Windows; Linux excludes them and is not runtime evidence.
It creates only a throwaway profile, tracked fake Codex session, copied gateway
bundle and HTTPS loopback protocol fixture. It checks actual connector Start,
duplicate Start, published capabilities, Terminal snapshot/input and duplicate
receipts, phone-list/invitation APIs, graceful Stop and the same surviving native
host/session. Startup custody is reserved before Start, including failed or
timed-out launches. Recovery verifies the private revision/generation/control
and unique copied process image, pins a Windows process handle and rechecks
control. Cleanup requires confirmed handle exit; an unknown final wait retains
the private root and fails the gate. Forced termination is limited to that owned
handle and also fails the graceful-stop gate. The separate `--custody-only`
launcher forbids executable-pattern cleanup and retains unknown timeout state;
there is no PID/tree fallback for the connector. It does not install
into an owner profile, contact a production relay or pair a real phone. Run the
production relay's existing contract suites separately. A skipped or unavailable
Windows suite is not evidence of supported runtime behavior.
