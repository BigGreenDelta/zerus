# Homebrew Cask Distribution Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Publish a signed, notarized, self-contained `Zerus.app` for Apple Silicon through the `ufna/homebrew-zerus` cask, produced and published by the existing manual release candidate and publication workflows.

**Architecture:** A secret-free macOS job builds and checks the bundle on a pinned official Qt; a separate main-only `macos-signing` job signs, notarizes and staples it; the existing Arch assembly job seals the zip and rendered cask into the same candidate; `publish-release.py` verifies the seal and pushes the cask to the tap after GitHub and AUR. A new `zerus-setup` for macOS performs the per-user steps that a cask must not perform.

**Tech Stack:** Python 3 (3.9-compatible for user-side scripts), Qt 6.11.2 / C++17 / QtTest, Bash, GitHub Actions, Homebrew cask DSL, `codesign`/`notarytool`/`stapler`.

**Spec:** `docs/superpowers/specs/2026-10-10-homebrew-cask-design.md`

## Global Constraints

- Apple Silicon only: every Mach-O slice in the bundle is `arm64`; cask `depends_on arch: :arm64`.
- Qt **6.11.2** official `clang_64` with `qtwebengine`, `qtwebchannel`, `qtpositioning`; `LSMinimumSystemVersion` **13.0**; cask `depends_on macos: ">= :ventura"`.
- Bundle ID stays `com.hgdev.hgs-tray`; GUI executable stays `hgs-tray`; `QCoreApplication` organization/application names unchanged.
- Asset name `zerus-<version>-macos-arm64.zip`; cask URL `https://github.com/ufna/zerus/releases/download/v#{version}/zerus-#{version}-macos-arm64.zip`.
- No cask `postflight`, `uninstall quit:`, `uninstall launchctl:` or `auto_updates`.
- Never start, stop or restart native agents, tmux servers, DeepSeek hosts or a running GUI from install, upgrade or setup; setup never passes `--restart`.
- User-side Python (`zerus-setup`, LaunchAgent installers) must run on `/usr/bin/python3` 3.9.
- Secrets: `macos-signing` holds `MACOS_DEVELOPER_ID_P12`, `MACOS_DEVELOPER_ID_P12_PASSWORD`, `APPLE_NOTARY_API_KEY_P8`, `APPLE_NOTARY_API_KEY_ID`, `APPLE_NOTARY_API_ISSUER_ID` and variable `MACOS_TEAM_ID`; `release` holds `HOMEBREW_TAP_DEPLOY_KEY`.
- No commit, push or hosted workflow dispatch without explicit owner authorization (repository policy); work stays in the `homebrew-cask` worktree.

## Review Focus

- A stable release containing the macOS zip must not break the website mirror or existing GUIs: `update_feed.py` must accept the optional zip, and GUI `parseFeed` must keep ignoring it (Task 9).
- A Finder-launched GUI has no Homebrew `PATH`; it must still find the bundled `hgs` (Task 2).
- `zerus-setup` run over an existing source installation must change nothing and say why (Task 1).
- A candidate without the macOS asset (owner opted out with `macos_cask=false`) must still publish Arch-only exactly as today and skip the tap (Task 8).
- Publishing an older or same-version-different-bytes cask must be refused, and a retry after a failed tap push must be a no-op for already-published parts (Task 8).

---

### Task 1: macOS `zerus-setup`

**Files:**
- Create: `packaging/macos/zerus-setup`
- Modify: `scripts/install-macos-session-service.py`, `scripts/install-recovery-service.py`, `scripts/install-swarm-service.py` (only if Python 3.9 incompatibility or a hard-coded `launchctl` path blocks testing)
- Test: `tests/test_macos_setup.py`

**Interfaces:**
- Produces: executable `zerus-setup [--app PATH] [--autostart] [--workers] [--remove]`. Default `--app` is the bundle containing the script (`<app>/Contents/Resources/bin/zerus-setup`). Exit 0 on success, 2 on usage/conflict errors with the conflicting path in the message.
- Consumes: bundle layout from Task 6 — `Contents/MacOS/hgs`, `Contents/Resources/setup/{hgs_state.py,install-macos-session-service.py,install-recovery-service.py,install-swarm-service.py,com.hgdev.hgs-tray.plist}`.

