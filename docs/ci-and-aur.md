# CI and Arch Linux publication

This document describes the checks, package layout and publication workflow.
[Zerus 0.37.0](https://github.com/ufna/zerus/releases/tag/v0.37.0) is the first
public Arch x86_64 bundle. Stable release builds and GitHub/AUR publication remain
separate manual workflows; verified nightlies publish automatically.

## Continuous integration

Hosted checks run automatically on pushes to `main` and pull requests targeting
`main`, including Dependabot proposals. External fork workflows wait for a
maintainer's **Approve and run** decision. Every automatic run selects source/MSRV,
full Linux CLI, Arch desktop/package and both macOS suites. The owner enabled
this policy on 2026-10-09 after making the repository public: standard Ubuntu and
macOS GitHub-hosted runners are free for public repositories.

Manual CI remains available and defaults to all suites. `ci.yml` chooses the cache
trust boundary and calls the shared jobs in `checks.yml`. Reusable CI keeps explicit
suite selection so another workflow cannot accidentally request unrelated jobs.
Every job checks out the exact commit resolved by quick checks. PR checks use the
GitHub merge commit; a manual `ref` can select a PR head or another exact revision.

| Job | Automatic CI | Checks |
| --- | --- | --- |
| Quick checks | Always | Privacy, Python/Bash syntax, version consistency, actionlint, stable/nightly publication contracts, locked Rust 1.85 unit tests |
| Linux CLI | Always | Stable Rust unit tests, terminal smoke tests, Python integration modules, release build |
| Arch desktop/package | Always | Qt build and all CTest suites, production package, namcap, staged and installed bundle checks |
| macOS CLI and desktop | Always | Native CLI integration and all Qt CTest suites on two standard macOS jobs |
| CI gate | Always | Requires every selected suite to succeed; fails on unexpected skips, cancellation and errors |

The full jobs start after quick checks. New CI runs cancel obsolete runs for the
same branch/PR and workflow. PR jobs have read-only permissions and no publication
credentials; fork PRs use `pull_request`, never `pull_request_target`. Publishing
is confined to trusted upstream `main` and the branch-restricted `release`
environment. Authenticated model-turn tests remain explicit opt-ins.

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

### Manual checks

```sh
# Full Linux, Arch and macOS coverage:
gh workflow run ci.yml --ref main
# Full coverage of a PR head with the current upstream workflow:
gh workflow run ci.yml --ref main -f ref=refs/pull/123/head
# Source/MSRV checks plus the aggregate gate only:
gh workflow run ci.yml --ref main -f run_linux=false -f run_arch=false -f run_macos=false
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
Current release packaging supports **Arch x86_64 only**. The macOS runner checks
build/test compatibility; it does not create a notarized macOS installer. Windows
and Arch ARM are outside this first package matrix.

All CI jobs have bounded timeouts. Checkout credentials are not persisted and
Actions are pinned to commit SHAs. Dependabot proposes monthly grouped action/Cargo
updates. Rust/C++ caches reduce repeated build time; their repository limit stays
at 10 GiB. Ordinary test artifacts expire after seven days and stable candidates
after fourteen. Scheduled logs, Arch packages and nightly candidates expire after
three days.
Standard Ubuntu/macOS runners and public artifacts are free while the repository
is public. Larger runners and explicitly increased cache storage have separate
billing rules. No self-hosted runner is attached to a development machine.
[Actions billing](https://docs.github.com/en/billing/concepts/product-billing/github-actions).

The Arch base image is digest-pinned, but `pacman -Syu` intentionally uses current
Arch packages. This tests rolling-distribution compatibility; it does not promise
bit-for-bit reproducibility months later. Rust dependencies are locked, fetched
in `prepare()` and built/tested with `--frozen` inside source packages. The CI
image, tmux and official DeepSeek fixture pins need periodic maintainer review.

### Dependency updates and cost

Dependabot checks version updates on the first day of each month. Each ecosystem
has one version-update group and at most one open version-update PR: two ordinary
PRs across GitHub Actions and Cargo. Automatic rebasing is disabled; refresh a
stale proposal before testing and merging it. Updates still require review and
checks appropriate to the active CI policy; there is no automatic merge.

Cargo proposals update only `Cargo.lock` within the reviewed `Cargo.toml`
constraints. This also avoids unreviewed breaking minor upgrades of `0.x` crates.
Manifest migrations are deliberate maintenance work, including Rust 1.85 and
existing state/configuration compatibility checks. Actions keep their exact
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
cargo +1.85.0 test --locked
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

`tests/test_claude_input_native.py` checks exact message acceptance by the real
Claude TUI with a private home/socket and a localhost fake API. Set
`HGS_CLAUDE_TEST_BIN` to a reviewed executable to run it; it never reads a user's
account. Claude 2.1.296 coverage verifies literal trailing backslashes, multiline
messages and durable request replay. Terminal submission alone is insufficient:
the test waits for the unchanged user message in native history.

### Native agent compatibility and upstream review

Full Linux CLI CI also runs `python3 scripts/ci/agent-compatibility.py`. The
reviewed Codex matrix in `scripts/ci/agent-compatibility.json` retains 0.160.1
(a resident-session regression) and 0.162.0. Each official Linux x86_64 package
has an exact version and SHA-512 integrity pin. Only its native executable is
extracted into a disposable directory; agent binaries are neither committed
nor bundled. The runner treats skipped or empty native suites as failures.

Real native checks run at 95x47 and 110x35 terminal sizes with animations explicitly
enabled and disabled. Animated 95x47 checks require the actual released settled
idle logo before inspecting a Terminal clear; unknown or partial logo frames do
not confirm a reset. Assertion failures retain the synthetic native panel,
cursor and lifecycle state. The tests resume synthetic
history, then verify GUI-command clear, Terminal clear, the deferred native
identity transition, stale usage suppression, a single durable clear boundary,
preserved process/run identity, unsent native drafts and foreign-directory
rejection. A separate fixture caches a newer version for the startup update
picker and verifies its Activity identity, **Skip** and **Skip until next
version**; it never selects **Update now**. Nested real `codex exec --ephemeral`
reviews verify separate child Activity under Claude and Codex parents, preserved
parent identity/model/history and the child's recorded result without a rollout.
Every fixture owns a private home
and tmux socket and uses only a localhost provider. No authenticated inference
or existing session is involved.
Fixture shutdown waits for its owned panes to exit before deleting the private
home, so a shell's final history write cannot race with filesystem cleanup.

The separate **Agent upstream compatibility** workflow runs daily at 04:41 UTC
and is also available manually on upstream main. It discovers published Codex,
Claude Code, Kimi Code and DeepSeek Harness versions from their official package
indexes, links their official release notes and tests the latest Codex as an
additional canary. It does not advance reviewed pins or install anything on
development machines. Its hosted job has read-only repository access, no cache
access and no publication credentials. Reports, native logs and public protocol
schemas expire after three days. A discovery or native-contract failure fails
the run; successful unittest skips do not hide missing coverage.

Codex protocol schemas are generated separately with and without experimental
APIs. The report compares the latest canary with the newest reviewed pin and
lists added/removed methods and added/removed/changed schemas. Changes to CLI help
appear with a generated diff. A detected addition is a review candidate, not
evidence that Zerus supports it or that a signed-in
account can use it. For Claude and Kimi this workflow currently discovers
versions only; their authenticated native suites remain opt-in. DeepSeek's
existing pinned keyless contract fixture runs in full Linux CLI CI.

To run the reviewed matrix or inspect current upstream changes locally:

```sh
cargo build --locked
python3 scripts/ci/agent-compatibility.py
python3 scripts/ci/agent-compatibility.py --latest
```

The generated `artifacts/test-results/agent-compatibility/summary.md` and
`report.json` contain the coverage result and review candidates. Before changing
a pin, read the provider's release notes and compare CLI flags, hook events,
native history and protocol schemas. Classify each relevant feature as already
supported, requiring an adapter change, or requiring a separately reviewed
integration; track implementation work in Beads. Add an isolated native contract
for every adopted lifecycle/input change and retain versions still used by
resident sessions until their compatibility boundary is deliberately reviewed.

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
separate, reviewed baseline cleanup. Logs are under ignored
`artifacts/test-results/`; packaging outputs are under ignored `artifacts/` and
`dist/`.

## Automatic nightly publication

`nightly.yml` runs daily at 00:17 UTC (03:17 Europe/Moscow) and supports manual
launches. It verifies one exact upstream `main` commit with the complete Linux,
Arch and macOS CI matrix, then repackages its already tested Arch binary. It
checks the generated pacman package, namcap output and installed bundle in an
isolated home. There is no second Rust/C++ compilation during nightly assembly.

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
Binary library floors come from the verified source package's `.BUILDINFO`.
Stable and nightly publishers share a serialization group, preventing AUR races.

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
published build to produce different bytes under its existing tag. Artifacts are
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
| Desktop runtime | `qt6-base`, `qt6-webengine`, `qt6-svg`, `kstatusnotifieritem`, `kwindowsystem`, `kidletime`, `hicolor-icon-theme` | Widgets, embedded web UI, SVG plugin, tray/window integration, idle detection and icons |
| CLI/runtime tools | `tmux>=3.7`, `openssh`, `python`, `curl`, `procps-ng`, `bash`, `tar` | Persistent sessions, remote access, compatibility helper, account HTTP, process inspection, installers and remote source transfer |
| Native library runtime | `glibc`, `libgcc`, `libstdc++` | Dynamically linked CLI/desktop binaries |
| Source build only | `rust`, `cmake`, `ninja`; additionally `git` for `zerus-git` | Locked Rust and production Qt builds; Git source/version discovery |
| Standard build environment | `base-devel` | Required by Arch's makepkg/AUR build convention; its tools are not repeated in makedepends |
| Optional features | `nodejs`, `npm`, `git`, `konsole`, `wl-clipboard`, `xclip`, `systemd` | Native agent installation/runtime, worktrees, external terminals, clipboard and user services/scopes |

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
  the explicit `zerus-setup` migration helper.
- A desktop entry, existing application icons and system-provided **user** unit
  definitions for `hgs-tray`, `hgs-swarm` and `hgs-recovery`.
- Documentation and a sample tmux configuration under `/usr/share/doc/zerus`.
- Zerus MIT, vendored libvterm MIT and the exact host-target Cargo dependency
  license texts under `/usr/share/licenses/<package-name>`.

Qt, Qt WebEngine and KDE libraries are dynamically linked system dependencies.
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
and `tray/CMakeLists.txt` are **0.41.0**. The independently versioned compatible
CLI is **1.50.0**. CI rejects a product/desktop version mismatch; packaging
records both versions in `build-info.json`, with the source commit when built
from Git. For source-archive builds, the release manifest and archive SHA-256
provide the commit provenance.
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

Packaged units and the current source installer template use `KillMode=process`.
A GUI restart must preserve native agents, tmux servers and DeepSeek hosts. Older
source-installed units
may still use systemd's control-group default. Before restarting one, add a user
drop-in with `[Service]` and `KillMode=process`, run `systemctl --user daemon-reload`
and verify `systemctl --user show hgs-tray.service -p KillMode`. Package upgrades
do not perform a restart. Review the old unit's kill policy before restarting an existing
installation; never use a broad process kill or restart all native clients.

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

`release.yml` runs quick checks plus full Linux CLI, Arch and macOS validation from
one exact commit. macOS can be explicitly disabled. It consumes that run's
Arch package, verifies its embedded version/source commit, assembles source and
binary archives, rebuilds both stable recipes, runs namcap and seals the result.
Stable and nightly candidates call the common `checks.yml` workflow directly
with read-only cache access. Their verification never includes the main-push dev
publisher or requests its tag-writing permissions.

```sh
# Only when the owner requests a hosted build:
gh workflow run release.yml --ref main -f version=0.41.0
# After this manual run succeeds:
gh run download RUN_ID --name zerus-0.41.0-candidate --dir /tmp/zerus-candidate
cd /tmp/zerus-candidate
sha256sum -c SHA256SUMS
```

The artifact contains source/binary archives, two checked pacman packages,
`release-info.json`, all three concrete `PKGBUILD`/`.SRCINFO` pairs, their sealed
`zerus-VERSION-aur.tar.gz` archive and `SHA256SUMS` covering every release asset.
Git export attributes omit tracker, agent instructions/skills, workflows and
prototype artwork from the source archive; runtime artwork and notices remain.

Review the source commit, package logs, dependency/license inventory and recipes.
`create_draft=true` additionally creates a GitHub draft; by default everything
remains in the candidate artifact. Candidate artifacts expire after fourteen days,
logs after seven. Old 0.36.2 artifacts predate the new names/schema and cannot be
published by the new workflow. Never substitute a package built from another SHA.

Equivalent local assembly on Arch, with a clean committed checkout and its exact
verified package, remains available:

```sh
python3 scripts/prepare-release.py --version "$(cat VERSION)" \
  --package artifacts/zerus-git-EXACT_VERSION-x86_64.pkg.tar.zst --output dist
bash scripts/check-release-packages.sh dist
```

The helper requires an empty output directory. Build artifacts are ignored and
are never committed to the application or AUR repositories.

### 2. Publish the reviewed candidate and update AUR

`publish.yml` takes the **successful candidate run ID** and its version. It uses
one Ubuntu runner, does no compilation and allocates no Arch/macOS runners:

```sh
# Only after review and an explicit owner publication request:
gh workflow run publish.yml --ref main \
  -f version=0.41.0 -f candidate_run=RUN_ID
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
`PKGBUILD` and `.SRCINFO`; release archives stay on GitHub.

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
`actions: read` and `contents: write`; no separate GitHub personal token is needed.
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
