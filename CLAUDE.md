# Agent instructions

Zerus provides a desktop UI and the `hgs` CLI for persistent local and remote
agent sessions. The CLI lives in `src/`; the Qt GUI lives in `tray/src/`.
The Rust mobile relay lives in `services/mobile/relay/`; the outbound Python
connector remains in `services/mobile/zerus_mobile/`. Keep API v1 and durable
request identities compatible; see `services/mobile/relay/README.md` for relay
contracts, isolated PostgreSQL tests and container memory qualification.
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
Test tmux operations with an existing isolated socket directory.
Do not hot-reload a DeepSeek bridge that owns live agents: its disposal closes
them. Use the native API compatibility path for resident adapters and keep
unconfirmed permission setup blocked until an explicit resume verifies it.
Ordinary non-Git folders are supported; never initialize Git automatically.
New session defaults to a generated worktree for a verified Git folder, created
only after the user selects Start session. Inspecting or opening a dialog must
never create a worktree. Keep direct-folder launch and explicit custom/existing
worktree choices available. Destructive UI actions require explicit confirmation.

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

Hosted CI runs the complete Linux/Arch/macOS/Windows matrix automatically on
pushes to main and pull requests targeting main. Manual checks remain available. Public
standard GitHub runners are free; do not introduce paid larger runners or raise
the 10 GiB cache limit without owner authorization. Authenticated model tests stay
opt-in. Validate workflow edits locally before pushing. See `docs/ci-and-aur.md`.
Native terminal/lifecycle changes need isolated real-agent contract coverage for
reviewed versions, including retained resident versions. Full Linux CI runs the
keyless pinned Codex matrix; the separate upstream canary reports new releases
and protocol changes without advancing pins or updating live agents. Review
capabilities before adopting them; a skipped native suite is not compatibility
evidence. See the native compatibility section in `docs/ci-and-aur.md`.
External fork workflows require maintainer approval for every external contributor.
PR/manual checks inherit read-only cache tokens; only exact upstream main pushes
and scheduled checks may write caches. Publish jobs have no cache access. Keep
action SHA pins enforced and never execute PR code with publication credentials
or on development-machine runners. Review workflow changes before approving runs.
Dependency proposals are monthly and grouped, with one ordinary PR per ecosystem.
Cargo proposals update only the lockfile within reviewed manifest constraints;
review manifest migrations explicitly and retain Rust 1.85 compatibility.
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
assets. Pure VCS version bumps do not create AUR commits. Nightly prereleases also
carry the tested Windows x86_64 portable zip unchanged; it stays GitHub-only (not
mirrored or in the v1 feed). Stable releases and Dev builds have no Windows asset.
The Homebrew casks `zerus` (stable) and `zerus@nightly` (tap `ufna/homebrew-zerus`)
ship the signed, notarized Apple Silicon `Zerus.app` from the same stable or nightly
candidate once `MACOS_CASKS_ENABLED` is set. Apple credentials stay in the main-only
`macos-signing` environment and the tap deploy key in `release`. Casks never quit
the GUI, unload LaunchAgents or write to user homes; `zerus-setup` stays explicit.
See `docs/macos-distribution.md`.

Run checks appropriate to the change; see `CONTRIBUTING.md`. Update Beads status
and export with `bd export -o .beads/issues.jsonl` before staging tracker changes.
Inspect `git status` and the staged diff. Commit, push and synchronize only when
explicitly authorized; the default workflow is conservative.
Mirror substantive instruction changes in `AGENTS.md` and `CLAUDE.md`.
