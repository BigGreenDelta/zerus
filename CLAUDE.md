# Agent instructions

Zerus provides a desktop UI and the `hgs` CLI for persistent local and remote
agent sessions. The CLI lives in `src/`; the Qt GUI lives in `tray/src/`.
The Rust mobile relay lives in `services/mobile/relay/` and the Rust outbound
connector in `services/mobile/connector/`; the legacy Python connector remains in
`services/mobile/zerus_mobile/` until every gateway has moved. Keep API v1, durable
request identities and the connector journal compatible; see
`services/mobile/relay/README.md` for relay contracts, isolated PostgreSQL tests
and container memory qualification. Moving a live gateway is owner-approved.
The Android client lives in `mobile/android/`; the native iOS client (Swift
package `ZerusKit` plus SwiftUI app) lives in `mobile/ios/`. Install iOS Dev
builds with `mobile/ios/scripts/dev-install.sh` or `remote-dev-install.sh`; never
commit team IDs, device identifiers or signing material. TestFlight uploads run
only from trusted main; see `docs/ios-distribution.md`.
`design/` contains prototypes and original artwork, not runtime dependencies.

The repository language is English. Write documentation and other project-facing
text in English; conversations may use the user's preferred language.

## Work tracking and memory

Use the `beads` skill in `.agents/skills/beads/SKILL.md` and run `bd prime` at
session start and after compaction. Use `bd` for all durable tasks and blockers.
Use `bd remember --key <key> "<decision>"` for concise, shareable project memory.
Do not create separate TODO or MEMORY files. Read current source before carrying
forward a historical limitation.

The source of truth is the Dolt database inside `.beads/` (currently
`.beads/embeddeddolt/` with the embedded backend). Synchronize with `bd dolt push/pull` over
`refs/dolt/data`; `.beads/issues.jsonl` is a passive export, not a sync protocol.
New clones should run `bd init --non-interactive --prefix zerus`, then `bd prime`.
Never replace an existing database with a JSONL import during normal operation.

If the optional shared-memory helper is installed, recall the project using
`python3 "$HOME/.local/lib/hgdev-memory/memory.py" recall --client codex`.
Use only the returned project wing. Store short verified decisions there; never
upload credentials, private personal data, transcripts or full prompts.
Repository documentation owns specifications; Beads owns tasks and blockers.

## Compatibility and operation

Treat pasted `zerus://session?...` URIs as references to another Zerus conversation.
Read them with `hgs inspect '<reference>'`; use
`hgs @configured-alias inspect '<reference>'` when its local SSH alias differs.
The CLI verifies computer and conversation identities and follows session renames.
Use the resolved name and IDs for any separately authorized operation. See the
session references section in `docs/reference.md`.

When referring to another conversation, include its Zerus session name or copied
reference. Beads IDs such as `zerus-ab12` identify tasks, not conversations; never
present a task number as a session ID.

Keep the `hgs` CLI, `HGS_*` variables, state/config paths and existing application
and service identifiers compatible. A repository rename must not reset accounts,
project catalogs, native agent processes or saved conversations.

Never restart or kill native agents, tmux servers or DeepSeek hosts as part of a
GUI update. Update local installations only with `install.sh`, `tray/install.sh`
and `mac-install.sh`; a running GUI is replaced through `tray/update-installed.py`,
which saves drafts and keeps one rollback copy. Never stage GUI copies in
`~/Applications` or `~/.local/bin` by hand. Another session may be testing its own
build: check the running GUI's `--build-info` and
`~/.local/share/hgs/build-provenance.json`, and coordinate before replacing it.
Source GUI installs from main must set `ZERUS_DEV_BUILD_NUMBER` to the CI sequence
for that exact source. Never replace a numbered Dev installation with an
unnumbered Stable-labelled main build.
Test tmux operations with an existing isolated socket directory.
Do not hot-reload a DeepSeek bridge that owns live agents: its disposal closes
them. Use the native API compatibility path for resident adapters and keep
unconfirmed permission setup blocked until an explicit resume verifies it.
Ordinary non-Git folders are supported; never initialize Git automatically.
New session defaults to a generated worktree for a verified Git folder, created
only after the user selects Start session. Inspecting or opening a dialog must
never create a worktree. Keep direct-folder launch and explicit custom/existing
worktree choices available. Destructive UI actions require explicit confirmation.

An idle agent with confirmed owned live shell or native jobs shows `Background`,
remains in Active and accepts new messages. Native work and explicit attention
take precedence. A native Stop ends a turn; it does not prove those jobs finished.
Retain unread reply identity and defer reply-only badges/notifications until the
work completes. Historical, unknown and offline jobs never establish current work.
Keep this contract aligned across desktop, Android, shared fixtures and docs;
changes to these status semantics need explicit owner direction.

Owner-approved independent-agent waits use `hgs wait`, separately from native
work. Before handing off work whose acceptance or result you will wait for,
declare the wait and give the producer its scoped reply ticket. Acknowledge
acceptance explicitly; deliver the result through the normal guarded send before
finishing the wait with that exact submission receipt. Cancel abandoned waits.
Never infer acknowledgement or completion from prose, Beads status, a native Stop
or peer idleness. Preserve exact waiter identity, native work/attention priority,
bounded previews and paged journals. Check the selected installed CLI's help
marker before using `wait` on an older peer. See `docs/agent-coordination.md`.