- [ ] **Step 1: Write failing tests** with a synthetic home, a fake bundle under a temporary directory and a fake `launchctl` first on `PATH` that appends its arguments to a log file. Cases: default creates both links and the session bridge plist, second run is a no-op, a regular file at `~/.local/bin/hgs` aborts before any change, a symlink pointing elsewhere is preserved and aborts, `--autostart` writes the tray plist with the bundle paths and does not load it, `--workers` installs both worker plists, `--remove` removes only links and plists pointing into the bundle, root refusal (`os.geteuid` patched via environment `ZERUS_SETUP_TEST_UID`), detection of `~/Applications/hgs-tray.app` reports migration and changes nothing, no installer receives `--restart`.
- [ ] **Step 2: Run** `python3 -m unittest tests.test_macos_setup -v` and confirm failures.
- [ ] **Step 3: Implement** following `packaging/linux/zerus-setup`: compute all planned actions, check all conflicts, then apply. Call the bundled installers with `sys.executable` and the bundle `hgs` path; render the tray plist with `__APP__`, `__HGS__`, `__LOG__` replacements.
- [ ] **Step 4: Run** the tests under `/usr/bin/python3` and the default `python3`; both pass.

### Task 2: GUI uses the bundled `hgs`

**Files:**
- Modify: `tray/src/AppConfig.h`, `tray/src/AppConfig.cpp`, `tray/src/main.cpp`
- Test: `tray/tests/test_appconfig.cpp`

**Interfaces:**
- Produces: `static QString AppConfig::bundledHgsPath(const QString &executable, Platform p);` returns the absolute path of an executable regular file `hgs` next to `executable` when `p == Platform::Mac` and the executable lives in `*.app/Contents/MacOS`; otherwise an empty string.

- [ ] **Step 1: Failing tests**: bundle layout with executable `hgs` returns it; non-executable or missing `hgs` returns empty; non-bundle directory returns empty; `Platform::Linux` returns empty.
- [ ] **Step 2: Implement** and in `main.cpp` after `AppConfig::load` succeeds: `if (cfg.hgsPath.isEmpty()) cfg.hgsPath = AppConfig::bundledHgsPath(QFileInfo(args.value(0)).absoluteFilePath(), AppConfig::currentPlatform());` before `--selftest`.
- [ ] **Step 3: Run** `ctest -R appconfig` in a local tray build; pass.

### Task 3: Homebrew ownership and Restart to finish update

**Files:**
- Modify: `tray/src/UpdateController.h`, `tray/src/UpdateController.cpp`, `tray/src/TrayMenu.cpp`, `tray/src/SessionsWindow.cpp`, `tray/src/UpdatesWidget.h` (if it renders the message)
- Test: `tray/tests/test_updates.cpp`

**Interfaces:**
- Produces: `DesktopInstallation::kind == "homebrew-cask"` with `package == "zerus"`; `static QString UpdateController::homebrewCaskVersion(const QString &canonicalExecutable, const QStringList &prefixes);` returns the installed cask version when the executable is `<...>/Zerus.app/Contents/MacOS/hgs-tray` and `<prefix>/Caskroom/zerus/<version>` exists for one prefix (default prefixes `/opt/homebrew`, `/usr/local`); `updateCommand()` returns `brew upgrade --cask zerus` for that kind; `static QString UpdateController::bundleShortVersion(const QString &infoPlistPath);` parses `CFBundleShortVersionString` from an XML plist; `bool restartRequired() const;` true when the on-disk bundle version is valid and differs from `QCoreApplication::applicationVersion()`; `void restartIntoInstalledBundle();` (macOS) starts a detached `/bin/sh` that waits for this PID to exit and then runs `/usr/bin/open -a <bundle> --args --sessions`, then quits the application.
- Consumes: existing `changed()` signal; periodic 15-minute timer.

