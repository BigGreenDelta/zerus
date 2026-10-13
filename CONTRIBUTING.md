# Contributing

Build requirements are documented in `docs/installation.md`. Keep fixtures synthetic and
machine connection settings local. `AGENTS.md` describes the Beads workflow.

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
headlessly in parallel (`build` or `test` runs only that half);
`test_sessionswindow` runs as ten shards of its test functions. Tray sources
compile once, into the `hgs-tray-*` static libraries that the application and
the suites link: a new suite links the lowest library that covers its code
instead of listing `tray/src` files, which fails configuration.
`ZERUS_TEST_JOBS` sets both test parallelisms (defaults: at most 16 Python and
8 Qt jobs, never more than the CPU count; `1` runs them one after another). Test
logs and Python job durations live in ignored `artifacts/test-results/`. Set
`ZERUS_BUILD_JOBS` to limit desktop build parallelism. Run
`cargo +1.85.0 test --locked` for the minimum supported Rust.

On Arch, `bash scripts/ci/arch.sh` also builds and validates the pacman package
as an unprivileged user. It substitutes a local Git source only in its disposable
build directory; it never installs the package or enables services.
Hosted CI runs full Linux CLI, Arch desktop/package, macOS and Windows checks
automatically on pushes to main and PRs targeting main. Manual runs default to full
coverage; explicit suite inputs can narrow a manual/reusable run. Validate workflow
edits locally before pushing. Daily nightlies publish a verified binary to GitHub and
AUR, and the Windows portable zip to GitHub, only after every check passes; stable
releases remain manually published.
See [CI and Arch publication](docs/ci-and-aur.md) for triggers and publication
requirements.

Native integration tests may need separately installed agent CLIs or a real desktop;
read each suite's setup before running it. Do not run tests against live agent state.
Some platform tests only exist on macOS, Linux or Windows. Record any unavailable
checks.

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

For the Rust mobile relay, run `cargo +1.85.0 test --locked` inside
`services/mobile/relay`, then the explicit PostgreSQL and connector contracts
in [its README](services/mobile/relay/README.md). The Docker image uses that Rust
binary. For the connector and reference compatibility fixtures, install
`services/mobile` in a Python 3.11+
virtual environment and run `python -m unittest discover -s services/mobile/tests -v`.
These tests use synthetic subprocesses and isolated databases, without live native
agents. For Android, run `./gradlew testDebugUnitTest lintDebug assembleDebug` from
`mobile/android` with JDK 17 or 21 and Android SDK 36. Follow
[mobile setup](docs/mobile-deployment.md) for device and notification checks.

Beads issues, history and memories must contain only information suitable for public
source collaboration. An ignored local database still becomes remote data when
`bd dolt push` is run. Review the exported issues and memory before synchronizing.

For release preparation, run Gitleaks on both the working tree and all Git history,
and review binary assets and dependency notices. The local privacy script catches
common accidental machine paths and keys; it is not a complete secret scanner.

<a id="windows"></a>

## Windows

Windows builds the [control console](docs/installation.md#windows): `hgs.exe`
without the local session host and `hgs-tray.exe` for remote machines. You need:

- Visual Studio 2022 with the x64 C++ tools and a Windows SDK; its own CMake
  (3.22 or later is needed for the Windows tests) and Ninja are used;
- Rust for MSVC: stable, and 1.85.0 with clippy;
- Python 3.9 or later for the Qt test fixtures, 3.11 or later for the packaging
  scripts, and 3.13 for the hash-locked Qt installer;
- Git for Windows with Git Bash;
- Qt 6.10.3 `msvc2022_64` with Qt WebEngine, Qt WebChannel, Qt Positioning and Qt Multimedia.
  CI installs it with `scripts/ci/aqt-requirements.txt` (aqtinstall 3.3.0 cannot
  install Qt 6.11 or later for Windows yet); the same works locally:

```sh
python -m venv ~/aqt
~/aqt/Scripts/python -m pip install --require-hashes --only-binary :all: \
    -r scripts/ci/aqt-requirements.txt
~/aqt/Scripts/python -m aqt install-qt windows desktop 6.10.3 win64_msvc2022_64 \
    -m qtwebengine qtwebchannel qtpositioning qtmultimedia --outputdir ~/Qt
```

Clone with `git clone -c core.autocrlf=false`, as CI does: test fixtures and
scripts must keep LF line endings. Then, from Git Bash in the checkout:

```sh
cargo test --locked </dev/null
cargo +1.85.0 clippy --locked --all-targets -- -D warnings
cargo build --release --locked
cargo build --locked --example fake_ssh
HGS_TEST_BIN="$(cygpath -w "$PWD/target/release/hgs.exe")" \
HGS_TEST_FAKE_SSH="$(cygpath -w "$PWD/target/debug/examples/fake_ssh.exe")" \
  python -m unittest discover -s tests -p test_windows_client.py -v </dev/null
export QT_ROOT_DIR="$(cygpath -w ~/Qt/6.10.3/msvc2022_64)"
bash scripts/ci/gui.sh </dev/null
python scripts/package-windows.py --cli target/release/hgs.exe \
    --gui .ci-build/gui/hgs-tray.exe --output artifacts/zerus-windows-x86_64.zip
python scripts/check-windows-portable.py artifacts/zerus-windows-x86_64.zip --run --selftest
```

`tests/test_windows_client.py` runs `hgs.exe` against `examples/fake_ssh.rs`,
injected through `HGS_SSH`, with `HOME` and `USERPROFILE` in a temporary folder;
it never uses your SSH configuration and skips itself on other systems. On Windows,
`scripts/ci/gui.sh` re-enters itself in the Visual Studio x64 environment
(`scripts/ci/msvc-env.py`), builds with Ninja and `cl` against `QT_ROOT_DIR` and runs
every Qt suite offscreen. The Qt fixtures are Python scripts that a small native
shim (`zerus-test-helper`) starts, because Windows cannot run scripts directly.
`package-windows.py` finds the app-local Visual C++ runtime with `vswhere`, and
`check-windows-portable.py --run --selftest` starts both executables from an
extracted copy with a minimal environment. Never start the packaged app with
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