Use non-interactive filesystem commands and `ssh/scp -o BatchMode=yes` in automation.
On Windows, reach `ssh.exe` only through `hgs.exe`: Windows OpenSSH hangs after
using a synchronous pipe, so scripts and tests must not pipe it directly.
Use synthetic fixture paths, reserved example domains and local/private configuration
for deployment values. Never commit login material, runtime data or personal audits.
All Beads data on the repository remote may eventually be public, including history.
Git and Beads attribution may use the team's work addresses; the source privacy
guard still rejects addresses elsewhere, including Beads free text. Use a GitHub
no-reply address instead if email privacy matters.

## Validation and handoff

Session counts can reach tens of thousands or more. Histories, audit journals,
logs and administrative views must use bounded pages, indexed queries and
background I/O; do not load complete journals or grow per-session GUI caches
without a bound. Keep pagination stable while new records arrive.

Automatic tool approvals are a separate, dangerous desktop opt-in, off by
default. Grant only a verified one-time native permission, reserve its private
audit record durably before delivery, and retain exact run/conversation/question
identity. Unknown delivery never authorizes replay. Ordinary questions, startup
trust and broader persistent permission changes remain manual.

Hosted Dev CI checks shared Rust/protocol contracts, Linux CLI, Arch, macOS and
Android on main. PRs add Windows/iOS for affected desktop, native or shared paths;
unknown change inventories retain both. Nightly and Stable run the full matrix.
iOS TestFlight Dev uploads follow successful Nightly verification, using the exact
source's main CI sequence. Manual checks remain available. Public
standard GitHub runners are free; do not introduce paid larger runners or raise
the 10 GiB cache limit without owner authorization. Authenticated model tests stay
opt-in. Validate workflow edits locally before pushing. See `docs/ci-and-aur.md`.
Native terminal/lifecycle changes need isolated real-agent contract coverage for
the latest available release of each supported agent. Ordinary CI resolves one
official version and package integrity per agent before native execution; it
never adds historical versions or falls back to them on discovery failure.
Keep terminal sizes, animation modes and native method coverage. The separate
upstream report captures current releases and capabilities without updating live agents. Review
capabilities before adopting them; a skipped native suite is not compatibility
evidence. See the native compatibility section in `docs/ci-and-aur.md`.
External fork workflows require maintainer approval for every external contributor.
PR/manual checks inherit read-only cache tokens; only exact upstream main pushes
and scheduled checks may write caches. Publish jobs have no cache access. Keep
action SHA pins enforced and never execute PR code with publication credentials
or on development-machine runners. Review workflow changes before approving runs.
Dependency proposals are monthly and grouped, with one ordinary PR per ecosystem.
Cargo proposals update only the lockfile within reviewed manifest constraints;
review manifest migrations explicitly. Zerus tracks the newest stable Rust: raise
`rust-version` and the pinned CI toolchain to each stable release.
Keep security alerts and security updates enabled separately. Never auto-merge.
AUR packages are `zerus` (stable source), `zerus-git` (upstream main),
`zerus-ade-bin` (stable binary) and `zerus-ade-nightly-bin` (verified nightly binary),
with x86_64 support. Stable fixes increment patch; Dev builds use the next planned
stable base from `DEV_VERSION` (Android: `mobile/android/DEV_VERSION`) plus the
growing verified CI sequence, `next-version-dev.N`. Keep installed build identity
independent of selected channels and retain all published asset versions. See the
version rule in `docs/public-distribution.md`.
Stable publication remains owner-triggered. Daily nightlies
publish automatically only after full CI and package checks succeed. Publication
credentials remain confined to trusted main in the release environment, never PR
code. Nightly tags/assets are immutable and never become the latest stable release.
Retry publishers with the same verified candidate; never rebuild or overwrite its
assets. Pure VCS version bumps do not create AUR commits. Stable releases after
0.42.2 and nightly prereleases also carry the tested Windows x86_64 portable zip
unchanged; it stays GitHub-only (not mirrored or in the v1 feed). Dev builds have
no Windows asset. `publish.yml` proposes each stable zip to winget as `ufna.Zerus`;
its manifest relies on the zip's `zerus-windows-x86_64/hgs-tray.exe` layout.
The Homebrew casks `zerus` (stable), `zerus@nightly` and `zerus@dev` (tap
`ufna/homebrew-zerus`) ship signed, notarized Apple Silicon `Zerus.app` once
`MACOS_CASKS_ENABLED` is set. Dev also requires `DEV_UPDATES_ENABLED`, follows
successful main CI and uses the existing bounded mirror; it creates no GitHub
Release per push. Publish its cask only after verifying the exact public catalog
and signed ZIP/DMG hashes against that CI candidate. Apple credentials stay in the main-only
`macos-signing` environment and the tap deploy key in `release`. Casks never quit
the GUI, unload LaunchAgents or write to user homes; `zerus-setup` stays explicit.
See `docs/macos-distribution.md`.

Run checks appropriate to the change; see `CONTRIBUTING.md`. Update Beads status
and export with `bd export -o .beads/issues.jsonl` before staging tracker changes.
Inspect `git status` and the staged diff. Commit, push and synchronize only when
explicitly authorized; the default workflow is conservative.
Mirror substantive instruction changes in `AGENTS.md` and `CLAUDE.md`.