- [ ] **Step 1: Failing tests** for `homebrewCaskVersion` (match, no Caskroom, wrong bundle name, two prefixes), `bundleShortVersion` (valid, missing key, malformed XML), `updateCommand` for the cask kind and `comparisonMessage` on macOS for the cask kind (newer and not newer).
- [ ] **Step 2: Implement** detection in `detectInstallation` under `Q_OS_MACOS` before `sourceInstallation`; extend `comparisonMessage` for the cask kind using `plainStableIsNewer`; recompute `restartRequired` on the periodic timer and after each check, emitting `changed()` only on transitions.
- [ ] **Step 3: UI**: tray menu entry `restartToUpdate` "Restart to finish update" and a status-bar button with the same object name in the Sessions window, both visible only when `restartRequired()`; trigger `restartIntoInstalledBundle()`.
- [ ] **Step 4: Run** `ctest -R "updates|traymenu|sessionswindow"`; pass.

### Task 4: Cask template and candidate schema 2

**Files:**
- Create: `packaging/homebrew/zerus.rb.in`, `scripts/macos_release.py`
- Modify: `scripts/release_common.py`
- Test: `tests/test_release_publication.py`

**Interfaces:**
- Produces in `macos_release.py`: `MACOS_ASSET = "zerus-{version}-macos-arm64.zip"`; `render_cask(version: str, digest: str) -> str`; `cask_fields(text: str) -> dict` returning `version` and `sha256` parsed from a cask; `require_template_cask(text: str, version: str, digest: str)` raising `ValueError` unless `text == render_cask(version, digest)`; `cask_version_key(version: str) -> tuple`.
- Produces in `release_common.py`: `finish_candidate()` also seals `*.zip`; `validate_candidate()` accepts schema 1 (no macOS) and schema 2 with `info["macos"] == {"asset", "qt_version", "team_id", "notarization_id", "minimum_system_version"}` and `info["homebrew_casks"] == ["zerus"]`, requiring the zip in `SHA256SUMS` and `homebrew/zerus.rb` equal to the rendered template; returns `info, sums` unchanged in shape.

- [ ] **Step 1: Failing tests**: render/parse round trip; extra or changed lines rejected; schema 2 candidate fixture validates; zip missing from sums rejected; altered cask rejected; schema 1 fixture still validates.
- [ ] **Step 2: Implement** and run `python3 -m unittest tests.test_release_publication -v`.

### Task 5: Pinned Qt installation

**Files:**
- Create: `packaging/macos/qt.json`, `scripts/install-qt-macos.py`
- Test: `tests/test_macos_qt.py`

**Interfaces:**
- Produces: `install-qt-macos.py --pin packaging/macos/qt.json --output DIR` installs Qt into `DIR`; `--lock` regenerates the archive list and SHA-256 values from the Qt repository metadata. Every archive is downloaded from `https://download.qt.io/online/qtsdkrepository/` with a recorded size and SHA-256 and verified before extraction. Extraction uses the system `bsdtar` after every archive is verified.

- [ ] **Step 1: Spike locally**: lock, download, verify, extract into a scratch prefix; build the tray against it with `CMAKE_PREFIX_PATH`; record which extractor works and whether Qt needs path patching.
- [ ] **Step 2: Unit tests** for pin parsing, hash mismatch rejection, unexpected archive rejection and path traversal rejection on synthetic archives.
- [ ] **Step 3: Implement** and run the tests.

### Task 6: Bundle assembly and checks

**Files:**
- Create: `scripts/package-macos.py`, `scripts/check-macos-bundle.py`
- Modify: `THIRD_PARTY_NOTICES.md`
- Test: `tests/test_macos_bundle.py`

**Interfaces:**
- Produces: `package-macos.py --qt QTDIR --cli target/release/hgs --app tray/build-release/hgs-tray.app --version X --output DIR` creates `DIR/Zerus.app`; `check-macos-bundle.py DIR/Zerus.app --version X [--signed]` exits non-zero with a list of findings. Pure functions `load_commands(otool_output) -> list[str]`, `foreign_references(paths, bundle) -> list[str]` are unit tested.

