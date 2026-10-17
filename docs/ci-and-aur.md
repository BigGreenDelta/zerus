# CI and Arch Linux publication

This document describes the checks, package layout and publication workflow.
[Zerus 0.37.0](https://github.com/ufna/zerus/releases/tag/v0.37.0) is the first
public Arch x86_64 bundle. Stable release builds and GitHub/AUR publication remain
separate manual workflows; verified nightlies publish automatically. Stable
releases after 0.42.2 and nightlies also carry a Windows x86_64 portable zip of
the control console.

## Continuous integration

Hosted checks run automatically on pushes to `main` and pull requests targeting
`main`, including Dependabot proposals. External fork workflows wait for a
maintainer's **Approve and run** decision. Every automatic run selects source/MSRV,
full Linux CLI, Arch desktop/package, both macOS suites and both Windows suites
(client and desktop, and real agents through the session host). The owner enabled
this policy on 2026-10-09 after making the repository public: standard Ubuntu,
macOS and Windows GitHub-hosted runners are free for public repositories.

Manual CI remains available and defaults to all suites. `ci.yml` chooses the cache
trust boundary and calls the shared jobs in `checks.yml`. Reusable CI keeps explicit
suite selection so another workflow cannot accidentally request unrelated jobs.
A short first job resolves the exact commit and the selected suites; every other
job checks out that commit. PR checks use the GitHub merge commit; a manual `ref`
can select a PR head or another exact revision.

| Job | Automatic CI | Checks |
| --- | --- | --- |
| Resolve commit | Always | Exact test commit and suite selection for every other job |
| Quick checks | Always | Privacy, Python/Bash syntax, version consistency, actionlint, stable/nightly publication contracts, locked Rust 1.99 unit tests |
| Rust relay | Always | Rust 1.99 format, clippy, and every storage contract (cross-worker ones included) on its own PostgreSQL database; every `services/mobile/tests` module, with the Rust relay fixtures on per-test PostgreSQL databases and no skips; the speech suites against the Python reference relay; the deployment image and its 256 MiB envelope |
| Rust mobile connector | Always | Rust 1.99 format, clippy, vector, binary and journal handoff tests; the black-box suites against the Python and Rust connectors concurrently with differential traces compared against the reviewed allowlist, and a 300 s soak of the Rust build (`services/mobile/tests/BLACKBOX.md`) |
| Rust mobile connector on macOS | Always | Cold Rust 1.99 build, unit, vector and binary tests, and the black-box modules that need no real `hgs`, journal handoffs with the Python connector included |
| iOS core tests and simulator build | Always | `swift test` for the ZerusKit core and an unsigned Release build of the app for the iOS simulator, with the newest installed Xcode 26 or later and no signing identity |
| Android unit tests and lint | Always | `testDebugUnitTest` and `lintRelease` of the Firebase variant that Dev ships, with a synthetic `google-services.json` and no credentials |
| Linux CLI | Always | Stable Rust unit tests, terminal smoke tests, Python integration modules, release build |
| Arch desktop | Always | Qt build and all CTest suites |
| Arch package | Always | Production package with the recipe's `check()`, namcap, staged and installed bundle checks; the only producer of the `arch-x86_64-<commit>` candidate artifact |
| macOS CLI and desktop | Always | Native CLI integration and all Qt CTest suites on two standard macOS jobs |
| Windows client and desktop | Always | Rust tests, Rust 1.99 clippy for the Windows target, Windows client and session host integration tests, MSVC Qt build with all offscreen CTest suites, and the portable zip checked from an isolated extraction |
| Windows real agents | Always | The pinned `win32-x64` Claude Code and Codex in isolated session hosts of the release build, each against a local fake API; skipped or empty suites fail |
| CI gate | Always | Requires every selected suite to succeed; fails on unexpected skips, cancellation and errors |

Quick checks and the full suites start together once the commit is resolved, so
a quick-check failure no longer holds back the other suites. New CI runs cancel
obsolete runs for the same branch/PR and workflow. PR jobs have read-only
permissions and no publication credentials; fork PRs use `pull_request`, never
`pull_request_target`. Publishing is confined to trusted upstream `main` and the
branch-restricted `release` environment. Authenticated model-turn tests remain
explicit opt-ins.

The Android job runs in every CI run, including PR checks before merge, and needs
no credential. It builds with `-PzerusFirebase=true`, like the signed Dev APK,
from a `google-services.json` written in the job with placeholder values (project
number zero, package `app.zerus.mobile`). The google-services and Crashlytics
plugins only turn that file into resources; tests and lint never contact
Firebase, and the mapping upload follows R8, which these tasks do not run.

On upstream `main` pushes with the [Dev channel](public-distribution.md#verified-main-dev-channel)
enabled, `dev-android.yml` starts with the checks instead of after them. In the
`release` environment and without cache access, it only assembles the signed
release APK (`assembleRelease`), seals it and deletes the signing inputs. It
then polls this run's jobs with a read-only Actions token and uploads
`zerus-dev-android-a<attempt>` only after the `CI gate` job succeeds. While this
attempt has no gate record, an earlier successful gate counts (a failed-job retry
does not rerun it); an earlier failed or cancelled gate means waiting. Any
other gate result, repeated API failures or a 135-minute wait fail the job without
an upload; **Re-run failed jobs** then rebuilds the candidate. `dev.yml` assembles
the desktop candidate and moves the `dev` tag only after the gate, this job
and the enabled Mac candidate succeed. Main CI invokes `macos-cask.yml` after
the gate with `channel: dev`; its full bundle/sign/notarize/install checks produce
the same signed ZIP and DMG later sealed into the desktop candidate. Release builds skip the `lintVital` tasks: the gate requires the
Android job, whose `lintRelease` covers those fatal checks for the same commit
and variant.

### Incoming PRs and publication trust

Running a PR compiles and executes code supplied by its author, including Rust
build scripts, tests and packaging commands. GitHub-hosted runners provide an
ephemeral environment; passing CI does not establish that the proposed code is
safe to merge.

Repository settings require approval for **all external contributors** and full
commit SHA pins for actions. The workflow token defaults to read-only and cannot
approve PRs. Review the complete diff before approving a fork workflow, especially
workflow changes, dependency/build scripts and attempts to change cache permissions
or runner labels. A previous harmless contribution does not grant automatic runs.
Keep development machines off the runner list; secrets, SSH credentials, account
configuration and authenticated model sessions do not belong in CI caches/artifacts.

PR checks and every manual CI run receive **read-only cache tokens**, including a
manual run of `refs/pull/NUMBER/head` from the upstream workflow. Only upstream
`main` pushes and scheduled checks of that exact `main` commit can populate caches.
This restriction is enforced by GitHub's fixed `cache-mode` on the reusable call,
so checked-out build code cannot bypass it by invoking the cache API itself.
Publishers use `cache-mode: none` and execute trusted upstream publication scripts.
They accept only successful main-only stable/Nightly candidates, check workflow
identity, repository, commit, run identity, archive safety and digests, and never
execute downloaded AUR recipes. Ordinary PR CI artifacts cannot become releases.
The `release` environment permits the `main` **branch** only, with no tag allowance.

The current actionlint 1.7.12 release predates `cache-mode`. Source checks validate
its fixed enum and placement; actionlint suppresses only its unknown-key warning
for that field. Trust-boundary tests exercise the actual routing conditions and
failure gate. GitHub validates the cache policy and reusable-workflow limits. Remove
the narrow exception when a released actionlint supports the key.

These boundaries limit credential theft, repository writes and compute abuse.
Maintainer approval still matters: a PR can change its workflow, and granting a
write-capable cache mode to untrusted code would bypass the secure cache default.
See [fork workflow approvals](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/approve-runs-from-forks),
[Actions security](https://docs.github.com/en/actions/reference/security/secure-use)
and [cache access limits](https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching).

### Build caches

`ci.yml` passes `save_caches: true` only to the trusted main checks, the one
caller with a write-capable cache token. `checks.yml` uses explicit
`actions/cache/restore` and `actions/cache/save` steps, and every save requires
that input: PR, manual, nightly and release runs restore caches and never try to
save them.

| Family | Job | Key after the prefix | Saved when | Bound |
| --- | --- | --- | --- | --- |
| `msrv-`, `cli-`, `windows-cli-` | Quick checks, CLI, Windows | OS/arch, hash of `rustc -Vv`, month, `Cargo.lock` hash | Exact key missed; Rust build passed | Monthly restart; about 200–250 MB each |
| `connector-` | Rust mobile connector | OS/arch, hash of `rustc +1.99.1 -Vv`, month, `services/mobile/connector/Cargo.lock` hash | Exact key missed; connector tests passed | Monthly restart; about 400 MB before compression, without incremental state |
| `arch-cargo-v1-` | Arch package | Month, hash of `Cargo.lock` and the connector's lock | Exact key missed; package built | Cargo registry only |
| `android-gradle-v1-` | Android | Month, hash of the Gradle wrapper properties and `*.gradle.kts` | Exact key missed; tests and lint passed | Monthly restart; about 700 MB (`~/.gradle/caches` and `wrapper`) |
| `windows-qt-6.10.3-` | Windows | Hash of the Qt pin and installer | Exact key missed; desktop build passed | The pinned Qt archives, no WebEngine |
| `arch-ccache-v1-`, `arch-package-ccache-v1-`, `mac-desktop-ccache-v1-`, `windows-ccache-v1-` | Arch desktop, Arch package, macOS, Windows | Architecture (macOS, Windows), Qt release (Windows), run ID | Build passed with ccache misses | `CCACHE_MAXSIZE` 150, 100, 100 and 500 MB |

The Windows real-agent job restores the `windows-cli-` family of the Windows
desktop job for its release build and never saves it, so two jobs cannot race to
save one key.

When `Cargo.lock` changes, a Rust cache restores the newest target directory of
the same compiler and month. A new compiler or month starts empty, so stale
dependency builds cannot accumulate. The Gradle cache does the same when the
wrapper or a build script changes. The signed Dev build has no cache access and
downloads its dependencies afresh. Compiler caches use one key per saving run
and restore the newest entry by prefix, so each main build that compiles
something refreshes them. Statistics are reset before and printed after each
build. Before saving, `ccache --evict-older-than` drops every entry the build did
not use (ccache updates an entry's time on each hit). Every new commit
recompiles the update controller, which embeds the source commit, so main pushes
normally save new ccache entries; GitHub evicts the least recently used entries
above the 10 GiB limit. Desktop builds use Ninja and every runner vCPU (`ZERUS_BUILD_JOBS`
4 on Arch and Windows, 3 on macOS), and the AUR recipes let Ninja choose the job
count.

The owner decided on 2026-10-13 that objects compiled by trusted main checks may
feed published builds: the Arch package job produces the Dev, nightly and stable
Arch candidates, and the Windows job the portable zip of stable and nightly
releases. Cache directories stay outside the checkout
(`/home/user/.cache/ccache`, `~/Library/Caches/ccache` and `RUNNER_TEMP`), so
they never enter packages, the zip or uploaded artifacts.
The Arch package job enables ccache for makepkg only through
`/etc/makepkg.conf.d/ccache.conf` in its disposable container, after checking that
the image's default `BUILDENV` differs only in `ccache`. The published recipes and
their `check()` are unchanged. The package build uses different compiler flags
from the desktop build, so it has its own cache family.

Windows installs the official ccache 4.14.1 x86_64 release. The job pins its
SHA-256, which matches the archive signed with the ccache minisign key
`RWQX7yXbBedVfI4PNx6FLdFXu9GHUFsr28s4BVGxm4BeybtnX3P06saF`. ccache officially
supports MSVC since 4.13. Release builds have no `/Zi`, which ccache cannot
cache; a build with debug information would need `/Z7` instead. Depend mode
(`CCACHE_DEPEND`) takes the includes from Ninja's `/showIncludes` instead of
running the preprocessor again. The job sets no `CCACHE_BASEDIR`: rewriting
paths to relative ones would also change `__FILE__`, from which suites locate
their fixtures, and the runner's workspace path is the same on every run.

### Manual checks

```sh
# Full Linux, Arch, macOS and Windows coverage:
gh workflow run ci.yml --ref main
# Full coverage of a PR head with the current upstream workflow:
gh workflow run ci.yml --ref main -f ref=refs/pull/123/head
# Source/MSRV checks plus the aggregate gate only:
gh workflow run ci.yml --ref main -f run_linux=false -f run_arch=false -f run_macos=false -f run_windows=false
```

An explicit test `ref` changes the checked-out commit, not GitHub's workflow run
metadata. Use the exact tested SHA recorded in the job summary when reviewing
such a manual run. Automatic PR and push runs produce CI gate without a dispatch.
Review and merging remain deliberate; successful CI does not auto-merge PRs.

Arch is the Linux GUI build environment because the application uses Qt 6 and KDE
Frameworks 6. Ubuntu remains useful for portable CLI and minimum-Rust checks.
Linux CLI CI builds the pinned official tmux 3.7c archive with an exact SHA-256;
Ubuntu 24.04's tmux 3.4 lacks `bracket_paste_flag`, introduced in tmux 3.7. That
format is required by the existing verified agent input transport. Unknown input
mode must keep rejecting delivery; upgrading the test toolchain preserves that
guard. The package requires tmux 3.7+. A binary upgrade must not restart live
servers; see the [installation requirements](installation.md#requirements).
Release packaging covers **Arch x86_64** and, through the `zerus`,
`zerus@nightly` and `zerus@dev` Homebrew casks, **Apple Silicon macOS**; see
[macOS distribution](macos-distribution.md). Ordinary macOS CI checks build/test
compatibility with Homebrew Qt; stable, nightly and successful main Dev candidates build, sign and
notarize the bundle, and only once the repository variable `MACOS_CASKS_ENABLED`
is `true`. Stable releases and nightlies add the Windows x86_64 portable zip
described below (owner decision of 2026-10-14; 0.42.2 is the last stable release
without it); Dev builds have none. Intel Macs and Arch ARM are outside this
package matrix.

All CI jobs have bounded timeouts. Checkout credentials are not persisted and
Actions are pinned to commit SHAs. Dependabot proposes monthly grouped action/Cargo
updates. Rust, C++ and Gradle caches reduce repeated build time; their repository
limit stays at 10 GiB. Ordinary test artifacts expire after seven days and stable
candidates after fourteen. Scheduled logs, Arch packages, Windows zips and nightly
candidates expire after three days.
Standard Ubuntu/macOS/Windows runners and public artifacts are free while the repository
is public. Larger runners and explicitly increased cache storage have separate
billing rules. No self-hosted runner is attached to a development machine.
[Actions billing](https://docs.github.com/en/billing/concepts/product-billing/github-actions).

After the trusted `main` checks, the `cache-trim` job keeps the two newest `main`
cache entries of each key family (the key without its last segment, which is a
run ID or a content hash) and deletes pull-request entries older than a day. It
logs each deleted key and size and the totals. The job has only `actions: write`
and `cache-mode: none`, checks out nothing and runs one inline `gh api` script.
An entry deleted concurrently is not an error, and a failed trim never fails the
run or holds back Dev. Its first `main` run confirms whether `cache-mode: none`
permits REST cache deletion; refused deletions appear in its log.

The Arch base image is digest-pinned, but `pacman -Syu` intentionally uses current
Arch packages. This tests rolling-distribution compatibility; it does not promise
bit-for-bit reproducibility months later. Rust dependencies are locked, fetched
in `prepare()` and built/tested with `--frozen` inside source packages. The CI
image, tmux, official DeepSeek fixture and Windows Qt pins need periodic
maintainer review.

### Dependency updates and cost

Dependabot checks version updates on the first day of each month. Each ecosystem
has one version-update group and at most one open version-update PR: two ordinary
PRs across GitHub Actions and Cargo. Automatic rebasing is disabled; refresh a
stale proposal before testing and merging it. Updates still require review and
checks appropriate to the active CI policy; there is no automatic merge.

Cargo proposals update only `Cargo.lock` within the reviewed `Cargo.toml`
constraints. This also avoids unreviewed breaking minor upgrades of `0.x` crates.
Manifest migrations are deliberate maintenance work, including raising the pinned
Rust to the newest stable release and existing state/configuration compatibility
checks. Actions keep their exact
commit pins and may propose major upgrades in their group for manual review.

Dependabot vulnerability alerts and security updates are enabled separately in
repository settings. Security proposals have their own per-ecosystem groups;
they are not delayed by the monthly version schedule or counted against its PR
limit. A fix requiring a manifest migration still needs maintainer attention.
Do not disable security alerts to reduce routine version-update noise.

Dependabot jobs on standard GitHub-hosted runners do not consume included Actions
minutes. Standard runner minutes are also free while this repository is public;
larger runners and storage have separate billing rules. There is no monthly
standard-runner minute quota to exhaust for a public repository. Schedule useful
coverage rather than artificial runs. Keep artifact retention bounded and use
standard runner labels; private-repository checks consume the account allowance.
See [Dependabot runner billing](https://docs.github.com/en/code-security/concepts/supply-chain-security/dependabot-on-actions),
[Actions billing](https://docs.github.com/en/billing/concepts/product-billing/github-actions)
and [Dependabot options](https://docs.github.com/en/code-security/reference/supply-chain-security/dependabot-options-reference).

### Test isolation and local commands

```sh
python3 scripts/ci/check-source.py
bash scripts/ci/cli.sh
bash scripts/ci/gui.sh
cargo +1.99.1 test --locked
# Arch only, as an ordinary user with the documented build dependencies:
bash scripts/ci/arch.sh
```

The CLI wrapper clears inherited `HGS_*` account/session selection before setting
its own `HGS_TEST_BIN`. Python integration modules run in separate processes so
module patches do not leak between suites. Terminal fixtures use private socket
paths and synthetic homes. No check runs against existing agent sessions.
Authenticated native-client tests remain explicit opt-ins; baseline CI needs no
model API keys. Linux installs the pinned official `@deepseek-ai/dsh@0.2.0-rc.2`
for its keyless local-model contract fixture. Native agent CLIs are not included
in the product package.

The CI CLI jobs set `ZERUS_PYTHON_JOBS` to twice the runner vCPUs (8 on Ubuntu,
6 on macOS): the Python test classes mostly wait on terminals and subprocesses.
The desktop jobs keep per-function QtTest JUnit results in `qt-functions/` of
their test-results artifacts. The `test_sessionswindow` shards are balanced by
`tray/tests/qt-shard-durations.txt`, the largest duration of each function, or
data row of its slowest functions, across the measured runs. After tests are
added or become slower, download the Arch, macOS and Windows desktop artifacts
of one main run and rebuild that file with `scripts/ci/qt-shard-seed.py`; its
usage lists the commands. A test the file does not list still runs exactly once.

`tests/test_claude_input_native.py` checks exact message acceptance by the real
Claude TUI with a private home/socket and a localhost fake API; it never reads a
user's account. Full Linux and macOS CI run it with the other Claude native
suites against the pinned Claude Code package (see below); `HGS_CLAUDE_TEST_BIN`
selects another executable for a manual run. Claude 2.1.296 coverage verifies
literal trailing backslashes, multiline messages and durable request replay.
Terminal submission alone is insufficient: the test waits for the unchanged user
message in native history. `tests/test_claude_settings_native.py` drives the
real `/model` picker with a synthetic model catalog and history. Claude 2.1.296
coverage changes model and effort in a fresh session, accepts Claude's own cache
notice in a resumed conversation, and requires the session-only transcript
receipt while `settings.json` stays unchanged. A session-only change made in
Terminal must reach hgs through `PostModelSwitch` with the exact model ID.
`tests/test_claude_statusline_native.py` starts the real TUI through a tracked
`hgs claude` launch, with the private home, state folder and socket and the
loopback API of `tests/model_api_fixture.py`. Claude 2.1.296 coverage requires the
user's own status line on screen and its `settings.json` unchanged, and the window
from Claude's status line input in `hgs inspect` for the exact run, conversation and
model: before the first reply, after a reply, after a session-only model change, a
compaction and a clear, and in a resumed run. In a linked worktree whose repository's
local settings set a status line, Claude keeps that status line and no window is reported.

### Unix tmux census

The Windows Phase 2 refactor moves every tmux call site behind a terminal backend
and must leave Unix behaviour byte-for-byte unchanged. `scripts/ci/tmux-census.py`
(Python 3, standard library only) records the tmux and `ps` calls that hgs makes
while the shell and Python suites run, and compares two recordings. It is a local
check, not a CI job. Record each tree with its own fresh debug build, on the same
machine, tmux version and `ZERUS_PYTHON_JOBS`, without `HGS_*`, `TMUX` or
`TMUX_PANE` in the environment:

```sh
census() {  # census OUT HGS: record both suites with that binary into OUT/
  HGS_TEST_BIN="$2" python3 scripts/ci/tmux-census.py record --out "$1/shell" -- \
    bash tests/test_hgs.sh </dev/null
  HGS_TEST_BIN="$2" ZERUS_PYTHON_JOBS=8 python3 scripts/ci/tmux-census.py record \
    --out "$1/python" -- python3 scripts/ci/python-tests.py </dev/null
  python3 scripts/ci/tmux-census.py summarize "$1/shell" "$1/python" --out "$1.json"
}
census "$scratch/main" "$main_build/debug/hgs"      # in a checkout of main
census "$scratch/branch" "$branch_build/debug/hgs"  # in the refactor checkout
python3 scripts/ci/tmux-census.py compare "$scratch/main.json" "$scratch/branch.json" \
  --rules scripts/ci/tmux-census-rules.json
```

Record both trees from the same kind of process: on Linux inside a user service
manager unit, a `new-session` that may start the server is preceded by
`list-sessions`, and without a server it reaches the shim through
`systemd-run --scope` (see
[Terminal, SSH and clipboard](reference.md#terminal-ssh-and-clipboard)).

`record` puts private `tmux` and `ps` shims first on `PATH`. `ZERUS_TMUX_CENSUS`
names the record directory, and `ZERUS_TMUX_CENSUS_TMUX` and `ZERUS_TMUX_CENSUS_PS`
name the real programs, resolved before the shims were added. Each call writes one
JSON record: argv, working directory, whether `TMUX` and `TMUX_TMPDIR` are set
(and whether that directory exists), `LC_ALL`, `LC_CTYPE`, `LANG` and `TZ`, what
stdin, stdout and stderr are connected to (terminal, pipe, `/dev/null`, file), the
nearest hgs ancestor process, the stdin bytes of `load-buffer ... -`, the exit
status and stderr. The shim then behaves like the real program, with the same
argv, environment, stdin, stdout, stderr and exit status; on Linux it undoes the
`LC_CTYPE` that Python's C-locale coercion would otherwise pass on.
`attach`, a `new-session` without `-d`, control mode and any call with a terminal
stdin exec tmux directly instead. No other call has its stdin read. A test's own
tmux wrapper reaches the shim through the path that the test resolved at import
(`tests/test_input.py:73`), so each call is recorded once, with the wrapper's
`-S` and `-f` kept apart from the command. Fakes that never run tmux, such as the
paste stubs in `tests/test_hgs.sh`, stay outside the census; those tests check
their own call logs.

`summarize` groups the calls into one operation per hgs process, in call order,
keyed by the test class and the normalized hgs argv. Temporary and home paths,
UUIDs, digests, PIDs, timestamps and generated names become placeholders numbered
within the operation, so equal values stay equal. tmux `%`/`$`/`@` IDs stay as
they are: every test owns a fresh server, so they repeat between recordings, and a
placeholder would hide a call that targets another pane. Hooks and supervisors
started inside a test's tmux server belong to the class that uses that socket.
Calls without an hgs ancestor, the fixtures' own tmux and `ps` calls, are listed
apart and compared only with `--include-direct`. `summarize` warns about tmux
calls that reach the recording user's default server (no `-S`, `-L`, `TMUX` or
existing `TMUX_TMPDIR`): a test that makes one is not isolated and must give hgs
a private socket or an existing `TMUX_TMPDIR`.

`compare` exits 1 on any difference, and 2 when the two files come from different
tool versions. It reports operations whose call sequences differ, what differs
(order, argv, exit status, environment, stdio, stdin, stderr), and call shapes
present in only one recording, including the calls of operations that an
`optional` rule drops. `scripts/ci/tmux-census-rules.json`
lists the nondeterminism measured on an unchanged commit, with the source of
each rule: back-to-back repeats of read-only observations in timed poll loops
(`answer`, `clear-context`, `compact-context`, `effort`, `settings`, `interrupt`,
`pause`), the error of the last `pause` poll when it meets the exiting server,
hooks and supervisors racing a test's `kill-server`, and tests that poll `hgs ls`
or emit hooks a timing-dependent number of times. Rule kinds are `replace` (a
pattern in a call's line), `optional` (drop an operation with a matching call),
`collapse`, `unordered` and `any_count` (ignore how often a distinct sequence
occurs); each applies to the operations whose key and test class match. Commands
that change tmux state keep their exact order and count. Add a rule only after two
recordings of one commit differ, never to absorb a difference that the refactor
introduced. Both test runs must pass: a failing test changes the census. When the
rules are in doubt, record main twice and compare the two recordings first.

### Terminal backend contract

`tests/test_backend_contract.py` states once what every terminal backend must do:
tmux on Linux and macOS, and the Windows session host. It is a black box at
the CLI: tests drive only `hgs` commands and observe only their output and JSON
and the records of the native fake agent (`examples/fake_agent.rs`, linked or
copied as `claude`), never the backend or the internal Rust `TerminalBackend`
trait. It covers listing without starting the backend; launches (folder,
arguments, launch ID, one session per name, untracked commands, a launch whose
session ended without a conversation); `a --existing [--run-id]` resolution and
attach from a terminal client (input, client size, several clients,
`Ctrl+B Ctrl+B`, detach, the session ending); the exact bytes of
`send` and Terminal input (one bracketed paste, then one Enter, no replay, keys in
both cursor modes); state read from the screen (styled capture, wide glyphs,
cursor, size, bracketed paste, an echoed draft against a dim placeholder) and
raw-mode evidence; rename; pause and resume; terminate and kill, with what each
does to the agent process. Signal expectations accept the Unix signal or the
Windows console event each operation sends: on Windows a pause is Ctrl+Break for
the supervisor's process group, terminate pauses first and then closes the
console, and kill closes it at once (spec D11).

`HGS_TEST_BACKEND` selects the backend: `tmux` (the default on Linux and macOS) or
`host` (the default on Windows, skipped elsewhere). Each test has a private home,
state and configuration directory. hgs puts the home's `.local/bin` first on its
`PATH` (ahead of Homebrew and `/usr/local/bin`; on Windows ahead of
`%APPDATA%\npm`), so the agent stand-ins live there. Only the backend classes in
the module differ: setup and teardown, `running()`, `describe()` (the backend's
state in failure messages), `alive()` (whether an agent process with a recorded
start time still runs) and the attach client.

- `tmux`: a private server through a `tmux` wrapper in that folder, a private
  `TMUX_TMPDIR` that keeps any other tmux off the user's server, the fake agent
  linked as `claude` and `fake_agent`, `ps` start times and a pseudo-terminal
  client.
- `host`: the test folder is also `USERPROFILE`, and `hgs session-host install`
  enables local sessions there, as `tests/test_windows_lifecycle.py` does: the
  build under test launches as a portable copy, the staged host image serves and
  the staged stable CLI supervises. The isolated `HGS_STATE_DIR` keeps the host's
  pipe away from any live host. The fake agent is copied as `claude.exe` and
  `fake_agent.exe` and answers hgs's version probe as Claude Code 2.1.296, whose
  hooks hgs writes in their exec form into a private `CLAUDE_CONFIG_DIR` (spec
  D12); `APPDATA` is private too. `running()` reads `hgs __host status`,
  `alive()` compares creation times through a process handle (spec D9), and attach
  runs `hgs a` on a pseudoconsole of the test (`PseudoConsole` of the lifecycle
  suite, spec D13). Teardown kills the sessions and stops the host through `hgs
  __host call`, then waits for the host and every agent and supervisor through
  handles opened while they ran, and ends and reports any that outlive it. Waits
  allow 30 s and commands 60 s, as in the lifecycle suite: a freshly copied
  executable may be scanned when it first starts.

The fake agent of a test has one configuration, set before its first launch,
because a backend may keep the environment of the launch that started it. The
module runs in the Linux and macOS CLI jobs in about 7 s, skips without tmux, and
passed 20 runs with 8 in parallel. The `windows-desktop` job runs it against the
host in its session host step
([below](#windows-client-desktop-and-portable-zip)).

### Windows client, desktop and portable zip

The `windows-desktop` job runs on a standard `windows-2025` runner. It disables
Git's CRLF conversion before checkout, so fixtures and scripts keep LF, and every
Python step uses one exact CPython 3.13 from the SHA-pinned `actions/setup-python`.
It runs `cargo test` with stable Rust and `cargo +1.99.1 clippy --all-targets -- -D
warnings`: clippy is pinned like the relay lints, so a new stable release cannot
fail an unchanged commit, and Windows-only code, compiled nowhere else, is checked
against Rust 1.99. It builds the release `hgs.exe` and the `fake_ssh` and
`fake_agent` examples and runs `tests/test_windows_client.py` and the session host
suites (`tests/test_windows_host.py`, `tests/test_windows_host_conpty.py`,
`tests/test_windows_host_survival.py`, `tests/test_windows_lifecycle.py`,
`tests/test_windows_hooks.py`, [testing the host](windows-host-testing.md)) and the
[terminal backend contract](#terminal-backend-contract) against them; real agents
run in a job of their own ([below](#windows-real-agents-through-the-session-host)).
Before the host suites, `scripts/fetch-conpty.py` fetches the
[ConPTY pair](#the-bundled-conpty-pair) once into `RUNNER_TEMP/conpty`; the host step names that folder in
`HGS_TEST_CONPTY_DIR`, and the portable zip takes the same files. Nothing caches
or uploads the folder. The runner puts every step in a job that forbids breakaway, where
hgs refuses to start a host (spec D4), so `scripts/ci/outside-job.py` starts the
host suites outside it through WMI. That copy restores the step's environment
without credentials and runs the suites in a kill-on-close job of its own that
allows breakaway, as an sshd session does. The step follows the log and exit
status, and after the suites or the 15-minute timeout it ends whatever they left,
including hosts of the tested `hgs.exe`. This is a CI harness only: the product
never escapes a job through WMI. The Rust cache uses its own
`windows-cli-` key with line-table debug information to bound its size. The runner
gives each step a synchronous anonymous pipe as stdin, and Windows OpenSSH never
exits after using one (the fake client refuses it), so steps that start `hgs.exe`,
`ssh` or the desktop app read stdin from `/dev/null`.

Qt 6.10.3 for MSVC 2022 x64 with Qt Multimedia comes from the official archives
pinned in `packaging/windows/qt.json`, which `scripts/install-qt-windows.py`
installs with the shared standard-library installer `scripts/qt_archives.py`
(also used for [macOS](macos-distribution.md#pinned-qt)). No PyPI package is
installed for it. The pin lists what aqtinstall 3.3.0 installed for this build
before: every archive of `qt.qt6.6103.win64_msvc2022_64` (`qtbase`, `qtsvg`,
`qtdeclarative`, `qttools` with `windeployqt`, `qttranslations`,
`d3dcompiler_47` and `opengl32sw`) and `qtmultimedia`, each with its URL path in
the repository, its size, its SHA-256 and the directory its `Extract` operation
in `Updates.xml` names. The installer downloads every archive (download.qt.io may
redirect to a mirror), stops reading at the pinned size, checks the SHA-256 and
lists the members with the runner image's 7-Zip before it extracts any archive.
A member with an absolute, drive, parent, device or alternate-stream name, or a
link, stops the installation; so does a link left in the extracted tree that
resolves outside it. A failed download is retried twice, each failure logged;
nothing unverified is kept. The result is aqtinstall's layout,
`RUNNER_TEMP/Qt/6.10.3/msvc2022_64`. Of aqtinstall's fixups only `bin/qt.conf`
(`Prefix=..`) is repeated: the official Qt is relocatable, the `qmake.exe`, `.prl`
and `qconfig.pri` patches find nothing to change in 6.10.3, and `qtenv2.bat` is
for interactive shells. Compared with an aqtinstall 3.3.0 installation of the
same release (on Linux, 2026-10-15), all other 11807 entries matched, except
for the line endings of `qconfig.pri`, which aqtinstall rewrites in text mode. On
Windows that rewrite keeps the archive's CRLF in `qconfig.pri` but gave the LF
`lib/*.prl` files CRLF, which only qmake reads; they now stay LF. The
installer also writes `zerus-qt.json`, the archive list it installed, and the
`Use Qt` step checks it with `--check`, so a restored cache must match the pin
too.

To update Qt, change the version, `prefix`, package names and directories in
`qt.json`, then run `python scripts/install-qt-windows.py --lock`: it reads the
archives and their `Extract` directories from the official `Updates.xml`,
downloads each archive and records its size and digest once it matches the
published `.sha256`. From Qt 6.11 each Windows architecture has a repository
directory of its own (`desktop/qt6_6112/qt6_6112_msvc2022_64` rather than
aqtinstall's expected `desktop/qt6_6112/qt6_6112`), which a package's `directory`
names; the 6.11 base package adds `qtdoc`. Review the locked pin like the other
pins and check `--output` locally (7-Zip on Linux works as well) before
committing. The Qt directory is cached with `actions/cache` under a key that
covers the Qt release, the pin and the installer. Like every cache, only trusted
main checks save it; PR, manual and nightly runs restore it (see
[build caches](#build-caches)). Such a run downloads Qt when no cache entry exists
yet. Qt WebEngine is not installed: the embedded DeepSeek Native UI is an
optional Linux plugin. The desktop build compiles through ccache.

`scripts/ci/gui.sh` re-enters itself through `scripts/ci/msvc-env.py`, which
locates Visual Studio with `vswhere` and applies `vcvars64.bat` (with its CMake
and Ninja first on `PATH`) to that build only, then builds with Ninja and `cl` and
runs every CTest suite offscreen.

`scripts/package-windows.py` bundles the exact tested `hgs.exe` and `hgs-tray.exe`
into `zerus-windows-x86_64.zip` with one top-level `zerus-windows-x86_64/` folder:

- `windeployqt` adds Qt, its plugins (including the SVG image and icon plugins
  loaded at run time) and translations. It runs with `--no-opengl-sw`: the Mesa
  software OpenGL fallback would need notices the zip does not carry. `--no-system-d3d-compiler` and `--no-system-dxc-compiler` keep
  Microsoft's shader compilers out: Windows includes `D3DCompiler_47.dll`, and
  the notices do not cover the Windows SDK's `dxcompiler.dll` and `dxil.dll`.
  The ICU DLLs it copies from the build machine's System32 (a forwarder
  to `icu.dll` on current Windows) are removed; `Qt6Core.dll` uses the system ICU.
  The minimum is Windows 10 1903, which the desktop app's UTF-8 code page also
  needs.
- The app-local Visual C++ runtime DLLs come from the default redistributable
  folder of the Visual Studio found by `vswhere`, never a hard-coded version.
- `conpty.dll` and `OpenConsole.exe`, the pinned [ConPTY pair](#the-bundled-conpty-pair),
  sit next to `hgs.exe`, so `hgs session-host install` from the extracted folder
  stages them into the host image. `--conpty` names a folder from
  `scripts/fetch-conpty.py`, whose files must match the pin; without it the
  script fetches the pair itself.
- `licenses/` holds the libvterm, md4c, Codex and ConPTY texts (the ConPTY license
  and Windows Terminal's notices), the host-target Rust
  dependency notices from `scripts/collect-licenses.py`, the per-module SPDX
  documents shipped with Qt and, in `licenses/qt`, the texts from
  `packaging/windows/licenses` (sources in its `README.md`). Packaging parses the
  documents and fails when one names a standard license or exception without its
  text there, or a `LicenseRef-` without extracted text in the documents.
- `LICENSE`, `THIRD_PARTY_NOTICES.md`, a `README.txt` for Windows users and
  `build-info.json` with the source commit, product, CLI and Qt versions.

`scripts/check-windows-portable.py` then checks every entry (no absolute, parent,
device, case-duplicate or symlink names, no ICU, software OpenGL or Direct3D
shader compiler DLLs and no Qt WebEngine files), the required runtime and license texts, the manifest
provenance and the limits: at most 512 MiB compressed, the release mirror's
asset limit, and 500 MB unpacked, the bound applied to whole nightly candidates.
`conpty.dll` and `OpenConsole.exe` must be the pinned builds, by size and SHA-256
read in memory, and no file of either name may sit anywhere but next to
`hgs.exe`; stable and nightly assembly and publication repeat these checks.
With `--run --selftest` on Windows it extracts the zip and starts both
executables with a minimal environment, without Qt or the bundle on `PATH`:
`--version` of each, `hgs.exe ls --json --local` must report the control-only
local computer, and `hgs-tray.exe --selftest` must find the `hgs.exe` next to
it. Last, `hgs.exe session-host install` enables local sessions in the
disposable profile and must stage the pinned pair into the host image; it starts
no host. The job uploads the zip and logs as `windows-x86_64-<commit>` without
recompressing them and keeps them for seven days (three for schedules). Qt
WebEngine, which made earlier zips about 140 MB (345 MB unpacked), is not part of
it. Only stable and nightly assembly use that zip. On main pushes the [Dev channel](public-distribution.md#verified-main-dev-channel)
waits for this job like every other suite, so a Windows failure also holds back
Dev, but Dev candidates carry no Windows build.

#### The bundled ConPTY pair

The session host prefers Microsoft's ConPTY from the NuGet package
`Microsoft.Windows.Console.ConPTY` to the inbox one (spec D6 of
[Windows Phase 2](superpowers/specs/2026-10-13-windows-native-host-design.md)): it
loads `conpty.dll` by absolute path from its image folder, and only when
`OpenConsole.exe` sits there too. `conpty.dll` runs the `OpenConsole.exe` next to
it, else one in an `x64` subfolder (where the package's own build targets put it),
else silently System32's `conhost.exe`; the zip checks therefore allow no copy
of either file outside the top folder.
`packaging/windows/conpty.json` pins version 1.25.260930003: the package by its
nuget.org URL, size and SHA-256
(`02b07b349af66d801159bdf9e440d4a1ce78bb951f37fc8609731665afdae7ee`), and the x64
`runtimes/win-x64/native/conpty.dll`
(`feeef341d891643c62d30b6b07800bc70f0bc148f44cb8c3bee84aa557ae805a`) and
`build/native/runtimes/x64/OpenConsole.exe`
(`3d66b23d0a71bb8eed2b77edc8b9df9bf54ce6c8fb74c863a30e760997f80586`) inside it.
The package attached to the microsoft/terminal release `v1.25.2733.0` differs only
in its signature file and holds the same two files byte for byte.
`scripts/fetch-conpty.py --output DIR [--nupkg FILE]` downloads the package (or
reads a copy), checks it before opening it, and writes only those two files once
they match; nothing in the package runs. The package declares MIT and carries no
license file: `docs/licenses/conpty-LICENSE.txt` is the `LICENSE` of
[microsoft/terminal](https://github.com/microsoft/terminal) (tag `v1.25.2733.0`,
unchanged since its first release), shipped as `licenses/conpty-LICENSE.txt`.
`OpenConsole.exe` also contains third-party code, such as Chromium's numerics
library (BSD-3-Clause), so `docs/licenses/conpty-NOTICE.md`, that repository's
`NOTICE.md` from the same tag, ships as `licenses/conpty-NOTICE.md`. Both are
stored with LF line endings; the text is otherwise unchanged.

To move to another version, read its `.nuspec` and build targets and the
exports `src/host/conpty.rs` resolves, record the new package, paths, sizes and
digests in the pin, refresh both notice files from the release tag that ships
that package, change the version named in `src/host/conpty.rs` (a contract
test compares them) and run the host suites, including
`tests/test_windows_host_conpty.py`, on Windows.

From Linux or macOS, `scripts/ci/check-windows-target.sh` type-checks all targets
for `x86_64-pc-windows-msvc` without the MSVC toolchain (`clippy` instead of the
default `check` runs `cargo +1.99.1 clippy -D warnings`). It needs `rustup target add
x86_64-pc-windows-msvc` for the toolchains it uses. Build scripts that compile C get a
stand-in compiler that writes empty objects, since nothing is linked; only the
Windows job builds and tests the real binary.

On a Windows development PC the same steps run from Git Bash;
[CONTRIBUTING.md](../CONTRIBUTING.md#windows) lists the requirements and commands.

### Windows real agents through the session host

The `windows-host` job runs the reviewed `win32-x64` Claude Code and Codex in
isolated session hosts of the release `hgs.exe` on a standard `windows-2025` runner
(M7 of [Windows Phase 2](superpowers/specs/2026-10-13-windows-native-host-design.md)).
It is selected with the Windows desktop job and the gate requires both, but it is a
job of its own: a failure points at the host, and the desktop job keeps its time
margin. Like that job it checks out text files unchanged and uses the same exact
CPython; it builds the release binary from the `windows-cli-` Rust cache, which it
restores and never saves, and fetches the [ConPTY pair](#the-bundled-conpty-pair)
the same way.

It then runs `scripts/ci/agent-compatibility.py` with `HGS_TEST_BIN` and
`HGS_TEST_CONPTY_DIR` ([native agent compatibility](#native-agent-compatibility-and-upstream-review)):
the runner verifies each package against its pin, extracts only `claude.exe` or
`codex.exe` and runs that agent's class of `tests/test_windows_native_agents.py`,
whose agents reach only a local fake API (Anthropic's for Claude Code, OpenAI's
for Codex) and keep their homes private. The suites confirm what spike S6 ran by
hand with these versions (a tracked launch to a ready composer, hook events in
listings, records and the journal, key input, pause, resume and terminate,
non-ASCII prompts) and assert what had run only with the fake agent: exact
`hgs send` acceptance with a trailing backslash and multiline messages, question
and approval answers, and clear. Compaction and the model and effort pickers are not
reviewed on Windows and stay out. Codex runs without its Windows sandbox: the
package's sandbox setup program and command runner, its code-mode host, `rg` and
voice libraries are never extracted.

The suites start session hosts, so `scripts/ci/outside-job.py` runs the whole
runner outside the step's job, as the desktop job does for the host suites, with a
45-minute bound inside the 50-minute step. Afterwards it ends what is left by
executable path: hosts of the tested `hgs.exe`, anything run from the suites'
`hgs-host-*` folders (copies and staged images) and the extracted agents in the
runner's `zerus-native-compat-*` folder. The step runs Python in UTF-8 mode, so a
non-ASCII prompt in a log cannot fail on the runner's code page. The harness log,
`report.json`, `summary.md` and each suite's log are kept as `windows-host-results`
like the CLI job's logs: for seven days, or three after a failed scheduled run.

### Real-peer smoke on a Windows PC

Hosted CI runs `hgs.exe` only against the fake SSH client. Changes to SSH handling,
the embedded terminal or swarm transport also need a manual check against a real
Linux peer from a Windows development PC. The names below (`winpeer`, user `peer`,
`/workspace/sample-project`) are placeholders; keep real host names, addresses and
accounts in private configuration.

1. **Peer.** Use a disposable Linux container or virtual machine with its own user,
   home, tmux server and hgs state, never a machine that runs live agents. Install
   the tested tree's `hgs` at `~/.local/bin/hgs`, add a project and start a
   detached shell session (`hgs sh sample-project -d`). Make its SSH server
   reachable from the PC, for example through a loopback-only tunnel.
2. **Isolation on the PC.** Use the executables from the extracted zip and point
   `HOME`, `USERPROFILE`, `APPDATA`, `LOCALAPPDATA`, `TEMP`, `TMP`, `HGS_CONFIG_DIR`
   and `HGS_STATE_DIR` at a scratch folder. That isolates `hgs.exe` and the desktop
   app's `tray.conf`, not the rest of `hgs-tray.exe`: Qt finds its settings,
   drafts and single-instance lock through the Windows known folders, which ignore
   `APPDATA` and `LOCALAPPDATA`, and names its single-instance pipe after the
   account's profile folder, whatever `USERPROFILE` says. No other Zerus tray may
   run during the desktop part (a second tray only activates the running one);
   otherwise run that part under a separate Windows test account. Before changing
   the variables, back up `hgdev\hgs-tray.ini` and the `hgs-tray.ini.drafts`
   folder next to it in the real Roaming AppData folder, and `hgdev\hgs-tray` in
   the real Local AppData folder, when they exist
   (`[Environment]::GetFolderPath('ApplicationData')` and
   `'LocalApplicationData'` name them). `ssh.exe` ignores `HOME` and
   `USERPROFILE` and keeps using the real `%USERPROFILE%\.ssh\known_hosts`. Back up
   that whole file before the first connection and restore it byte for byte
   afterwards: OpenSSH's `UpdateHostKeys` appends the peer's other host keys, so
   removing the one line you added is not enough. Alternatively point `HGS_SSH` at
   a small wrapper executable (not a batch file) that inherits its standard handles
   and runs `%SystemRoot%\System32\OpenSSH\ssh.exe -o UserKnownHostsFile=<scratch
   file> -o UpdateHostKeys=no` followed by its own arguments.
3. **Checks.** Run them from PowerShell, or from Git Bash with `MSYS_NO_PATHCONV=1`:
   - register the peer's host, port, user and key with
     `hgs.exe machine set winpeer --json '{…}'` (Windows PowerShell 5.1 strips
     unescaped double quotes from native arguments, so write them as `\"` there),
     then run `hgs.exe machine check winpeer`;
   - `hgs.exe '@winpeer' ls --json`, then `hgs.exe ls --json` (the control-only
     local box and the peer) and plain `hgs.exe ls`. Time ten `@winpeer ls --json`
     calls: about 150 ms each over a LAN on 2026-10-11, almost all of it the SSH
     handshake;
   - forwarded read-only commands: `inspect`, `project ls --json`, `dirs`,
     `git-status --path /workspace/sample-project --json` and `worktrees`;
   - join the peer's swarm and run `hgs.exe swarm sync`: `hgs.exe swarm get` and
     `hgs.exe '@winpeer' swarm get` must report the same `digest`. With
     `hgs.exe swarm worker` running, a preference change reaches the peer within
     seconds;
   - agents, when a change touches agent control: install the reviewed Claude,
     Codex, Kimi and DeepSeek CLIs on the peer and point them at a loopback-only
     fake model (`ANTHROPIC_BASE_URL`, a Codex `model_providers` entry, a Kimi
     `providers` entry and the DeepSeek test model from
     `tests/fixtures/dsh-model.mjs`), never at an account. For each agent, run
     from `hgs.exe`: a `--new -d --launch-id` launch, `answer` for the startup
     dialogs, `send` with Cyrillic, several lines and a trailing backslash plus an
     image attachment, `history`, `terminal` snapshot, `interrupt`,
     `compact-context`, `clear-context`, and the `session-action` steps the
     session offers. Run the same sequence from a Linux client; every step must
     give the same result. Attach through a pseudoconsole with
     `a <session> --existing --run-id <run>`, type, resize and detach with
     `Ctrl+B d`. On 2026-10-12 both clients matched for Claude 2.1.296, Codex
     0.162.1, Kimi 2.1.1 and dsh 0.2.0-rc.2;
   - when the PC's desktop is free: attach the desktop app's Terminal to the peer's
     session, type, resize, interrupt a running command with Ctrl+C and
     disconnect. `hgs.exe` and `ssh.exe` must end, and the session must stay on
     the peer.
4. **Cleanup.** End every `hgs.exe`, `ssh.exe` and `hgs-tray.exe` started for the
   smoke, restore `known_hosts`, and delete the scratch folder and the disposable
   peer. In the real Roaming and Local AppData folders, remove
   `hgdev\hgs-tray.ini`, `hgs-tray.ini.drafts` and `hgdev\hgs-tray` written by
   the smoke, or restore their backups.
   Never attach to, stop or restart live sessions or tmux servers.

### Native agent compatibility and upstream review

Full Linux and macOS CLI CI also run `python3
scripts/ci/agent-compatibility.py`, and the Windows real-agent job runs it on
Windows ([above](#windows-real-agents-through-the-session-host)). Each reviewed
version in `scripts/ci/agent-compatibility.json` names the official npm package
for one target, `linux-x64`, `darwin-arm64` or `win32-x64`, with an exact version
and SHA-512 integrity pin. The runner selects the pins for its host and refuses an
agent that has none. The Codex matrix pins 0.162.0 for Linux and Windows and
retains 0.160.1 (a resident-session regression boundary) on Linux only. The Claude
Code matrix pins 2.1.296 for all three targets, so macOS runs Claude Code alone.
Only the native executable is extracted into a disposable directory (`claude.exe`
and `codex.exe` on Windows, never Codex's sandbox setup program or its other
helpers); agent binaries are neither committed nor bundled. The runner treats
skipped or empty native suites as failures. It writes each suite's output to
files rather than pipes, so a process a suite leaves behind cannot keep the run
waiting, and a suite that times out still leaves its log. `--agent claude` or
`--agent codex` limits a local run to one matrix. `--hgs` names the build under
test (default: `HGS_TEST_BIN`, else the debug build). The suites get a private
home; of the caller's `HGS_` variables only `HGS_TEST_CONPTY_DIR` reaches them. On
Windows the runner's own version, help and schema probes also get a private
`USERPROFILE`, `APPDATA` and `LOCALAPPDATA`, while the suites keep the real
profile: each case gives its hosts and agents a profile of its own, and Codex
prints folders relative to the real one, which the suites place projects by.

On Linux and macOS, Claude Code runs every `tests/test_claude_*_native.py` suite
plus the Claude clear and compaction fixtures: exact input, questions, approvals,
trust, resume, clear, compaction, session-only model settings and the status line
context window. Each test launches the real TUI under `env -i`, or through a tracked
`hgs` launch without the caller's `ANTHROPIC_` and `CLAUDE` variables, with a fixture
API key, a loopback or local fake API and no nonessential traffic, so no account,
telemetry or model request leaves the runner. On Windows each agent runs its class of
`tests/test_windows_native_agents.py` through the session host instead.

Real native checks run at 95x47 and 110x35 terminal sizes with animations explicitly
enabled and disabled. Animated 95x47 checks require the actual released settled
idle logo before inspecting a Terminal clear; unknown or partial logo frames do
not confirm a reset. Assertion failures retain the synthetic native panel,
cursor and lifecycle state. The tests resume synthetic
history, then verify GUI-command clear, Terminal clear, the deferred native
identity transition, stale usage suppression, a single durable clear boundary,
preserved process/run identity, unsent native drafts and foreign-directory
rejection. A resumed conversation whose `SessionStart` is still deferred must
pause, and clear from the GUI and from Terminal, where the next message's hook
names the new conversation. A new session in a folder outside the fixture home,
which Codex prints in full in its header, answers a first message from the localhost
provider; idle polls must not treat that header above the history as a reset,
and a later clear must still be confirmed. A separate fixture caches a newer
version for the startup update picker and verifies its Activity identity,
**Skip** and **Skip until next version**. It selects **Update now** only after
checking that a fake `npm`, first on Codex's own `PATH` and with a private npm
prefix, stands in for the installer; the announced command must deliver the
choice while that fake still runs, and no input may reach it.
Nested real `codex exec --ephemeral`
reviews verify separate child Activity under Claude and Codex parents, preserved
parent identity/model/history and the child's recorded result without a rollout.
Codex async questions (feature `send_message_to_user_async`) come from a
localhost provider that holds the following turn open, because Codex shows the
panel only while a turn works. `hgs inspect` and `hgs answer` must recognize each
pinned panel (`ctrl+] skip` in 0.160, `^] skip` in 0.162, both with a `1 of 2`
line), keep a native draft, answer an option, Other and free text, and skip
without a reply. Codex 0.162 steers each answer into the working turn; 0.160
queues it until the turn ends, so the answer stays submitted until recorded.
Every fixture owns a private home
and tmux socket and uses only a localhost provider. No authenticated inference
or existing session is involved.
Fixture shutdown waits for its owned panes to exit before deleting the private
home, so a shell's final history write cannot race with filesystem cleanup.
Codex fixture homes turn plugins off, and each fixture checks the effective value
with `codex features list`: plugin sync clones the curated plugins from the
network through a git child that can outlive Codex, `codex exec` included, and
write into the home while it is deleted.

The separate **Agent upstream compatibility** workflow runs daily at 04:41 UTC
and is also available manually on upstream main. It discovers published Codex,
Claude Code, Kimi Code and DeepSeek Harness versions from their official package
indexes, links their official release notes and tests the latest Codex and
Claude Code as additional canaries. It does not advance reviewed pins or install
anything on development machines. Its hosted job has read-only repository
access, no cache access and no publication credentials. Reports, native logs
and public protocol schemas expire after three days. A discovery or
native-contract failure fails the run; successful unittest skips do not hide
missing coverage.

Codex protocol schemas are generated separately with and without experimental
APIs. The report compares the latest canary with the newest reviewed pin and
lists added/removed methods and added/removed/changed schemas. Changes to Codex
and Claude Code CLI help appear with a generated diff per agent. A detected
addition is a review candidate, not evidence that Zerus supports it or that a
signed-in account can use it. For Kimi this workflow currently discovers
versions only. DeepSeek's existing pinned keyless contract fixture runs in full
Linux CLI CI.

To run the reviewed matrix or inspect current upstream changes locally:

```sh
cargo build --locked
python3 scripts/ci/agent-compatibility.py
python3 scripts/ci/agent-compatibility.py --agent claude
python3 scripts/ci/agent-compatibility.py --latest
```

On Windows it takes the release build and the ConPTY pair as
[testing the session host](windows-host-testing.md#real-agents) shows. The
generated `artifacts/test-results/agent-compatibility/summary.md` and
`report.json` contain the coverage result and review candidates. Before changing
a pin, read the provider's release notes and compare CLI flags, hook events,
native history and protocol schemas. Classify each relevant feature as already
supported, requiring an adapter change, or requiring a separately reviewed
integration; track implementation work in Beads. Add an isolated native contract
for every adopted lifecycle/input change and retain versions still used by
resident sessions until their compatibility boundary is deliberately reviewed.
Pin a reviewed version for every target its agent supports. Only an older version
kept as a regression boundary may cover fewer targets: each of its pins gives the
reason in `retained`, and the newest version is never retained. The runner refuses
a manifest that breaks this rule, and its unit tests cover it.

Zerus receives activity from structured hooks, conversation messages and usage
from native history, and DeepSeek controls from its native API. Terminal checks
verify input ownership, empty composers and native dialogs, including Codex's
reset before its deferred SessionStart. Prefer a structured provider protocol
when it can address the same exact live process/run/conversation. A separate
app-server process must not be assumed to own an existing terminal session.
See [Codex App Server](https://developers.openai.com/codex/app-server) for its
JSON-RPC protocol and version-specific schema generation. Migration of live
terminal-backed sessions requires separate ownership/resume verification.

No blanket formatting/clippy gate was added over legacy source: it would reject
unrelated existing formatting rather than verify this change. Existing unit,
integration and Qt suites provide the initial gate. Add stricter linting with a
separate, reviewed baseline cleanup. The Windows job is the exception: it runs
the pinned Rust 1.99 clippy with `-D warnings` over every module compiled for the
Windows target, which is clean today; Linux-only modules are not linted.
Logs are under ignored
`artifacts/test-results/`; packaging outputs are under ignored `artifacts/` and
`dist/`.

## Automatic nightly publication

`nightly.yml` runs daily at 00:17 UTC (03:17 Europe/Moscow) and supports manual
launches. It verifies one exact upstream `main` commit with the complete Linux,
Arch, macOS and Windows CI matrix, then repackages its already tested Arch binary
and copies the tested Windows zip unchanged. It checks the generated pacman
package, namcap output and installed bundle in an isolated home. There is no
second Rust/C++ compilation during nightly assembly.

The package is `zerus-ade-nightly-bin`; `zerus-git` remains a source package that
tracks upstream `main`. Nightly versions have the form
`0.37.0.r62.gabcdef0.n7`: product version, Git revision count, short source SHA
and monotonically increasing Nightly run number. The application keeps its
product/CLI versions. Rebuilding an unchanged commit creates a new package
version, allowing daily Arch ABI/dependency refreshes without replacing assets.

After a successful Nightly run, `publish-nightly.yml` automatically verifies the
upstream repository, workflow path/event, main branch, completion status, exact
source SHA, build number, GitHub artifact digest, sealed AUR recipes and checksums.
Only then does it receive the dedicated AUR key from the `release` environment.
Downloaded scripts/PKGBUILDs are never executed with publication credentials.

Each build creates an immutable `nightly-RUN_ID` GitHub prerelease and updates only
`zerus-ade-nightly-bin` in AUR. It never becomes the latest stable release or changes
`zerus`/`zerus-ade-bin`/`zerus-git`. Its public binary URL and SHA-256 stay fixed.
With `MACOS_CASKS_ENABLED=true` the Nightly run also builds, signs and notarizes the
Apple Silicon bundle with the same nightly version, adds it to the prerelease
(candidate schema 2) and updates only `Casks/zerus@nightly.rb` in the tap after AUR.
A failed macOS bundle fails the nightly, so AUR and Homebrew nightlies stay equal.
Binary library floors come from the verified source package's `.BUILDINFO`.
Stable and nightly publishers share a serialization group, preventing AUR races.

The Windows zip from the same CI run becomes `zerus-<nightly version>-windows-x86_64.zip`.
`prepare-nightly.py` reads only its zip directory and a bounded `build-info.json`,
requires the nightly's commit and product version, and records the name as
`windows_portable` in `release-info.json`. The zip is sealed in `SHA256SUMS`, and the
publisher checks its layout and provenance again before uploading it to the same
immutable prerelease; nothing in it is extracted or executed. A candidate whose
manifest names a zip must contain exactly that file, while retries of candidates
built before the Windows job remain valid. Because nightly publication requires the
whole CI run to succeed, a Windows failure also holds back the Arch nightly.

The zerus.dev mirror accepts the zip in the nightly asset family and checks its
GitHub digest, but neither mirrors it (the mirror never deletes and has a fixed
disk budget) nor lists it in the v1 desktop feed: deployed GUIs reject unknown
artifact kinds. Deploy the updated `scripts/update_feed.py` to the mirror before
the first nightly with a zip: older mirror code fails closed on the new asset
family and stops updating the nightly feed.

```sh
# Build and automatically publish a new fully verified nightly:
gh workflow run nightly.yml --ref main
# Retry partial publication using the same successful retained candidate:
gh workflow run publish-nightly.yml --ref main -f candidate_run=123456789
# Install the latest prebuilt nightly (conflicts with the other Zerus flavors):
yay -S zerus-ade-nightly-bin
# Source-based main builds require devel update checks:
yay -Syu --devel
```

Failed builds never publish. GitHub or AUR publication failures leave stable
releases untouched. Retry only the publisher with the same run ID; do not rerun a
published build to produce different bytes under its existing tag. If GitHub
refuses the job token's draft (seen once, for 0.42.0, as `403 Resource not
accessible by integration`), the publisher stops before any public write and prints
the release notes and the exact `gh release create ... --draft --target <commit>`
command. The owner creates that draft with their own token and reruns the same
publisher for the same candidate, which verifies the draft's target and uploads
only the sealed assets. Artifacts are
retained for three days and logs for three; public GitHub release assets remain
available after artifact expiry. Nightlies run even without new commits to test
current rolling Arch dependencies and produce fresh, uniquely versioned binaries.
After an accepted AUR push, RPC metadata and yay may briefly show the previous
version. Check the AUR Git repository's `.SRCINFO` and wait for metadata to refresh;
do not rebuild or republish the candidate to work around that delay.
Stable candidates/publication remain separate manual workflows; macOS checks are
now selected by default for new candidates and can still be explicitly disabled.

## What yay actually installs

`yay` is an AUR helper. AUR hosts a public `PKGBUILD` and generated `.SRCINFO`,
which tell users how to obtain and build a pacman package. Binary release archives
live upstream; they are not uploaded into AUR Git.
[AUR submission guidelines](https://wiki.archlinux.org/title/AUR_submission_guidelines),
[PKGBUILD manual](https://man.archlinux.org/man/PKGBUILD.5.en).

The owner selected the stable/source family on **2026-10-08** and added automated nightly binaries on **2026-10-09**:

| Name | Source | Purpose |
| --- | --- | --- |
| [zerus](https://aur.archlinux.org/packages/zerus) | Versioned source archive with SHA-256 | Stable release compiled locally |
| [zerus-ade-bin](https://aur.archlinux.org/packages/zerus-ade-bin) | Versioned Arch x86_64 binary archive with SHA-256 | Stable release without Rust/C++ compilation |
| [zerus-git](https://aur.archlinux.org/packages/zerus-git) | Public upstream `main`, version derived by `pkgver()` | Development build compiled locally |
| [zerus-ade-nightly-bin](https://aur.archlinux.org/packages/zerus-ade-nightly-bin) | Immutable verified nightly binary with SHA-256 | Current main without local compilation |

Primary owner/maintainer: **ufna**, **Vladimir Alyamkin <ufna@ufna.dev>**.
The stable/source AUR package pages confirm the approved maintainer. The publisher
rechecks ownership and SSH write access before writing. All flavors install the
same files and conflict with one another. The binary/VCS flavors provide versioned `zerus`.

Open-source prebuilt packages require the `-bin` suffix. `zerus-bin` belongs to
an unrelated project, so our binary package uses `zerus-ade-bin`.
[AUR naming rules](https://wiki.archlinux.org/title/AUR_submission_guidelines),
[existing zerus-bin package](https://aur.archlinux.org/packages/zerus-bin).

Stable recipes are templates until candidate assembly inserts the release version,
archive hashes and binary library floors. The VCS recipe tracks `main` explicitly;
its seed version is updated by makepkg. `SKIP` is only for its Git source.
Generate `.SRCINFO` with `makepkg --printsrcinfo`; never edit it manually.
[Rust package guidelines](https://wiki.archlinux.org/title/Rust_package_guidelines),
[VCS package guidelines](https://wiki.archlinux.org/title/VCS_package_guidelines),
[.SRCINFO](https://wiki.archlinux.org/title/.SRCINFO).

### Dependency declarations

| Class | Packages | Reason |
| --- | --- | --- |
| Desktop runtime | `qt6-base`, `qt6-multimedia`, `qt6-svg`, `kstatusnotifieritem`, `kwindowsystem`, `kidletime`, `hicolor-icon-theme` | Widgets, voice recording, SVG plugin, tray/window integration, idle detection and icons |
| CLI/runtime tools | `tmux>=3.7`, `openssh`, `python`, `curl`, `procps-ng`, `bash`, `tar` | Persistent sessions, remote access, compatibility helper, account HTTP, process inspection, installers and remote source transfer |
| Native library runtime | `glibc`, `libgcc`, `libstdc++` | Dynamically linked CLI/desktop binaries |
| Source build only | `rust`, `cmake`, `ninja`, `qt6-webengine`; additionally `git` for `zerus-git` | Locked Rust and production Qt builds, the native UI plugin; Git source/version discovery |
| Standard build environment | `base-devel` | Required by Arch's makepkg/AUR build convention; its tools are not repeated in makedepends |
| Optional features | `nodejs`, `npm`, `git`, `konsole`, `wl-clipboard`, `xclip`, `systemd`, `qt6-webengine` | Native agent installation/runtime, worktrees, external terminals, clipboard, user services/scopes and the embedded DeepSeek Native UI |

The binary package has no Rust/CMake/Ninja build dependencies. Its Qt/KDE, C/C++
runtime and glibc dependency floors are generated from the **verified builder's
`.BUILDINFO`**, not the publication runner. `release-info.json` records these
versions. This prevents installing that binary against older libraries that may
lack symbols; Arch users should keep a fully updated system. A fresh binary/ABI
rebuild gets a new product version and immutable assets.

Node.js and npm are separate optional dependencies. Native agents are installed
separately through Accounts or their upstream installers; agent executables,
credentials and user configuration are never bundled. Systemd integration is
optional and detected at runtime. Packaging never enables services.

### Package contents and compatibility

Each flavor installs the same ADE bundle:

- `/usr/bin/hgs`, `/usr/bin/hgs-tray`, the `hgs_state.py` compatibility shim and
  the explicit `zerus-setup` migration helper, whose `--mobile-connector
  {protect,migrate,rollback}` runs `/usr/lib/zerus/install-mobile-connector-service.py`.
- `/usr/lib/hgs-tray/plugins/libhgs-tray-native-ui.so`, the embedded DeepSeek
  Native UI. It alone links Qt WebEngine and loads only when `qt6-webengine` is
  installed; otherwise Native UI opens in the browser. `check-package.py`
  requires it and rejects an `hgs-tray` that links Qt WebEngine itself.
- `/usr/bin/zerus-mobile-connector`, the Rust mobile connector, and its user
  unit `zerus-mobile-connector.service` (`KillMode=process`, configuration
  `%E/hgs/mobile/connector.json`, skipped while that file is missing). The
  package never enables it, and an existing
  `~/.config/systemd/user/zerus-mobile-connector.service`, such as the Python
  connector's, keeps precedence; switching an installation over is the
  owner-approved cutover in the connector design. `check-package.py` requires
  both, a release build without test hooks that links only the C runtime, and
  the unit's `ExecStart` and kill mode.
- A desktop entry, existing application icons and system-provided **user** unit
  definitions for `hgs-tray`, `hgs-swarm` and `hgs-recovery`.
- Documentation and a sample tmux configuration under `/usr/share/doc/zerus`.
- Zerus MIT, vendored libvterm MIT and the exact host-target Cargo dependency
  license texts of `hgs` and the mobile connector (both locks) under
  `/usr/share/licenses/<package-name>`.

Qt and KDE libraries are dynamically linked system dependencies. Qt WebEngine is
needed only by the native UI plugin. Upgrades keep an installed `qt6-webengine`:
an optional dependency is not an orphan for `pacman -Qdt`. Candidates sealed
before Qt WebEngine became optional still validate for publication retries.
`qt6-svg` supplies the dynamically loaded SVG image plugin used by menu and
combo-box arrows; a Qt test renders the actual bundled resources so a missing
plugin fails CI. The current Arch Qt base package supplies its Wayland display
plugin. [Arch Qt SVG package](https://archlinux.org/packages/extra/x86_64/qt6-svg/).
The runtime also declares curl for native account HTTP requests and procps-ng
for process inspection. Optional Node.js/npm enable agent installation from
Accounts; Git enables repository/worktree features. Arch's Node.js package does
not include npm, so both optional dependencies are listed.
The package does not bundle their distributions, native agent executables,
accounts, credentials or configuration. Package installation does not start or
stop services, edit user homes or replace the user's tmux configuration.

The product package version currently follows the desktop version: `VERSION`
and `tray/CMakeLists.txt` are **0.42.0**. The independently versioned compatible
CLI is **1.51.0**. CI rejects a product/desktop version mismatch; packaging
records both versions in `build-info.json`, with the source commit when built
from Git. For source-archive builds, the release manifest and archive SHA-256
provide the commit provenance.
Stable fixes use patch releases (`0.42.1`, `0.42.2`); feature releases use a new
minor version. `DEV_VERSION` names the next planned stable release and trusted
main CI embeds its growing sequence (`0.42.1-dev.215`). Android follows the same
rule with its independent `mobile/android/DEV_VERSION`. See the
[version rule](public-distribution.md#version-rule) for identity and channel
ordering. Bump both stable and next Dev bases when preparing a stable release;
ordinary main commits advance only the Dev sequence, never stable release tags.
Increase the product/desktop version for each new public bundle, including a
CLI-only bundle change, so published archive URLs and hashes remain immutable.
A recipe-only `pkgrel` bump can reuse immutable upstream assets. A newly compiled
binary needs a new product version under the prepared archive naming scheme;
do not replace an existing release archive during an ABI rebuild.

Existing runtime paths and IDs remain `hgs`/`HGS_*`/`hgs-tray`.
The desktop entry keeps the `hgs-tray.desktop` ID so existing launchers remain
associated with the same application. A local source-install entry can shadow
the system entry; inspect it deliberately during migration.
Remote SSH protocols call `~/.local/bin/hgs`. Pacman cannot install into arbitrary
user homes, so each user runs this once, **without sudo**:

```sh
zerus-setup
```

It creates compatibility symlinks to `/usr/bin` only after validating every
target and conflict. It is idempotent, preserves existing files and dangling
symlinks, and refuses root execution. An existing source installation needs an
intentional migration: inspect and move aside conflicting executables before
running it. Do not remove `~/.config/hgs` or agent/session state.

Old user unit files in `~/.config/systemd/user` can shadow packaged definitions;
inspect `systemctl --user cat hgs-tray.service` and the worker units before
switching. Back up old definitions deliberately. Enable background services only
when wanted:

```sh
systemctl --user daemon-reload
systemctl --user enable --now hgs-tray.service
systemctl --user enable --now hgs-swarm.service hgs-recovery.service
```

`hgs-tray`, `hgs-recovery` and `hgs-swarm` use `KillMode=process`, both as
packaged and as the source installers write them (`install.sh` also writes
`hgs-voice`). A restart, or a worker exiting after an upgrade replaced `hgs`, then
ends only the unit's main process: native agents, tmux servers and DeepSeek hosts
that it started keep running. Under systemd's `control-group` default the same
events kill them. Units installed by earlier releases, and a
`zerus-mobile-connector.service` made from earlier
[mobile setup](mobile-deployment.md#protect-running-agents) instructions, may still
use that default. Before restarting or upgrading one, add a user drop-in with
`[Service]` and `KillMode=process`, run `systemctl --user daemon-reload` and verify
`systemctl --user show <unit> -p KillMode`. `install.sh` rewrites the worker units
and reloads the user manager before it restarts them. Prefer `restart` to `stop`:
tmux builds with systemd support put panes in `tmux-spawn-*.scope` units that are
`PartOf=` the unit holding the tmux server, so stopping that unit ends those panes
whatever its kill mode; `systemctl --user show <unit> -p ConsistsOf` lists them.
Package upgrades do not perform a restart. Review the old unit's kill policy before
restarting an existing installation; never use a broad process kill or restart all
native clients.

`hgs` starts a new tmux server from any user unit in a transient scope of its own
(see [Terminal, SSH and clipboard](reference.md#terminal-ssh-and-clipboard)), so
the panes of that server are not part of `hgs-tray`, the mobile connector or a
worker. A server that an earlier release started inside one of those units stays
there until it exits, and so does one started where systemd could not make the
scope: until then, restart that unit rather than stop it.

The swarm worker is the exception: it holds no agents. A current `hgs-swarm`
exits when an upgrade replaces its executable and `Restart=always` starts the new
version. A worker started by an earlier release keeps running and cannot read a
catalog that holds the phone notification timing, so the next `hgs swarm get`
(Zerus reads it every five seconds), `hgs swarm preference` or inbound swarm
exchange runs `systemctl --user try-restart hgs-swarm.service`, at most once
every ten minutes and only for the default `~/.config/hgs`. It never restarts
`hgs-tray`, `hgs-recovery`, agents, tmux servers or DeepSeek hosts. If Settings
still reports an outdated sync service, run
`systemctl --user restart hgs-swarm.service`.

## Manual build and publication

The public upstream is [ufna/zerus](https://github.com/ufna/zerus). There are two
independent manual workflows. Neither runs on pushes, pull requests, tags,
GitHub releases or a schedule.

### 1. Build and review a candidate

`release.yml` runs quick checks plus full Linux CLI, Arch, macOS and Windows
validation from one exact commit. macOS can be explicitly disabled; Windows
cannot, because the candidate carries the portable zip that this run tested. It
consumes that run's Arch package and Windows zip, verifies their embedded
version/source commit, assembles source and binary archives, rebuilds both stable
recipes, runs namcap and seals the result.
Stable and nightly candidates call the common `checks.yml` workflow directly
with read-only cache access. Their verification never includes the main-push dev
publisher or requests its tag-writing permissions.

```sh
# Only when the owner requests a hosted build:
gh workflow run release.yml --ref main -f version=0.42.0
# After this manual run succeeds:
gh run download RUN_ID --name zerus-0.42.0-candidate --dir /tmp/zerus-candidate
cd /tmp/zerus-candidate
sha256sum -c SHA256SUMS
```

The artifact contains source/binary archives, two checked pacman packages, the
Windows portable zip `zerus-VERSION-windows-x86_64.zip`, `release-info.json`, all
three concrete `PKGBUILD`/`.SRCINFO` pairs, their sealed `zerus-VERSION-aur.tar.gz`
archive and `SHA256SUMS` covering every release asset.
Git export attributes omit tracker, agent instructions/skills, workflows and
prototype artwork from the source archive; runtime artwork and notices remain.

The zip is the `windows-x86_64-<commit>` artifact of the same run, copied
unchanged under its stable name, exactly as nightlies take theirs.
`prepare-release.py` reads only its zip directory, a bounded `build-info.json` and
the pinned ConPTY pair, requires the candidate's commit and version (the bundled
`hgs-tray.exe` then reports that stable version with its commit and no Dev build
number) and records the name as `windows_portable` in `release-info.json`.
Validation and publication repeat these checks; nothing in the zip is extracted or
executed. A manifest that names the zip requires exactly that file, and an
undeclared zip fails. Candidates sealed without one, such as 0.42.2 and every
earlier stable release, remain valid for publication retries; a later candidate
without it fails validation, because portable Windows apps announce every stable
release after 0.42.2 as a Windows update.

Review the source commit, package logs, dependency/license inventory and recipes.
`create_draft=true` additionally creates a GitHub draft; by default everything
remains in the candidate artifact. Candidate artifacts expire after fourteen days,
logs after seven. Old 0.36.2 artifacts predate the new names/schema and cannot be
published by the new workflow. Never substitute a package built from another SHA.

Equivalent local assembly on Arch, with a clean committed checkout and its exact
verified package, remains available:

```sh
python3 scripts/prepare-release.py --version "$(cat VERSION)" \
  --package artifacts/zerus-git-EXACT_VERSION-x86_64.pkg.tar.zst \
  --windows-zip artifacts/zerus-windows-x86_64.zip --output dist
bash scripts/check-release-packages.sh dist
```

The Windows zip must be the one CI built and tested from the same commit; stable
releases after 0.42.2 carry it, and desktop apps rely on that. The helper requires
an empty output directory. Build artifacts are ignored and are never committed to
the application or AUR repositories.

Once the repository variable `MACOS_CASKS_ENABLED` is `true`, the candidate run
also builds the Apple Silicon bundle against the pinned official Qt, signs and
notarizes it in the main-only `macos-signing` environment and installs the result
through a local tap before assembly (`macos-cask.yml`). The candidate then uses
release-info schema 2 and contains the `zerus-VERSION-macos-arm64.zip` asset and
`homebrew/zerus.rb`. Until then, and with `-f macos_cask=false`, the candidate has
no macOS asset. Details, secrets and retry rules are in
[macOS distribution](macos-distribution.md).

### 2. Publish the reviewed candidate and update AUR and Homebrew

`publish.yml` takes the **successful candidate run ID** and its version. It uses
one Ubuntu runner, does no compilation and allocates no Arch/macOS runners:

```sh
# Only after review and an explicit owner publication request:
gh workflow run publish.yml --ref main \
  -f version=0.42.0 -f candidate_run=RUN_ID
```

The publisher verifies the same-repository manual run came from `main`, succeeded,
and contains exactly one unexpired versioned artifact. It checks GitHub's artifact
SHA-256, the manifest's exact source commit, all asset checksums, sealed AUR files
and public recipe URLs. Candidate source/recipe code is never executed with the
publication credential.

Before any GitHub release write it authenticates to AUR, checks ownership of all
names, prepares Git changes and refuses recipe downgrades. It creates or finishes
a draft for the exact commit, publishes it, verifies **every asset anonymously**,
then pushes the stable source, binary and VCS recipe updates. AUR Git receives only
`PKGBUILD` and `.SRCINFO`; release archives stay on GitHub. A macOS candidate also
stages the cask in `ufna/homebrew-zerus` with a dry-run push before the GitHub
write and pushes it after the AUR recipes.

The Windows zip reaches the same release unchanged, and the publisher checks its
layout and provenance again first. Release notes the publisher creates describe
it; a `create_draft` draft keeps its own notes. Like the nightly zip it stays on
GitHub: the zerus.dev mirror checks its sealed name and GitHub digest but neither
stores it nor lists it in the v1 desktop feed. Deploy the updated
`scripts/update_feed.py` to the mirror before publishing the first stable release
with a zip: older mirror code accepts a zip only in nightlies and fails the whole
`desktop/stable` job on that release. It keeps the previous stable catalog and,
where the same job also runs `--dev-platform desktop`, the previous desktop Dev
catalog too.

A separate `winget` job then proposes that zip to `microsoft/winget-pkgs` as
`ufna.Zerus` ([winget](public-distribution.md#winget)). It runs after the release
is public, reads only published assets and never proposes a version twice, so a
failed submission is retried with **Re-run failed jobs** without repeating the
release, AUR or Homebrew steps.

The VCS flavor follows `main` automatically when users rebuild. Release-time sync
updates its recipe when dependencies/build/package logic change; pure `pkgver`
bumps (including the corresponding versioned provides) do not create AUR commits.
Arch's submission rules explicitly disallow version-only VCS updates.

Publication runs are serialized across versions. Existing public assets must
match exactly; missing or different public assets stop publication. Uploads never
use `--clobber`, tags never move and AUR pushes never force. An interrupted draft
can resume only with matching existing assets and the exact commit. If an AUR push
fails after GitHub publication, **rerun Publish release with the same candidate
run and version**. Completed recipes become no-ops; the remaining updates retry.
The two services are not an atomic transaction, so inspect the failed job's output.
Do not rebuild or replace published archives to recover a partial AUR update.

Description-only AUR maintenance updates both `PKGBUILD` and generated `.SRCINFO`
without changing `pkgver`, `pkgrel`, dependencies or release assets. It does not
trigger package upgrades. Retrying a retained candidate preserves the explicitly
approved description refresh; every other recipe difference still follows the
normal version and provenance checks. Older sealed nightlies accept their exact
previous description, with the rest of the trusted binary template unchanged.

Publication retries require the same retained candidate artifact. After expiry,
recover the exact published assets/recipes for local review; do not regenerate
archives and assume identical checksums. A functional packaging-only stable update
needs an explicit `pkgrel` bump and a separately reviewed recipe change.

### Publication credentials

The dedicated AUR SSH public key is registered to **ufna** and SSH authentication
was verified on **2026-10-08**. Its private key is configured as the GitHub
**release environment** secret `AUR_SSH_PRIVATE_KEY`. That environment permits
only the `main` branch. CI/candidate jobs receive no AUR key. Publication jobs use
`actions: read` and `contents: write`; only the `winget` job needs a personal token:
the `release` secret `WINGET_TOKEN`, a classic token with the `public_repo` scope of
the account that owns the `microsoft/winget-pkgs` fork. Without it the job warns
and stable publication still succeeds.
The owner starts publication manually; environment reviewers may be added later.

`packaging/aur/known_hosts` pins AUR's Ed25519 host key, independently verified
against the [official AUR homepage](https://aur.archlinux.org). SSH uses strict
host checking, batch mode and only the dedicated identity. Rotate a host key only
after verifying a newly announced official fingerprint. Rotate the client key by
registering its replacement, updating the environment secret and revoking the old
public key. No password, private key or account session is stored in the repository.

### Release maintenance

For each new bundle, increase the product version, deliberately run a fresh
candidate, review its exact source/hash/library provenance, then publish that run.
Verify public GitHub downloads and all three AUR packages, including a fresh yay
installation, user setup and a process-preserving upgrade. Monitor AUR comments
and rolling-library changes. New stable candidates select macOS checks by default.

An authenticated release listing can find GitHub drafts that the tag endpoint
returns as 404. The publisher checks all pages for the exact tag and rejects
ambiguous matches and API errors before creating or resuming a draft.
After creating a draft, it retries read-only discovery for up to sixty seconds of
backoff to handle delayed GitHub visibility. It creates no duplicate draft and
does not retry API permission errors. A persistent failure retains the same
candidate for a publisher-only retry.

Signing with an owner-controlled identity and appointing a backup maintainer are
optional follow-ups. Checksums and exact run/commit provenance are provided;
package/tag signing is not configured.

## Validation record

The earlier runs below record platform/package validation before the temporary
manual-only budget policy. That budget change was checked locally with actionlint,
source checks and all eight suite selections, including 104 success/failure/skip
gate scenarios. The automatic policy enabled on 2026-10-09 supersedes it; its first
public nightly validation is recorded at the end of this section.

On **2026-10-08**, the complete
[main CI run](https://github.com/ufna/zerus/actions/runs/37705887611) passed every
job at commit `548469a`, including both CLI platforms, both desktop platforms,
Rust 1.85 and the installed Arch bundle check. Linux CLI validation covered 135
Rust unit tests, 202 terminal smoke checks and 398 Python cases across 33 modules;
native authenticated tests remain opt-in. Desktop coverage includes 30 Linux
and 32 macOS CTest suites. The corrected account fixture also passed ten
consecutive runs on each platform.

The artifact-only
[release candidate run](https://github.com/ufna/zerus/actions/runs/37705888076)
also passed its full CI and rebuilt the stable source and binary packages with
makepkg. Downloaded results are available locally under the ignored
`artifacts/review-candidate/` directory and in the `zerus-0.36.2-candidate`
Actions artifact (fourteen-day retention):

- Both pacman packages are approximately 5.4 MiB. All four archive/package hashes
  pass `sha256sum -c SHA256SUMS`.
- All three `.SRCINFO` files are present and exactly match `makepkg --printsrcinfo`.
- The binary manifest pins `548469a20972ec24594130efdff2d4d35ba0c768`; versions are
  GUI/product 0.36.2 and CLI 1.46.1. The package contains 59 dependency notices.
- The source archive contains 433 entries and excludes Beads, agent instructions,
  workflow files and design prototypes while retaining runtime artwork/notices.
- The downloaded binary package was actually installed in a disposable Arch
  container. System binaries, repeated `zerus-setup`, compatibility links and an
  isolated terminal session in an ordinary non-Git folder passed.
- namcap reported no errors. Warnings about dynamic dependencies/interpreters
  remain in the separate `release-packaging-548469a20972ec24594130efdff2d4d35ba0c768`
  log artifact and local `artifacts/review-release-logs/`.

Production packaging uses `BUILD_TESTING=OFF`. Migration helper tests cover
idempotence, conflict preservation, dangling links, missing targets and root
refusal. Removing `qt6-svg` makes the actual resource-render test fail; restoring
it passes. The final handoff/tracker documentation may follow the pinned candidate
commit; regenerate the exact candidate after changing release source or URLs.
Those preparation checks did not establish public AUR installation. The first
public installation and deployment validation is recorded below.

On **2026-10-08**, the publication changes were validated locally with actionlint,
source checks, generated makepkg metadata and credential-free contracts covering
provenance, archive safety, library floors, immutable draft/public retries, version
downgrades and GitHub/AUR ordering. No hosted workflow was dispatched.

The **0.37.0 local review candidate** pins commit `11dff8f` and is available under
the ignored `artifacts/review-candidate-0.37.0-11dff8f/` directory. A clean isolated
checkout built the real `zerus-git` production package, then assembled the exact
source/binary archives and rebuilt `zerus` and `zerus-ade-bin` with makepkg. Rust's
135 release unit tests passed; all three package layouts/executable versions and
59 dependency notices passed validation. namcap reported no errors; dynamic-tool
warnings remain in the logs. Both stable packages are about 5.4 MiB. All six sealed
release assets passed SHA-256 verification and all three `.SRCINFO` files exactly
match makepkg output. Library floors were checked against the real `.BUILDINFO`.
This local packaging review does not replace the full exact-commit release checks.
Regenerate the hosted candidate from the selected final `main` commit before
public publication; later documentation/tracker changes have a different SHA.

### First public release: 0.37.0

On **2026-10-08**, the owner explicitly requested the latest build, required
checks and deployment. The [release candidate run](https://github.com/ufna/zerus/actions/runs/37830899221)
pins `998bc4e9565a645e59b26bec3e2ced187e3d1444` and passed quick/minimum-Rust,
full Linux and Arch checks. macOS hosted jobs were skipped. Linux covered 145
Rust unit tests, 212 terminal smoke checks and 427 Python cases in 35 modules;
24 authenticated/native opt-in cases were skipped. All 32 Arch CTest suites
passed. VCS and both stable packages passed layout/version/notice checks and
namcap with no errors; known dynamic-tool/plugin warnings remain in the logs.

Independent candidate review verified GitHub's artifact digest, all six sealed
asset hashes, 466 source-export entries, binary embedded source provenance and
all three canonical makepkg metadata pairs. Binary floors record Qt base/SVG
6.12.0, WebEngine 6.11.2 and KDE Frameworks 6.30.0, plus the builder's C/C++/glibc
versions. Both stable packages include 59 Cargo dependency notices and the
vendored libvterm/md4c notices.

The [successful publication run](https://github.com/ufna/zerus/actions/runs/37833745339)
resumed the same candidate after correcting authenticated draft discovery. It
published [v0.37.0](https://github.com/ufna/zerus/releases/tag/v0.37.0), verified
all seven download assets anonymously and synchronized the three AUR recipes.
The release tag and archive hashes remain unchanged. AUR HTML pages became
visible before RPC metadata refreshed; yay lookup succeeded after that refresh.

A disposable, fully updated Arch environment installed **all three flavors through
yay**: binary/stable source `0.37.0-1` and VCS `0.37.0.r60.ga2fa919-1`. Installed
binary checks, idempotent user setup and ordinary non-Git terminal sessions passed.
An actual X11 desktop GUI, its isolated tmux server and terminal process survived
binary package reinstallation and subsequent source/VCS flavor replacement.
No user services were enabled by package installation or setup.

Existing Linux and Mac source installations were also updated. Linux binaries
were built from the reviewed source against its installed Qt 6.11 libraries. Mac native validation covered 143 Rust tests and 34 Qt suites; the
terminal suite was rerun after correcting a test assumption about rounded native
font metrics. Runtime code was unchanged by that correction. The Mac bundle was
verified with its existing ad-hoc signing procedure. Both managed GUIs activated
Sessions after claiming their sockets; existing persistent process identities and
state access were preserved. These local Mac checks used no GitHub macOS runner.
Authenticated model-turn tests remain opt-in and were not run for this release.

### First public nightly

On **2026-10-09**, automatic [push CI](https://github.com/ufna/zerus/actions/runs/37848718266)
and the first [Nightly build](https://github.com/ufna/zerus/actions/runs/37848718703)
passed full source/Rust 1.85, Linux CLI, Arch desktop/package and both macOS suites
from exact commit `9eb3a0e3867b42ef82c758781f1b6a23ac848978`. Nightly assembly
reused the tested Arch binary, checked the generated package with namcap and
verified its installed binaries, user setup and isolated tmux session.

The [successful publisher](https://github.com/ufna/zerus/actions/runs/37850588572)
resumed that same candidate after the initial publisher encountered delayed draft
visibility. Bounded read-only draft discovery now handles that condition, covered
by regression tests for transient absence, timeout and immediate API rejection.
All 27 stable/nightly publication contracts pass locally.

[nightly-37848718703](https://github.com/ufna/zerus/releases/tag/nightly-37848718703)
contains five immutable assets and is a prerelease; the latest stable release
remains `v0.37.0`. A fresh, fully updated isolated Arch environment installed
`zerus-ade-nightly-bin 0.37.0.r77.g9eb3a0e.n1-1` through the real yay helper.
Public archive checksums, runtime dependencies, idempotent `zerus-setup` and an
ordinary non-Git terminal session passed. No host agent or tmux process was touched.

The next [Nightly run](https://github.com/ufna/zerus/actions/runs/37851516418) passed
all suites and installed-package checks at
`fef1a00f0e505dd02d9aa0ad94c655ef6d954a1a`. Its [publisher](https://github.com/ufna/zerus/actions/runs/37852775356)
was triggered automatically by successful completion, created and anonymously
verified [nightly-37851516418](https://github.com/ufna/zerus/releases/tag/nightly-37851516418),
and synchronized AUR `0.37.0.r78.gfef1a00.n2-1` without a manual publisher retry.
After AUR metadata refreshed, ordinary `yay -Syu` detected and installed the new
version. The upgrade retained an existing isolated tmux server, terminal process,
session identity and user links. Installed-package checks passed again; all files
expected under the container's pacman extraction policy were present, and the
downloaded archive's build manifest matched the exact verified source commit.

### Stable release 0.41.0 and Android 0.1.29

On **2026-10-11**, the owner requested a new release from main and updates to the
installed clients, including macOS. The [release candidate](https://github.com/ufna/zerus/actions/runs/38160427346)
pins `39018336bfed70d57af148ecb0310c08384ed385` and passed the complete
Linux/Arch/macOS matrix, including the pinned keyless native Codex contracts,
relay PostgreSQL/memory qualification and both stable package rebuilds. Review
verified the Actions artifact digest, source contents against `git archive`,
embedded binary provenance, library floors, dependency notices and sealed assets.
The [protected publisher](https://github.com/ufna/zerus/actions/runs/38162077178)
published [v0.41.0](https://github.com/ufna/zerus/releases/tag/v0.41.0) as the latest
stable release. All seven immutable asset digests and the three anonymous AUR Git
recipes match the candidate; the VCS recipe retains its seed for version-only changes.

Existing Linux and macOS source installations now run **Zerus 0.41.0 / hgs
1.50.0** from that exact revision. On both machines, focused composer,
question-form and update tests passed, installed/running artifact hashes match
the qualified builds, and the managed GUI claimed its socket before activation.
Updates preserved native process identities, existing settings and drafts, with
private rollback backups. The Mac bundle uses the existing ad-hoc source-install
signature; this release does not provide a notarized public macOS installer.
Background draft persistence is installed, but full input responsiveness under
general macOS computer load still requires workload verification.

[Android 0.1.29](https://github.com/ufna/zerus/releases/tag/android-dev-0.1.29)
(version code **30**) uses the same source, package and published signing
certificate. Firebase-enabled Debug and Release each passed 597 unit tests;
release lint passed. Its exact minified APK, source archive, metadata and checksums
were verified anonymously after publication, while APK-bound R8 diagnostics remain
private. Scheduled synchronization published the new desktop and Android catalogs;
their anonymous downloads match the sealed candidates. No physical Android device
was connected for installation. The existing Android signer and Firebase inputs
are now provisioned in the protected release environment. Continuous main-build
Dev activation still requires the Actions-aware mirror and its read credential;
the same-app channel selector alone does not activate that pipeline.

### Stable release 0.42.0 and Android 0.1.30

On **2026-10-11**, the owner requested releases for all platforms and updates to the
installed clients. The [release candidate](https://github.com/ufna/zerus/actions/runs/38166908970)
pins `dfc63abd15ae71d90378ec1d962ebd8dbd8dc2a9` and passed the complete
Linux/Arch/macOS matrix and both stable package rebuilds. Review verified the
artifact checksums, the manifest source commit and the source archive file by
file against `git archive` of that commit.

The first two [publisher](https://github.com/ufna/zerus/actions/runs/38168633520)
runs failed before any public write: GitHub answered `403 Resource not accessible
by integration` when the job token created the draft, although it had
`contents: write` (zerus-ktg3). Their AUR step was only the `--dry-run` preflight.
The owner token then created the `v0.42.0` tag at the candidate commit and a draft
holding exactly the candidate assets, with every size and digest verified. The
[third publisher run](https://github.com/ufna/zerus/actions/runs/38168967736)
verified that draft, published [v0.42.0](https://github.com/ufna/zerus/releases/tag/v0.42.0)
as the latest stable release, checked every asset anonymously and synchronized
`zerus` and `zerus-ade-bin`; their AUR Git recipes name `0.42.0-1` and the v0.42.0
assets.

[Android 0.1.30](https://github.com/ufna/zerus/releases/tag/android-dev-0.1.30)
(version code **31**) was built locally from the exact archived source with the
existing signer and Firebase inputs. Debug and Release unit suites each passed 598
tests and release lint passed. The signing certificate matches 0.1.29, the source
archive passed Gitleaks 8.30.1, and the published assets match the sealed candidate
anonymously; the R8 mapping stays private. The public v1 Android dev feed serves
the same APK digest, and the v1 desktop stable feed serves the v0.42.0 package and
archive digests. No Android device was connected.

Existing Linux and macOS source installations run **Zerus 0.42.0 / hgs 1.51.0**
from that commit. Native agent and tmux processes were unchanged; only the GUI and
hgs helper services restarted.

### Shared Dev channel activation

On **2026-10-11**, [main CI 38170405374](https://github.com/ufna/zerus/actions/runs/38170405374)
passed the full Linux/Arch/macOS matrix and sealed desktop and Android Dev builds
from `196d2817ebb9efbe4521d55d9a2e91702dbffa48`. The lightweight `dev` tag advanced
to that source. [Protected publisher 38172339849](https://github.com/ufna/zerus/actions/runs/38172339849)
published this same candidate through the existing mirror after the Actions archive
media-type fix in `d45e8d2`; no candidate was rebuilt or GitHub Release created.
Both anonymous [desktop Dev](https://zerus.dev/updates/v2/desktop/dev.json) and
[Android Dev](https://zerus.dev/updates/v2/android/dev.json) catalogs, every payload
size/digest and exact Git source archive were verified. Android **0.1.30-dev.201 /
code 10201** preserves `app.zerus.mobile` and the original signing certificate.

`DEV_UPDATES_ENABLED=true` now enables successful-main publication. The existing
protected Android inputs and restricted SSH transport are provisioned; each
publisher lends only its repository-scoped Actions/Contents-read job token to the
existing unprivileged mirror. There is no stored personal token or second public
host. The original three mirror timers and legacy v1 catalogs remain in use;
GitHub latest remains `v0.42.0`. A fresh isolated desktop controller discovered the
real public Dev feed, emitted one verified notice, persisted its channel choice
and suppressed a duplicate check. Retention keeps three Dev builds per platform,
a 72-hour retirement grace and the shared 20 GiB limit. Desktop installation stays
explicit. Mac Dev now uses the same candidate/mirror with signed ZIP and DMG
assets and `zerus@dev`; see [Mac publication](macos-distribution.md#publication).