- [ ] **Step 1: Failing tests** for the pure checker functions with recorded `otool -l` excerpts (allowed `@rpath`/`@executable_path`/`/System`/`/usr/lib`; rejected `/opt/homebrew`, Qt install paths, absolute user paths), plist checks and required notices.
- [ ] **Step 2: Implement**, then locally build an unsigned bundle with the Task 5 Qt and run the checker; launch `--selftest`.

### Task 7: Signing and notarization script

**Files:**
- Create: `scripts/sign-macos.py`, `packaging/macos/QtWebEngineProcess.entitlements`
- Test: `tests/test_macos_signing.py`

**Interfaces:**
- Produces: `sign-macos.py --app DIR/Zerus.app --identity "Developer ID Application: ..." --team-id ID --output DIR [--notary-key-file P8 --notary-key-id ID --notary-issuer ID]`; without notary arguments it signs only and refuses to produce a release zip. `signing_order(paths) -> list[Path]` (inside-out) is unit tested. Writes `notarization.json` with the submission ID.

- [ ] **Step 1: Failing test** for `signing_order` and for refusing a zip without notarization.
- [ ] **Step 2: Implement**; locally verify with ad-hoc identity `-` that `codesign --verify --deep --strict` passes and that a WebEngine view still renders with only `allow-jit`.

### Task 8: Workflows and tap publication

**Files:**
- Modify: `.github/workflows/release.yml`, `.github/workflows/publish.yml`, `scripts/publish-release.py`, `scripts/fetch-release-candidate.py` (size limit), `tests/test_ci_security.py` or the existing workflow contract tests
- Create: `packaging/homebrew/known_hosts`
- Test: `tests/test_release_publication.py`

**Interfaces:**
- `release.yml`: input `macos_cask` (boolean, default true; requires `run_macos`); jobs `macos-bundle` (no secrets) and `macos-sign` (`environment: macos-signing`); `assemble` downloads the signed artifact into `dist/` and renders `dist/homebrew/zerus.rb` before `check-release-packages.sh` seals.
- `publish-release.py`: `--tap-ssh-key PATH`, `--tap-known-hosts packaging/homebrew/known_hosts`; `prepare_tap(candidate, workspace, env) -> plan | None` clones, refuses downgrade or same-version-different-bytes, commits and `push --dry-run` before any GitHub write; the push happens after AUR pushes.

- [ ] **Step 1: Failing contract tests**: tap plan with no existing cask, identical cask no-op, downgrade refused, same version with different bytes refused, candidate without macOS skips tap, missing tap key with a macOS candidate fails before GitHub writes.
- [ ] **Step 2: Implement** and run the unit tests plus `actionlint` on both workflows.

### Task 9: Website mirror accepts the macOS asset

**Files:**
- Modify: `scripts/update_feed.py`
- Test: `tests/test_update_feed.py`

- [ ] **Step 1: Failing test**: a stable release fixture with schema 2 metadata and the zip produces a valid feed whose artifacts exclude the zip; an unexpected extra zip name is still rejected.
- [ ] **Step 2: Implement** and run the feed tests; record in the documentation that the mirror must be updated before the first release with a macOS asset.

### Task 10: Documentation and instruction files

**Files:**
- Modify: `docs/installation.md`, `docs/ci-and-aur.md`, `docs/public-distribution.md`, `README.md`, `AGENTS.md`, `CLAUDE.md`
- Create: `docs/macos-distribution.md`

- [ ] **Step 1:** Write the macOS install, setup, migration, upgrade and uninstall sections; the publication runbook with secrets, retry rules and mirror ordering; mirror the Homebrew distribution line in `AGENTS.md` and `CLAUDE.md`.
- [ ] **Step 2:** Run `python3 scripts/ci/check-source.py` and the privacy guard.

## Execution

Native execution in this session in the order above; the owner asked to implement directly after approving the spec. Hosted runs, commits and pushes wait for explicit authorization. Signing with a real Developer ID and notarization (Task 7 live path) wait for `zerus-6a2u.2`.
