# Zerus reference

[Product overview](../README.md) | [Installation](installation.md)

This reference covers session lifecycle, desktop workflows and native integration
limits. Run `hgs -h` for the complete command syntax.

## CLI sessions

`hgs` starts a command in a named project tmux session and attaches locally or
through SSH. The session survives a closed terminal, an SSH interruption or a
sleeping client while its host remains running. Terminal windows, tabs and splits
belong to your terminal emulator; tmux provides the persistent session.

```sh
hgs claude sample-project           # attach if present; otherwise start Claude
hgs @mac claude sample-project      # the same operation over SSH
hgs ls                             # local and peer sessions
hgs status                         # version, sessions and services here and on peers
hgs version                        # this hgs version (also hgs --version)
hgs a claude/sample-project/review  # attach by full name
hgs kill claude/sample-project      # remove the hgs binding; keep native history
hgs pause --all                     # pause verified idle sessions
hgs resume --all                    # restore saved sessions in the background
hgs project ls                     # legacy local folder aliases
hgs claude sample-project -c        # continue the latest conversation in the folder
hgs --dry-run claude sample-project # show the tmux/SSH command
```

Pass an absolute folder path when no alias is configured. `--new` generates a
fresh session name; `--new -n review` uses your label and rejects an occupied name.
Arguments after `--` go to the agent or project wrapper. Arbitrary terminal commands
can run through `hgs`; native lifecycle and Activity functions require an adapter.

### Session references

**Copy session reference** in the desktop session context menu copies a
`zerus://session?...` URI. Android offers the same action in the
session toolbar and menu. It contains the stable computer UUID, a native machine
name for routing, the session name and exact conversation identity; child and
archive references include their explicit identities. It contains no credentials.

Paste the reference into another conversation to identify its source. An agent
can run `hgs inspect '<reference>'` to read its Activity and recorded messages.
The CLI verifies the computer UUID, follows a renamed session by its conversation
ID, and rejects cleared, forgotten or ambiguous bindings. A conversation reference
survives resume into a new run; an unconfirmed first-message reference requires
the original run. A configured peer matching the machine name routes through SSH;
use `hgs @configured-alias inspect '<reference>'` when its alias differs. A URI
alone never launches a session or sends input.

`hgs [@host] reference <session> --json [--archive ID] [--agent ID]` returns the
same URI and readable identity fields. Optional `--run ID --conversation ID`
guards ensure a copy action still addresses the selected conversation.

An agent obtains its own reference with `hgs self [--json]`. The caller must run
inside a live tracked native agent: like an agent-originated send, the binding is
proven through that agent's process ancestry, never through `HGS_SESSION` alone.
The reply carries the current name after a rename, the `reference` fields and
`provider`. It never routes to a peer and refuses `--dry-run`.

`hgs [@host] guide [send|wait|launch|stop]` prints compact instructions for
agents that read, message, wait for, launch or stop other sessions. The text is
compiled into hgs and stays small because agents read it into their context;
topics are read only when needed. `@host` returns that peer's guide after
probing its installed help, so an older peer fails without launching anything.

Native conversation IDs appear only in **Details** on desktop and **Session
details → Overview** on Android, labeled **Session ID** with a dedicated copy
icon immediately after the exact identifier. The desktop icon uses the same
borderless style as header actions. The ID stays on one line whenever the pane
has room. The icon copies the ID as plain text, as does the **Copy session ID**
icon in the desktop session header, which copies without showing the ID;
**Copy session reference** remains in the desktop session context menu and copies
the portable URI. Successful text copies use the window's status confirmation
without a system notification on Linux or macOS. Before that ID is reported,
**Run ID** identifies the current
process run; it can change on resume. Child details also show their agent ID.
Search accepts complete IDs or
parts of conversation, run, archive and reported child IDs; a child match
identifies its parent conversation.
Machine, project and state filters still apply. Desktop ID results show the
matched identifier and expose its complete value in the tooltip.

The desktop session header keeps frequent actions as icons: open the session
folder, open a shell there, **Copy session ID** and **Clear session**. Its **⋯**
menu names the same actions (**Open folder**, **Open shell in session folder**,
**Copy session ID** and **Clear session…**, each available exactly when its icon
is) and also holds **Rename…**, **Fork session…**, **Copy connect command**
(**Open externally** without clipboard mode, or the native DeepSeek UI), **Pause
session** or **Resume session**, **Start fresh…**, **Move to archive** and
**Terminate session…**. A stopped session hides the Clear session icon and offers
**Start fresh…** in the menu; an archived session shows **Restore session** in its
place. The session list context menu offers **Clear session…** for a live session
where a stopped one offers **Start fresh…**. For a session that is not open, it
becomes available once that session's own inspection confirms the conversation
listed for it; choosing it opens the session and asks the same confirmation.

Identifiers such as `zerus-ab12` are Beads task numbers, not session IDs. Desktop
message search finds their public mentions; Android session search finds mentions
in the available session metadata and previews. A task mention alone does not
establish a unique session identity.

### Static agent guidance

The static hint source currently covers tracked Claude Code and Codex main
conversations. Its native delivery contract still requires qualification on the
reviewed versions before adoption. Kimi, OpenCode and DeepSeek channels remain
separate work; older hgs peers inject nothing.

The hint points to `hgs guide` with fixed English text, independent of session
name, computer and hgs version. It uses the existing SessionStart hook context,
without changing user instruction flags, project launchers or saved native
arguments. It is not sent as a person's Activity message. New conversations and
previously unmarked conversations receive an attempt; repeated resumes suppress
another attempt. Clear and compaction replace context and request a new copy.
A fork inherits suppression only from the exact marked context it forks.
Renaming does not change the text or hint revision. Arbitrary commands and native
subagents receive no hint.

Configure optional injection in the hgs config file or caller environment:

```sh
HGS_AGENT_HINT=0                         # disable new hint injection globally
HGS_AGENT_HINT_OFF="native-codex work"   # exclude exact account IDs
```

Injection defaults to enabled. The global value accepts `0` or `1`; the exclusion
list is whitespace-separated and uses `native-claude` or `native-codex` for the
provider's own login. Environment values take precedence, including an explicitly
empty exclusion list. Invalid or unreadable hint policy disables only optional
injection with a bounded diagnostic; unrelated configuration validation is
unchanged. `hgs guide` and `hgs self` remain available when injection is disabled.
Opt-out does not remove context already retained by the provider.

Each fresh start, resume, recovery, archive restore and fork resolves the current
caller's effective hint policy again. Only those effective hint values enter the
new process environment, after login-shell startup on Unix. The hgs config path
is unchanged and policy is not stored in the saved launch recipe or binding.
This is a process snapshot: editing the file does not live-reload a running
native process. An `hgs` invocation inside that agent inherits the snapshot as
ordinary environment overrides, which can continue to shadow later file edits.
On Windows, an absent exclusion variable alongside the resolved global value
means an empty list; the hook does not supplement it from another default file.

The private conversation/revision mark suppresses retries; it does not prove
stdout delivery or native acceptance. An output failure reports unknown delivery
and is not automatically retried. There is no crash-atomic exactly-once promise.

<a id="recovery"></a>

## Pause, resume and reboot

```sh
hgs pause claude/sample-project/review
hgs pause --all
# Reboot the machine running the agents after checking the pause result.
hgs ls
hgs resume claude/sample-project/review
hgs resume --all
hgs a claude/sample-project/review
hgs @mac pause --all
```

Pause preserves an exact native conversation binding. It does not preserve open
network connections, child processes or an unsent native terminal draft. Supported
integrations include Claude Code, Codex, Kimi Code, the official DeepSeek Harness
and OpenCode. DeepSeek and OpenCode use their native engines instead of a
terminal-agent process. OpenCode sessions have no separate process to pause
([OpenCode controls and limits](opencode.md#controls-and-limits)).

For terminal agents, pause verifies the live pane, run and conversation identity,
native history, supported resume arguments and an idle state. It sends SIGTERM only
to the verified agent process, checking its PID and start time, and waits up to ten
seconds. It never kills the tmux server or escalates a failed pause to SIGKILL.
`pause --all` preflights every live session on the host: an untracked agent, busy
session or ordinary shell blocks the operation before the first signal. Once
stopping begins, some sessions can fail independently; inspect the nonzero result
before rebooting.

Restore uses `claude --resume ID`, `codex resume ID`, `kimi --session ID` or the
supported native DeepSeek operation. It preserves the original folder, account
and project wrapper. There is no fallback to the latest conversation or a new
conversation when exact restore fails. `resume -d` and `resume --all` run in the
background; use `hgs a ...` when native startup needs input. A normal launch with a
saved name restores it; `--fresh` explicitly replaces its binding.
`hgs resume SESSION --fresh [-d] [--expected-run-id RUN]` starts a stopped Claude,
Codex or Kimi session with a new conversation from its own folder, account, safe
launch options and model, like `/clear` in a live agent. The replaced conversation
stays in native history and readable in Activity above a **Session cleared**
boundary; a running agent uses **Clear session** instead.

You can also resume native history created outside `hgs`:

```sh
hgs codex -c 01a0e955-b037-73f0-bc6c-36622a8c27b7
hgs codex /path/to/project --resume=01a0e955-b037-73f0-bc6c-36622a8c27b7 -n review
```

After `-c`, IDs are recognized as UUIDs; Kimi also accepts `session_UUID`. Without
an ID, `-c` continues the latest conversation in the project. An already tracked
conversation keeps its session name; another gets `resume-ID` unless `-n` is set.
An occupied name bound to another conversation is an error. Native history must
remain on the selected host: project synchronization does not move conversations.

Before the agent confirms its conversation and creates native history, pause is
unavailable. Codex and Kimi may defer creation until the first message. Codex can
also defer `SessionStart` after resume; Activity may send the first resumed message,
pause or clear only after verifying the exact process, matching history and empty
native input. Codex reports such a clear with the next message's `SessionStart`,
which then names the new conversation instead of the restored one. A sent first
message or an archive restore still waits for that confirmation.
Unknown flags or noninteractive modes such as `codex exec` are not guessed.
The original positional prompt is never replayed during resume.

A launch interrupted before conversation confirmation is not a Saved session.
Repeat the same command without `--fresh`; the diagnostic attempt is preserved in
`attempts/`, and native history is retained. Old untracked sessions cannot be safely
paused: launch the intended conversation with its explicit native ID to bind it.

### Tracking and durable state

`hgs hooks --account native-claude --json` inspects the account's hook
registration without running the agent. Use another exact account ID for an
isolated profile. The report contains provider/account identifiers, configuration
and path fingerprints, expected/present/missing/duplicate event names and cached version
compatibility, never settings, commands, prompts or credentials. DeepSeek and
OpenCode report `not_applicable`: they use their native APIs instead of tracking
hooks. An unavailable or changed executable-version cache means compatibility
`unknown`; inspection never runs `--version` or refreshes that cache.

`hgs hooks SESSION --json` accepts a JSON object on stdin containing
`request_id`, `expected_run_id` and `expected_conversation_id`. Only that exact
bound conversation is inspected. Its `observed` field counts known main-agent
callbacks recorded after this feature was enabled in that run/conversation;
older bindings or silence report `unknown`, not broken hooks. The summary is
bounded by the known event names and never scans native history. Registration,
observed callbacks and provider-effective settings/trust are different evidence:
`effective` and `trust` remain explicitly `unknown` in a file inspection. Project
settings, policy, native trust or a running agent's already-loaded settings can
make its effective hooks differ. Attaching does not retrofit them.

Explicit configuration repair is native-only:
`hgs hooks --account ID --repair --json`. Its stdin must contain a fresh UUID
`request_id`, `expected_provider`, `expected_account_id`,
`expected_config_sha256` and `expected_path_id` copied from the inspected account
report. Repair refuses changed content, account/path/symlink identity, malformed
or oversized files and unknown/incompatible native-version evidence. It merges
only missing HGS tracking registrations under the existing hook lock and atomic backup
rules, preserving personal settings/hooks and trusted legacy commands. It never
trusts hooks, enables/widens answer channels, changes permissions, restarts a provider or retrofits running
agents. If compatibility is unknown, explicit repair is unavailable. The current cache is
normally populated for Claude's version-qualified hooks; Codex and Kimi can remain
unknown even after launch. Their normal supervised launch retains the existing
hook installer, but this diagnostic never invents version evidence. Review the returned configuration
and any conditional matchers rather than treating a successful file edit as
proof that hooks are effective. Existing partial Kimi blocks require manual review
or a supervised launch: explicit repair refuses to rewrite them lossily.

The additive mobile operation `hooks_status` exposes only the exact-session
read, and only when the selected CLI advertises `hooks-health-v1`. Both outbound
connectors preserve the request/run/conversation identity. The relay and gateway
refuse account-wide, repair, archive and child extensions; no mobile hook write
operation or client repair UI is advertised. Diagnostic receipts have short
read-only retention and never authorize replay of a mutation.


At launch, `hgs` adds handlers to Claude `settings.json`, Codex `hooks.json` and
Kimi `config.toml`, preserving other hooks and making a `.pre-hgs` backup before the
first edit. It respects `CLAUDE_CONFIG_DIR`, `CODEX_HOME` and `KIMI_CODE_HOME`.
Handlers do nothing outside an `hgs` run. `HGS_TRACKING=0` disables registration for
new launches. Native hook trust still requires an explicit user choice in Codex.
Claude Code's `PostModelSwitch` is registered only once `claude --version` reports
2.1.296 or later: older versions stop every start at a settings warning for a hook
event they do not know. Until then each launch reads the version again; a version
that cannot be read gets no such event. A launch through a project wrapper
(`run_claude.sh` or `run_claude.ps1`) never reads it, because a launcher runs only under supervision. Once registered,
the version is read again only when the `claude` executable changes; a version older
than 2.1.296 then has hgs remove its own `PostModelSwitch` entries before a tracked
start, dropping the event only when nothing else is left in it; started outside hgs
or with `HGS_TRACKING=0` first, it still warns until then. The user's own hooks for
that event stay, and older versions still warn about those. An unreadable version or
a launcher keeps the entry, as does a wrapper script that starts another Claude Code
without itself changing: delete the entry from its `settings.json` by hand there.
`PreModelSwitch` is never registered.

Claude Code can offer once, at startup or under a finished reply, to make auto
mode the default permission mode when the user settings name another default.
Activity shows the offer with both native choices; Zerus never answers or hides
it, and only the choice made in Activity or Terminal changes that setting.

Claude Code reports its context window only to a status line command. For a tracked
Unix start of the `claude` CLI that reports 2.1.296 or later (the version read as
for `PostModelSwitch`), `hgs` adds `--settings` with its own `statusLine`,
`hgs __state statusline`, to the agent's command line; no settings file changes and
the saved launch arguments stay as typed. The command keeps the run's latest
`session_id`, `model.id` and `context_window.context_window_size` from Claude's
input in `context-telemetry/` of the state folder, one file per run; a new run
removes those not updated for 30 days. It then runs the status line Claude would otherwise use:
the `statusLine` of the user settings file (`CLAUDE_CONFIG_DIR` or `~/.claude`),
then the session folder's `.claude/settings.json` and `.claude/settings.local.json`,
each later file's fields over the earlier ones. It runs that command with
`/bin/sh -c`, Claude's input, environment and folder, prints its output and exits
with its status, so Claude shows the user's status line as before and a change to
it applies at once. Its `padding`, `refreshInterval` and `hideVimModeIndicator` are
copied at launch. Without a user status line nothing is printed; Claude 2.1.296
then keeps one empty row below the prompt. Claude keeps its own status line, and
the session reports no window, on Windows, through a project launcher, with an older
or unreadable version, when the launch passes `--settings`, `--setting-sources`,
`--project-config-root`, `--managed-settings` or `--worktree` (`-w`), when managed
settings exist (`/etc/claude-code` or
`/Library/Application Support/ClaudeCode`, or `CLAUDE_CODE_MANAGED_SETTINGS_PATH`),
when a settings file cannot be read or its status line is not a command, and when
the session folder lies below another repository root (an enclosing repository, or
the repository of a linked worktree such as a generated one) and the local settings
of that root or of the folder set one: Claude reads one of the two by ownership checks.

Tracked Codex needs `--no-daemon`; the integration has been verified with 0.160.0,
while 0.155.1 lacks that required flag.

New hooks call `hgs __state hook`. Existing trusted Python commands are retained:
`hgs_state.py` forwards them to the Rust CLI, so keep Python for legacy hooks.
The CLI and session core are Rust; SQLite is bundled and requires a C compiler at
build time. Existing session bindings and provider history remain compatible.

Session records live in `${XDG_STATE_HOME:-~/.local/state}/hgs/sessions`, or
`HGS_STATE_DIR`. Writes are atomic and synchronized to disk. Records contain the
command and arguments, folder, conversation ID and run identity; environment
secrets are not saved. A secret passed directly as an argument is still an argument
and can appear in the private record. Native agents retain their full history.
Late events from a previous run and child-agent events cannot rebind the parent.

`runtime_state`, `process_state` and `conversation_state` distinguish the tmux
terminal, agent process and native conversation. `SessionEnd` alone does not prove
process exit. A live process without verified activity shows **Status unknown**;
**Agent exited** means the process ended while its tmux terminal still exists.

### Rename, Saved and Archive

**Rename…** changes the label after the project name without replacing the process
or working folder. Tracking and journal history follow the new name; attached
terminals receive updated titles. Archive renaming targets only the selected ID.

```sh
hgs rename codex/project-docs/review codex/project-docs/database-review
hgs @mac rename claude/sample-project/review claude/sample-project/deploy
hgs rename codex/project-docs/review codex/project-docs/database-review --archive ID
```

Live and Saved names must be free. Archive names may repeat. Labels accept Unicode
and internal spaces, but reject `/`, `:`, `.`, control characters and surrounding
whitespace. `--dry-run` validates without mutation; `HGS_TAB=0` disables title updates.

**Saved sessions** are unfinished conversations paused explicitly or left after
process interruption, tmux loss or reboot. **Archive** contains completed
conversations. For automatic archiving, a tracked run needs both native main
conversation completion and a successful process exit without evidence of pause
or an external signal. `SessionEnd`, an SSH disconnect or a completed reply alone
is insufficient. Older ambiguous records are not reclassified automatically.

```sh
hgs archive codex/my-project/review                 # stopped sessions only
hgs ls --local --json
hgs inspect codex/my-project/review --archive ID
hgs resume codex/my-project/review --archive ID -d
hgs kill codex/my-project/review --archive ID        # forget only this binding
```

Archives live in `archives/`, each with an independent ID. A new launch with the
same name preserves the archive. Restore refuses an occupied live or Saved name
and removes an archive binding only after the native agent confirms the expected
conversation. Failed restore keeps it. **Forget archive** preserves native history;
`resume --all` excludes archives.

Desktop Restore keeps the opened conversation and its message draft when the
resumed row confirms the archived conversation ID. Text, attachments and editor
selection move to the live draft key; machine/project/search scopes stay in
place. An existing live draft is never overwritten: the archive draft remains
in Saved drafts. A later user selection is not replaced when Restore completes.

### Browse Archive by date

Desktop and Android filter **Archived date**, the time a conversation moved into
Archive, independently of its creation or last message. Choose All dates, Today,
7/30 calendar days, or an inclusive From/Through range. Days use the viewing
device's local time zone, including daylight-saving changes. Pages request the
most recently archived instances first; the desktop retains saved row order within
each project. **Refresh** starts a fresh view. **Older sessions** and **Newer
sessions** replace the current page, retaining at most 50 rows per selected online
computer. Only two cursor anchors per computer are kept, without a growing page
or cursor history; every matching record remains reachable in either direction.
The compact period button opens quick ranges and a custom From/Through picker.
Quick ranges fill the From/Through draft and keep the picker open, so either date
can be adjusted before **Apply range**. Only Apply changes the active date filter;
opening a picker, choosing a preset, Cancel, Escape or dismissing the panel never
fetches a page. **All dates** shows **Any date** for both bounds and applies no
date limits. Calendar inputs follow the app's desktop theme.
The navigation row shows the current range for one computer or the loaded/matching
count across computers. Hover the desktop count or expand the mobile count for
each computer's exact page range. Older/Newer arrows sit beside this summary;
their accessible labels describe the direction. Refresh uses the adjacent icon.
The Archive badge counts rows in the current project/machine scope, with `+` when
the current window does not contain the whole archive. An unavailable
computer keeps its own error and retry state; changing dates, search or machines
discards responses belonging to the previous view.

Upgraded desktop and connector polls use `hgs ls --json --local --no-archives`.
For older Android clients, the connector attaches a cached recent window of at
most 50 archived rows/128 KiB and explicit `mobile_archive` loaded/total/partial
metadata. It refreshes that window only when the native directory revision
changes or five minutes expire. Native identity checks and the existing fresh
snapshot gate remain in force for that window. Updated Android ignores this
compatibility window when page operations are supported; an old relay can still
show it while the system updates. Full Archive browsing needs the updated client.
The legacy `ls --json` default remains compatible. The native, read-only ABI is:

```sh
printf '%s' '{"from":0,"until":253402300800,"limit":50,"cursor":"","query":""}' | hgs archives --json
```

`from` is inclusive and `until` exclusive Unix seconds (integers, nonnegative);
`limit` is 1–50. All fields are optional. `query` is literal case-insensitive
archive metadata search (names, IDs, provider, folder and title), at most 256
characters; this is distinct from the desktop's global public-message search.
The result contains `version:1`, `sessions`, `total`, `next_cursor`, the additive
`previous_cursor` and zero-based `position`, and the echoed range/query. Pass either
opaque cursor with the same range/query to move older or newer. Both directions
use date/ID keysets; no offset scan or retained client cursor stack is required.
Ordering uses `(archived_at, archive_id)` descending, avoiding offset duplicates
when an earlier page changes. Newer completions appear after Refresh. Deleted or
restored instances disappear; all actions still recheck exact native identity.
Cursors belong to one computer/catalog and cannot be reused after rebuilding it.

A rebuildable `archive-catalog-v1.sqlite3` indexes metadata beside the durable
state. Archive JSON files remain authoritative. Its first use indexes existing
records; unchanged polls/pages reuse it, and atomic archive writes, rename,
restore or forget update the directory generation. Only requested rows open
conversation summaries, outside the state writer lock. Browsing never starts an
agent or changes read marks. New archive pages use the additive mobile
`archive_list` capability; older computers retain their previously loaded archive
view and need an updated CLI/connector/relay for paging. Archive IDs are admitted
from validated pages for one hour (bounded to 2,000 recently paged instances),
while native inspection and lifecycle actions still verify name/archive/run.
An already-open older archive also survives a connector restart or cache expiry:
`hgs archives lookup --json` accepts only `name` and `archive_id`, reads that one
authoritative record without scanning the catalog or transcript, and returns its
name/archive/run/conversation identity. The connector verifies this response and
any requested run/conversation before admitting an uncached target. Failed or
mismatched lookups grant no admission; lifecycle operations keep their native
fresh checks. This internal read ABI adds no relay mutation or phone operation.

Desktop bulk removal is explicitly **Remove loaded archived sessions** when
paging is enabled. Its confirmation names/counts those loaded instances in the
current date/machine scope; unloaded entries are kept. Local user grouping follows
the exact observed run even when its reusable session label later changes owner.

## Project wrappers

A wrapper in the project folder runs inside the session in place of the native
agent. Use it to prepare the environment and launch the agent with the arguments
it receives.

- On Linux and macOS an executable `run_COMMAND.sh` runs as it is. Without one, a
  `run_COMMAND.ps1` runs when PowerShell 7 (`pwsh`) is on `PATH`, as
  `pwsh -NoLogo -NoProfile -File run_COMMAND.ps1 ARGS...`; without `pwsh` the
  native agent starts, so a project may ship a `.ps1` for its Windows users only.
- On Windows only `run_COMMAND.ps1` is a wrapper; a `.sh` file is never started.
  It runs as `pwsh.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File
  run_COMMAND.ps1 ARGS...` with PowerShell 7 (`pwsh.exe`) from `PATH`, the Microsoft
  Store installation included, else with Windows PowerShell (`powershell.exe`).
  Windows PowerShell drops the double quotes and the empty arguments that a script
  passes on to a native program, such as Codex's `-c key="value"`, so install
  PowerShell 7 for wrappers. When neither can be found, the launch fails with an
  error naming the wrapper; `--bare` starts the native agent instead.

```sh
hgs codex sample-project                 # run_codex.sh starts a new conversation
hgs codex sample-project -c              # run_codex.sh resume --last
hgs codex sample-project -- resume ID     # continue this exact conversation
hgs codex sample-project --bare          # bypass the wrapper
```

A PowerShell wrapper passes its arguments on with `@args` and returns the agent's
exit code:

```powershell
# run_claude.ps1
$env:EXAMPLE_TOKEN = (Get-Content -Raw "$HOME/.config/example/token").Trim()
& claude @args
exit $LASTEXITCODE
```

Explicit arguments after `--` suppress automatic Codex `resume --last` insertion,
even with `-c`. Use `-- resume --last --model MODEL` when needed. Claude and Kimi
forward `-c` to their wrapper. Existing sessions attach without rerunning the
wrapper. Interactive setup needs an attached launch; detached mode assumes the
required environment is already available. Never put private keys in wrappers.
Pause, resume, recovery, fork and archive restore start the same wrapper again
with the conversation's resume arguments after it. `-File` hands every later
argument to the script unchanged, except that PowerShell splits an argument of the
form `-name:value` at its colon. PowerShell cannot replace itself with the agent
as `exec` does in a `.sh` wrapper, so the tracked process is PowerShell and the
agent its child, as with a `.sh` wrapper that does not `exec`. Checks that need
the tracked process to be the agent itself then refuse: the first message to a
resumed or forked Codex conversation goes through Terminal until Codex confirms
it. On Linux and macOS a pause sends SIGTERM to that process only; prefer a `.sh`
wrapper that ends with `exec` there.
Tracking never runs `run_codex.sh` or `run_codex.ps1` to detect Codex capabilities;
a wrapper runs only as the launch and always receives `--no-daemon`, so it must
start a Codex release with that flag. `--bare` checks the native Codex and omits
the flag for an older release.

<a id="desktop"></a>

## Desktop workspace

The tray's **Open Zerus**, a left click or `hgs-tray --sessions` opens the workspace.
On macOS, use `~/Applications/hgs-tray.app/Contents/MacOS/hgs-tray --sessions`.
On Windows, starting `hgs-tray.exe` without options opens it too; `--background`
starts in the tray only ([Windows](#windows)).
Repeated launch raises the existing window. Closing it leaves the tray and agents
running. The executable, bundle path, bundle ID and service names remain `hgs-tray`
for compatibility. Windows, notifications, the tray, `--version` and the Linux launcher
show Zerus. On macOS, a localized display name shows Zerus in the Dock, Finder,
Spotlight and System Settings for the unchanged `hgs-tray.app` bundle.

Settings → Mobile connection also lists and pairs [phones](#machines-and-accounts). Settings → Mobile connection → Phone notifications sets when the phone notifies: immediately, as soon as you are away from every Zerus computer, or after 5, 10, 15 or 30 minutes away. The choice is shared through the swarm catalog; computers this one synchronizes with directly whose hgs is too old to receive it are named there.

The tray has two counters: all sessions and **Needs attention**. Attention includes
input, approvals, errors and unread main-agent replies. An offline host retains
its last total but adds no live attention requests; a dot marks incomplete data.
Linux application badges and the macOS Dock show the attention count, hiding zero.

The session menu's **Mute** disables notifications for that conversation on all
devices and removes it from their notification counters and bells. The session's
host stores the preference; other desktops and phones receive it in their next
host snapshot. A crossed-out bell and neutral gray or white attention and unread
badges identify a muted session. The preference survives restart, rename and
resume of the same conversation. Muting preserves the agent's actual state,
unread replies, questions, approvals and review reminders. The menu requires an
online host with shared notification preferences and a configured local `hgs`
that advertises `session-notifications-v1`; older hosts show an explanation
instead of a separate local-only preference. A pending or failed change never
replaces the last confirmed host value.
If a host later becomes unavailable or stops reporting this feature, its last
confirmed muted display is retained and labelled as such; it does not enable a
new change. Concurrent changes use the host's revision and require a fresh
snapshot after a conflict. Unmute records the host's current notification
identities, so a device that missed the muted interval does not announce those
old events when it reconnects. This does not mark the underlying work as read.
**Unmute** restores future notifications without replaying events seen while
muted or acknowledging pending work. Muted sessions remain available in the
Needs attention filter.

Notifications open Activity for the exact requesting run and conversation,
including remote or renamed sessions. Polling and restarts do not repeat the same
request; resolved notifications are removed. Main-agent reply banners wait until
the session is idle and has no reported active children. While native agent work,
live background commands or active subagents continue, intermediate replies remain
unread in Activity but show no new-reply badge or reply-only attention count on
desktop or Android. The pending reply becomes visible when work finishes; this
never marks it as read. Explicit questions, approvals, errors and review reminders
remain actionable. Optional Codex questions, which the agent continues without,
are not attention: on the desktop, Android and iOS they change neither the
session's status nor the Needs attention filter, counters or badges. Listings report them separately
as [`optional_questions`](#optional-questions-in-listings). Settings → Sessions →
**Notify about optional questions**, off by default, adds an *Optional question*
banner once for each new question. Questions already pending when it is turned on,
or first seen while Zerus is focused or the session needs action, are only
recorded; a stop, resume or temporarily unknown count announces nothing again. The
banner opens the conversation's Activity. It is withdrawn when its questions are
answered or skipped, the session is marked as read here or on another device, or
the setting is turned off; delivered Windows messages remain but no longer open a
session. Routine replies from a turn the
agent scheduled itself (Kimi `CronCreate`) are not new replies while a recurring
job remains: they appear in Activity without an unread mark, badge or banner. The
reply after the last job ends, a one-shot reminder and any reply to the person's
prompt notify as usual. Explicit input and approval
requests still notify immediately. Ordinary child results and errors stay with
the parent. While Zerus has application focus, session banners are suppressed and
are not replayed when focus leaves. Unread replies and attention requests remain
visible in the interface, including requests from other sessions and machines.
Delivery failures show a reason and retry after a minute while still relevant.
macOS uses Notification Center and needs its system permission; Focus and OS
notification settings still apply. Notification Center looks up the icon by bundle
ID, so a stale per-user IconServices entry can keep an earlier icon after updates
while Finder and the Dock show the current one. Remove
`$(getconf DARWIN_USER_CACHE_DIR)com.apple.iconservices/store.index`, then run
`killall iconservicesagent NotificationCenter`; both restart on demand.

While Zerus runs, even with its window closed, it records keyboard/mouse idle time
and screen lock for phone notifications. macOS uses CoreGraphics without a
permission prompt. Linux uses the desktop's screen-saver and idle interfaces (KDE
Plasma, GNOME and other compositors with Wayland idle notifications). When none
is available only input in Zerus windows counts, which can only cause extra phone
notifications.

**Sessions**, **Projects**, **Machines** and **Accounts** share the navigation rail;
click the logo for **Overview**. Overview shows attention, work in progress,
online machines, CPU, memory and load, plus account limits. Counts open the matching
filter, sessions open Activity, and machine settings lead to Machines. Native
account quotas are grouped by verified identity instead of adding machine usage.

**About** is the final Settings section, after **Voice input**.

**Settings → Voice input** selects the speech computer, its recognition profile
and the local microphone. The speech computer is independent of the agent's
computer: a laptop can record locally and use a configured GPU peer over SSH.
The provider manager creates, edits, tests and removes profiles on the selected
processing computer. Choose **Local GigaAM** (model and CPU/GPU) or an
**OpenAI-compatible server** (API base URL, model, language and optional API key).
An empty language lets the server detect it; an explicit language uses a two-letter
code such as `ru`. Keys remain on the processing computer and are never shown
again; Keep, Replace and Remove are explicit choices. Test checks the saved
profile with synthetic audio and cannot send input to an agent. Removing a
profile requires confirmation. Choosing a default is separate from browsing or
editing profiles. Configuration is available even when inference is not running.
See [provider management](speech-providers.md) and
[host installation](../services/speech/README.md). Local GigaAM starts with the
Russian `v3_e2e_rnnt` profile. Saving or selecting a profile does not install or
download a model and never falls back to another provider.

On desktop, click **Voice** in the message toolbar to start recording. The icon
immediately to its right switches voice/video mode without starting capture;
it stays disabled while opening, recording or retaining a preview. Recording
continues after the click. Click **Send** to send, or **Stop and preview** to
review it with **Play**, **Discard** or **Send**. Enable **Preview before sending**
in Voice input settings to show **Stop** instead of **Send** while recording
audio or video. The setting is local to this device and applies to the next
recording; existing drafts and deliveries are unchanged. Audio recordings
are limited to two minutes and remain separate from the typed draft. Capture
opens the local microphone without waiting for speech computers or providers.
Provider discovery runs in the background. If it fails, the recording remains
available for playback; refresh providers in the preview before sending.
An unavailable selected provider stays selected until you explicitly change it.
A retained recording also pins a versioned provider's configuration revision;
editing that profile requires explicit reselection before sending the recording.
During capture, the typed draft is preserved and read-only, and ordinary message
submission and attachment changes are blocked. History remains readable.
**Space** or **Escape** stops recording into the retained preview; neither sends
nor discards it. Recording takes priority over Escape's agent-interruption
action. Holding either key stops capture only once, and editing resumes after
capture ends unless another delivery lock applies. Android supports the same
controls with a hardware keyboard; its touch Stop control remains available.
On Android, an empty editor with no attachments shows a microphone in the Send
position. Tap to switch voice/video, hold to record, release to send,
slide left to cancel or up to lock. Stop a locked recording to keep a preview.
The recording row overlays the retained editor and shows a red indicator, elapsed time and **Slide to cancel**;
the hint moves and fades as you slide left. Short haptic feedback accompanies
confirmed capture start and explicit stop/cancel, respecting Android settings.
Text or attachments restore the ordinary Send button. Sending requires a
confirmed provider for the exact conversation; delayed discovery keeps a preview
until an explicit Send.
After granting microphone permission, press again to start recording.
The preview lists speech profiles reachable from the conversation computer. Explicit Send
stores the original there, starts recognition and delivers recognized text to
the pinned agent conversation. Video is limited to 60 seconds and 10 MiB. Its MP4 and extracted WAV share one
message identity; the agent receives recognized speech. A tap starts circular
video playback. One history card retains the playable original,
expandable transcript and processing or delivery errors across devices. Its
circular play/pause control sits beside the recording's amplitude waveform;
the played portion is highlighted, and selecting a point on the waveform seeks
within the recording and keeps paused audio paused. The time display follows
playback. Older recordings gain their waveform in the background without
recognition or delivery being repeated.
Clients recover interrupted requests by the original message UUID without blindly
uploading or sending again. Hiding the composer stops unsubmitted capture.
See [persistent voice messages](voice-messages.md) for storage and delivery details.
Voice settings also show the selected computer's
90-day counters. Desktop Settings > Private analytics and Android Settings > Computer analytics
show bounded per-computer and fleet summaries with explicit coverage. Agent collection
starts disabled; local desktop controls distinguish enablement, retention and purge.
See [analytics](analytics.md) for the private storage and read contracts.

If microphone access is denied, Voice shows a permission explanation with
**Open microphone settings** on macOS and Windows. On macOS this opens
**System Settings → Privacy & Security → Microphone**. Allow Zerus, return to
the conversation and press **Voice** again; opening settings does not start a
recording. If settings cannot open, the dialog keeps the manual navigation path.

Session rows show provider, machine, current action, model, effort and Git context.
Double-click a session row to toggle between the full card and a compact row
showing only its name, status and agent type. Right-click **Compact session row**
is also available. Compact rows keep their expand icon; it or the Right key
restores the full row. The row remains selectable and keeps its normal context actions. This device
remembers the preference for that conversation across restarts. **Expand all
collapsed** in the Sessions menu restores every compact row, including sessions
hidden by the current filters. Row density never compacts the agent's context
or changes its runtime state. A muted session keeps its mute icon in either size.
The folder/branch line also shows native outline Git badges: a diff with the
number of changed/new files, upload/download with outgoing/incoming commit counts,
and a green cloud check for a clean checkout matching its verified upstream.
Changes include staged, unstaged and untracked paths, counting each path once.
Merge conflicts use a red diff badge. Local changes and outgoing commits can
appear together; counts above 99 show `99+`, with exact values on hover.
Hover over the badges for the checkout, branch, upstream, counts and remote check
time. These describe the current checkout shared by sessions in the same folder,
not ownership of an individual feature or an archived conversation.

Git reads run separately from agent/fleet polling, with at most two concurrent
requests. Local evidence refreshes about every 15 seconds and expires after 30;
read-only remote verification is cached for at most 60 seconds. A changed tracking
OID or remote endpoint invalidates that verification. Nothing fetches, commits or
pushes automatically. A muted cloud question mark means verification is unavailable,
expired, detached or lacks a remote upstream. Outgoing/incoming counts become muted
when only the local tracking ref is known; a changed remote asks you to fetch.
Ordinary non-Git folders show no badge. Offline and changed checkout evidence
cannot show a green check.

**Ready** means a reply ended, not that a long-running task is complete. An idle
agent with confirmed live shell commands shows **Background** in muted blue, with
the command preview and elapsed time. Open **Processes** to follow output or inspect
the running commands. The session remains in **Active** and still accepts new
messages. Green **Working** indicates native agent work. Input/approval requests
take precedence; ordinary agent work takes precedence over background work,
followed by Scheduled and Ready. Offline and
historical/unconfirmed process records never establish background work. The CLI's
additive `process_work` summary uses current owned OS trees (one process-table
sample per fleet listing) or the native DeepSeek job registry; it does not scan
native transcripts on each fleet poll.

Working and compacting timers use native start evidence and update locally without extra
host requests. An idle agent with pending jobs it scheduled for itself (Kimi
`CronCreate`) is **Scheduled**: its badge and detail show the next run time
instead of a timer, or "due" once that time has passed. A question, approval or
error still takes precedence, and work shows as Working. Theme, density, animation and machine/project label appearance are
local preferences. **Settings → Appearance → Content scale** adjusts Activity,
questions, the message field and Terminal from 75% to 200% in 5% steps, with 100%
as the default; the session list, the side panel, the session header and its tabs
keep their size.

Drag the small handle above the message toolbar upward to enlarge the message
field, or downward to shrink it. Double-click the handle to restore the default
height. The height is a local preference shared by sessions and retained across
restarts; smaller windows temporarily limit it so the conversation stays visible.
With the handle focused, Up/Down resize it and Home restores the default.

**Settings → Appearance → Activity width** keeps Activity readable in a wide or
fullscreen window: the transcript, its queued input, questions and the message
field share one centered column of at most 900 px by default (480–2400 px). The
width is measured at 100% and grows with content scale, so lines keep their
length. The scroll bar stays at the pane edge and the side margins scroll the
transcript. Drag either column edge in Activity to resize the column
symmetrically, or double-click an edge to restore 900 px. **Full-width Activity**,
in Settings and in Activity's context menu, fills the whole pane; **Reset Activity
width** in the same menu restores the default column. Narrower panes use their
full width, and Terminal, Processes and Native UI are not affected. A reset icon
appears beside an Appearance slider's value once it differs from the default and
restores that default.
**Settings → Appearance → Usage limit indicator** sets how the account usage in the
status bar and on Overview account cards shows each limit window. The default, **Usage and elapsed time bars**, draws usage above a gray
bar for the elapsed part of the window; a longer usage bar means a faster than
even pace. **Pace marker** shows elapsed time as a mark on the usage bar, and
usage beyond the mark takes the warning color. **Pace marker and reset
countdown** adds an hourglass with the time until reset, such as `26h` or `6d`;
compact Overview cards omit the countdown. Colors follow the average pace since
the window started: green when the limit lasts until reset, orange when it would
run out in the second half of the remaining time or 90% is used, and red when it
would run out sooner or is exhausted. The first 8% of a window, and windows
without a reported length or reset time, keep the fixed 70%/90% thresholds. The
tooltip adds the time until reset and the pace forecast.
**Settings → Appearance → Keep Zerus above other windows** keeps the Zerus window
over other applications on X11, macOS and KDE Plasma 6 on Wayland. On KDE Wayland,
Zerus uses temporary KWin scripts scoped to its own window and confirms the
compositor's state; it installs no window rules. The pin above Settings on the
left rail switches the same option, and both controls follow changes made in
KDE's window menu. The unpinned icon is a muted tilted outline; the pinned icon
is upright, filled and gold, with a small line beneath it. The icon follows the
confirmed window state and retains its color across theme changes. Hover only
highlights the button background; its tooltip names the next action.
KWin rules can override the preference; Zerus explains a
refused change. Other Wayland desktops require their own integration and show
the option disabled with a window-menu/rules workaround. Files and terminals
opened from Zerus may appear behind it. Missing Git metadata does not turn
a folder into a repository.

The project button beside the machine chooser focuses Sessions on one or more
logical projects. Check projects to apply changes immediately; selected projects
appear as removable chips. **All projects** clears this restriction. Right-click
a project band for **Filter this project**, which replaces the selection with
that project alone. Clear it using its chip or **All projects** in the chooser.
Multiple projects selected in the chooser form a union,
narrowed by the machine, state, folder and search filters. Selection stays local
to the open workspace while navigating sections; an explicit session link or
Overview scope opens its requested context. Renaming a project retains selection,
and new projects do not join an explicit selection automatically.

The project chooser shows parent-session counts for the current state, machine
and folder scope before project selection or text search, including zero counts.
Its open checkboxes and selection stay in place during polling. A removed selected
project remains as **Unavailable project** until removed from the filter.

Changing the state, machine or project filter only changes the session list.
The open conversation and its draft stay in place even when no row matches;
its row is selected again when it returns to the list. Click another session
to open it.

When the open session is terminated or removed, the conversation pane returns
to **Select a session**. Polling, new sessions and filter changes keep this
selection empty until you choose a session. Unsent drafts remain in **Saved drafts**.

The **Active** filter combines sessions currently working or Scheduled, those in
**Needs attention**, and sessions with unsent text or attachment drafts, counting
each session once. Desktop and Android use the same filter IDs and membership
rules; drafts remain local to each device. Drafts for visible subagents also keep
their parent session active. Drafts and explicit review-later reminders remain
active offline; stale work alone does not. Archives stay in Archive. Active also
applies to desktop content search, and the Dashboard's Active counter opens this
same filter. Creating, clearing or sending a draft updates the filter and count.
Cards with an unsent draft have a blue edge in the Draft color, including the
open conversation, offline sessions and Saved drafts inspection. Collapsed tiles use
the same color around their frame. Attention and errors retain their warning
colors; working indicators and unread identity remain unchanged.

The **+** in the session list header opens **New session**, like the one in the
side rail. The button beside **⋯** collapses the list into a strip. Every project
band and session row keeps its height, so each session
becomes a square tile: status and provider icons, the session name on up to two
lines and a short state such as the working time. Instead of the attention
edge, a tile's frame takes the color of its state (working, needing an answer,
error, paused, draft or a new reply) with a faint glow inward. Each band shows
the project name and its most urgent counter. Above the tiles and as wide as
them, the strip keeps a search button and two filters: the active one and the
most urgent other one (sessions needing an answer, otherwise active sessions,
otherwise all). Rows open on click and name their session in a tooltip;
tiles do not open a session's subagents. Expanding grows the same rows back
into cards. The strip's search button opens the full list over the conversation
without resizing it; Escape or a click outside returns to the strip, and the pin
docks the list again. While the pointer rests on the
strip, the full list opens over the conversation; turn this off with **Settings →
Sessions → Expand the collapsed session list on hover**. The collapsed state and
the docked width are local preferences.

Search names, IDs, folders, branches and models immediately. From two characters,
content search also reads public Codex, Claude and Kimi messages and saved tool
events on each host, and DeepSeek messages through a running native host
([DeepSeek](#deepseek)). Project, machine and state filters narrow results; the all-sessions
view includes archives. Results open the recorded message; **Back to latest
activity** returns to the live timeline. **Limited history searched**, **Some
history unavailable** and excerpt labels describe incomplete coverage. Internal
reasoning and system instructions are excluded.

**New reply** stays unread until its latest result is visible in an active window
or an attached Terminal at the bottom. Background windows and old search results
do not clear it. Read state is kept on the session's host: a reply or request
read on any desktop or phone is read everywhere after the next poll and its
notification is withdrawn; it survives rename and resume. Hosts with an older
`hgs` keep read state on each device. Viewing never answers an approval. **Mark
as read** clears the current reminder without answering the agent; **Mark needs
attention** creates a local review reminder.
Opening the conversation does not clear an explicit review reminder.
**Mark all as read** (right-click the Sessions icon in the rail, the icon beside
Saved drafts, or the project and machine action menus) marks current replies on
every machine as read, including hidden sessions. Sessions marked as needing
attention keep their reply unread; pending approvals, questions and errors stay
highlighted.

Ctrl/Command-click selects multiple sessions while preserving the open conversation
and draft. Shift-click selects the visible range from the last selection anchor;
Ctrl/Command+Shift-click adds that range to the selection. Shift with Up/Down or
Home/End selects ranges by keyboard, using the current filtered row order and
skipping hidden rows, project headers, subagents and pending launches. Anchors
follow session identities across refreshes; a hidden or removed anchor falls back
to the visible open row, or to the clicked row when none is visible.
Batch actions include local **Compact session rows** and **Expand session rows**
icons, read/review later, pause, resume, archive, project move, termination and
forgetting. Both density actions are available for a mixed selection and apply
only to rows needing that change, including offline rows. Applicability is shown
per selection. Escape or the selection close button
clears selection. Context menus and shortcuts preserve the target identity.
**Clear archive…** requires confirmation, preserves offline records and excludes
new records created after the confirmation snapshot; native history remains.

Enter opens the selected live session and Escape closes the current transient UI;
it never closes the workspace. An archive needs explicit **Restore**; Enter does
not start it. The other keyboard commands are listed below.

<a id="keyboard"></a>

### Keyboard

**Mod** is Ctrl on Linux and Windows and Command (⌘) on macOS. **Control** is the
physical Control key everywhere; macOS keeps Command-Tab and Command-` for its
application and window switchers.

| Command | Linux / Windows | macOS |
|---|---|---|
| Overview, Sessions, Projects, Machines, Accounts | Ctrl+1 … Ctrl+5 | ⌘1 … ⌘5 |
| Settings | Ctrl+, | ⌘, |
| Go to… (sessions, projects, accounts, machines, commands) | Ctrl+K | ⌘K |
| Keyboard shortcuts | F1, Ctrl+/ | F1, ⌘/ |
| Next / previous session | Alt+↓ / Alt+↑, Ctrl+PgDn / Ctrl+PgUp | ⌥↓ / ⌥↑, ⌘⌥↓ / ⌘⌥↑ |
| Next / previous session that needs attention | Alt+Shift+↓ / ↑ | ⌥⇧↓ / ↑ |
| New session | Ctrl+N | ⌘N |
| Rename session | F2 | F2 |
| Search sessions | Ctrl+F | ⌘F |
| Refresh | Ctrl+R, F5 | ⌘R, F5 |
| Next / previous tab (Activity, Terminal, Processes, Native UI) | Ctrl+Tab / Ctrl+Shift+Tab | ⌃Tab / ⌃⇧Tab |
| Switch between Activity and Terminal | Ctrl+` | ⌃` |
| Show or hide the side panel | Ctrl+I | ⌘I |
| Next / previous area (list, conversation, message field, side panel) | F6 / Shift+F6 | F6 / ⇧F6 |
| Send message (also Enter) | Ctrl+Enter | ⌘↩ |
| Send queued messages now | Ctrl+Shift+Enter | ⌘⇧↩ |
| Send and push now | Ctrl+X, Ctrl+S | ⌃X, ⌃S |
| Increase, decrease, reset content scale | Ctrl+= / Ctrl+- / Ctrl+0 | ⌘= / ⌘- / ⌘0 |

Session navigation moves through the visible list, or the search results while
they are shown, and skips project headers and subagents; it does not wrap.
Attention navigation moves among the sessions of the same list that match the
**Needs attention** filter, wraps around and opens a collapsed project. Tab
cycling skips hidden tabs. From Terminal, Ctrl+` returns to Activity with the
cursor in the message field.

**Send queued messages now** runs the agent's own send-now action for the queue
shown in Activity, exactly like its **Send now** button; it never sends the text
being typed. **Send and push now** sends the typed message and, once the agent
shows it in its queue, sends that queue now. With an empty message field it is
the same as Send queued messages now. If the message does not appear in the
queue within 20 seconds, for example because the agent took it directly or a
dialog such as the cold cache warning is still open, or the session's run or
conversation changes, the push is dropped with a notice; the message is never
resent. Selecting another session drops the push at once, and a new push
replaces one that is still waiting. A message with attachments but no text is
sent without a push, because the queue cannot be matched without text. Without
a selection, Ctrl+X starts this sequence; with a selection it still cuts. As in
every Qt key sequence, a key other than Ctrl+S after Ctrl+X cancels the
sequence and is not typed.

**Terminal keeps the agent's keys.** While Terminal has focus on Linux and
Windows, a key reaches Zerus only when a command uses it and it is Ctrl or
Ctrl+Shift with a digit, Tab, PgUp, PgDn, ` or comma, or Ctrl+Shift+Enter.
Ctrl+letters (Ctrl+C, Ctrl+R, the agent's own Ctrl+X Ctrl+S), Alt chords,
Escape, Enter and function keys go to the agent. On macOS every Command chord
except ⌘C and ⌘V reaches Zerus, and of the Control chords only ⌃Tab, ⌃⇧Tab, ⌃`
and ⌃⇧`.

**Text fields keep their editing keys.** In the message field and search, a
command's key wins over the field except for copy, cut, paste, undo, redo,
select all, word and line movement and deletion (Ctrl, Command or Alt with
arrows, Home, End, Backspace or Delete), and keys without Ctrl, Command or Alt.
Function keys are not editing keys: F1, F2, F5 and F6 reach Zerus without a
modifier. So Alt+↑/↓ switches sessions even while typing, and on Linux Ctrl+K
opens Go to instead of deleting to the end of the line. A focused drop-down
list keeps Alt+↑/↓ and F4 to open it, and the Projects dialog's list keeps
Alt+↑/↓ to reorder projects.

**Go to** (Ctrl+K) searches sessions (not archived), projects, accounts,
machines, sections and commands by name; start the query with `>` to list only
sections and commands. With an empty query, sessions opened recently come
first, then the others in the order of the session list. Enter opens the
result; Escape or the Go to chord again closes the popup. A session opens
under the current filter when it is visible there; otherwise the filter
changes to All sessions. A project selects its first session, or opens
Projects with that project selected when it has none.

**Settings → Keyboard** changes any command's chords (up to two per command,
each a key or a two-key sequence), marks the chords that also work in
Terminal, and resets one command or all of them. Keys used for typing,
navigation, text editing, context menus and Terminal copy/paste cannot be
assigned. A chord already used by another command can be reassigned after
confirmation. Changes apply at once and are stored on this device.
**Keyboard shortcuts** (F1) lists the current chords and these fixed keys:
Enter and Shift+Enter in the message field, Escape, Tab and Shift+Tab, Tab to take
a suggestion, list navigation keys, Menu and Shift+F10, Ctrl/Command-click,
Space and Escape during voice capture, Ctrl+Shift+C/V (⌘C/⌘V on macOS) and
Shift+PgUp/PgDn in Terminal, and Alt+↑/↓ in the Projects dialog's list.

<a id="activity"></a>

## Activity, messages and native controls

Activity shows recorded public messages and tool events rather than a full copy
of native history. User messages preserve literal text, Markdown markers and
line breaks; agent replies render Markdown. The copy button at the right of a
reply's header puts its Markdown source on the clipboard; a text selection still
copies the visible text. Claude's recorded thinking appears in
full as a quieter **Thinking** card: the same Markdown at the same size, without
the card fill and with dimmer text. Adjacent tool events collapse into groups.
Reading older history preserves scroll and selection; **Jump to latest** returns
to live events without resizing the timeline. Its double-down-arrow icon sits beside
Voice in the composer toolbar, outside the messages; the tooltip includes the
number of new events. Subagents use the same icon in their own toolbar.
Pressing Enter on a session or subagent row opens Activity and performs the same
jump. Double-clicking a subagent row also jumps to latest; double-clicking a
session row toggles its compact presentation.
Connect commands remain available through **Copy connect command** in the session
menu. Claude hands finished
background work and other sessions' messages back as a new turn. Activity never
shows them as your message: a background task becomes a violet **Background
task** notice, a subagent's final report a **Subagent report** with one summary
line and **Show report** for the full Markdown, and a message from another
Claude session a **Message from** notice with its full text. Search and the
session's last request ignore all three.

The owner-only `events.sqlite3` journal sits with session records on the agent host
and retains about 50,000 events per machine. Excerpts can contain project data.
Pause/resume continues the same conversation journal. Selected Activity normally
polls every 2.5 seconds while visible; host lists poll every five seconds, and the
tray keeps its background polling when the workspace is closed.

Kimi replies and intermediate public messages come from the exact conversation's
`agents/main/wire.jsonl` when Stop lacks text. Transcript reads are bounded to an
8 MiB tail, 50 replies and 32,000 characters per reply. Late copies do not duplicate
cards. Nested `codex exec` reviews register as separate children, including
`--ephemeral` runs without a transcript. Detection uses the rollout's `source:
exec` or the native caller's ancestry to the verified live parent process.
Child events preserve the parent's conversation, model and history; ephemeral
children show their recorded request, tools and result. Unknown inherited
SessionStart events cannot replace an established parent. `/new` and `/clear`
still change the main native conversation.

### Sending and attachments

Enter sends, Shift-Enter inserts a line and Escape releases focus while keeping
the draft, unless a turn is working. Paste images, drag local files or use the
attachment button. Attachments collect in one chip in the fixed-height toolbar
above the message field, next to session notices (cache, account limit,
recovery) and the context counter; its popover previews, opens and removes them. Labels such as `[Image #1]` and `[File #2]` preserve
attachment placement within the text; removing one does not renumber the rest. Text, cursor/selection and attachment
bytes are saved locally as they change, separately for each machine, session and
subagent. They survive GUI crashes, restarts, unavailable machines and ended
sessions, even if the original attachment file is gone. Storage is a private
directory next to the desktop's Qt settings file (`<settings-file>.drafts`),
outside P2P configuration sync.

Use **Reference an existing file** (the link button beside Attach, or its Files
menu in a narrow column) when the file
already exists on the agent's computer, including very large files. Locally,
choose a file or enter its absolute path. For a remote session, enter an absolute
path (including Windows drive or UNC paths) or `~/` path on the named remote
computer; a file on your desktop is not uploaded
or made accessible to that computer. Zerus inserts an editable, quoted path into
the draft and saves it as ordinary message text. Review it and press Send when
ready. This action does not read, copy or Base64-encode the file and has no file
size limit. It refers to the existing file, whose contents may change; the agent
decides how to use it. Ordinary attachment uploads retain their existing limits.

Session refresh, connection and recovery errors appear on one line beside the
attachment button, with the complete diagnostic in its tooltip and an explicit
session refresh action. They do not move the Activity or Terminal tabs. A blocking
session error makes the message field read-only and muted while preserving its
draft, focus and selection; informational notices leave editing available. When
the composer is hidden, the same diagnostic uses the existing workspace footer.
Other operation notices retain their priority there.

Reads from the selected remote machine show a yellow loading circle beside the
session name only after two seconds. It remains visible while an actual read is
pending and clears when that read completes or fails. Switching sessions shows
only reads belonging to the newly selected session or machine. Local reads do not
show this indicator, and its appearance does not change the header layout.

Ordinary text, cursor and question-form edits save through one ordered background
writer. Waiting edits coalesce to the latest snapshot per draft, so typing does
not wait for disk commits. The editor shows **Saving draft** until its latest
checkpoint completes; a crash before that checkpoint can lose pending edits.
Graceful close drains outstanding writes. Sending, renaming and delivery-state
changes use durable barriers, and an older queued save cannot replace a later
checkpoint or resurrect a confirmed sent message. Attachment snapshots remain
immutable. Save failures remain visible and block sending until persistence
succeeds.

**Settings → Automatic approvals** provides a separate dangerous opt-in, off by
default. Enabling it explicitly allows reachable sessions to run tools without
individual review, including destructive commands and paid usage. It works
while this Zerus process is running, including with its window hidden, and does
not change another desktop's preference or the agents' permanent permissions.
Resuming after an expired cache may process the full conversation again. While
enabled, the normal peer polling interval is capped at five seconds so remote
permission prompts need not wait for the ordinary minute-long refresh.
Only verified, answerable one-time Claude, native OpenCode and native DeepSeek
tool permissions qualify. Ordinary questions, startup trust, incomplete panels and
persistent permission changes remain manual. Existing native identity and
delivery guards still apply.

The same Settings page contains this computer's private approval journal, naming
the computer, session, time, question, selected answer and confirmed outcome.
Each attempt is durably reserved before native input. Failed, cancelled and
unconfirmed attempts remain visible; an unconfirmed delivery is never retried
automatically, including after a restart. If the journal cannot be saved,
automatic approvals pause without sending new answers. Turning the option off
stops new approvals; requests already sent may still finish.

The journal stores full questions only in the private local
`approval-audit/journal.sqlite3` below the existing `HGS_STATE_DIR`. It is not
part of P2P settings sync. Pages contain at most 50 summaries; exact computer and
session filters use indexes, and Older/Newer navigation uses stable record-ID
cursors rather than offsets or complete-table counts. Full question text loads
only for the selected entry. Background discovery scans at most 512 sessions per
tick, admits at most two concurrent targets and bounds its cooldown cache to
4096 entries, independent of total session or journal counts.

`hgs approval-log begin|finish|list|detail` is the local desktop journal API
(JSON stdin). `begin` selects only a supported one-time grant and returns a
durable request UUID; an existing reservation never permits another delivery.
`list` supports exact `host`/`session` filters and `before` or `after` record IDs.
`detail` reads one entry. The command itself never sends native input.

**Saved drafts** below the session list lets you preview, copy or restore drafts
from ended or missing sessions into the current session. It lists the most
recently edited drafts first; each row names the draft's session and computer
(and whether it is a subagent message, question answer or voice message), with
its time and first line. **Go to session** opens the session that owns the
selected draft, or its subagent, while that session is in the session list.
Double-clicking a draft opens the same owning session; unavailable sessions
remain in the draft browser for preview, copying or explicit restoration.
Rows reuse the Sessions cards, including provider and computer badges, with
the last-edited date beside the model footer. Long fields stay within the card;
rows have no hover tooltip because the selected draft's full text appears in
the preview below. Both desktop themes use the workspace colors. Metadata uses
separate fields or slashes, without dot separators.
Copying keeps the dialog open; restoring never sends anything and replacing an
existing draft requires confirmation. A source draft remains available after
copying it to another session.

Independent sessions can explicitly declare **Waiting for acceptance** and
**Waiting for result** through `hgs wait`. The idle card names the producer and
computer and shows elapsed time; the session stays in Active. Native work and
attention take precedence, while reply-only completion badges wait. Settlement requires the scoped reply ticket
and, for a result, a confirmed delivery receipt in the exact waiting conversation.
The durable journal has bounded previews and stable pages. See
[Agent coordination](agent-coordination.md).
Confirmed sends clear the outgoing draft; failures retain it. After an interrupted
or uncertain send, check Activity or Terminal before explicitly allowing another
send. Saving failures are shown in the editor and retried locally; sending starts
only after its recovery snapshot has been saved.

Delivery verifies the exact run, process, conversation and native input state.
For Claude and Kimi messages ending in a literal backslash, the paste includes a
final newline so the single Enter submits the unchanged message instead of
activating the agent's backslash-Enter line continuation. This is verified
against native history; no second Enter is sent as an automatic retry.
Confirmed receipts are idempotent by request ID. An uncertain result is never
retried automatically; inspect the native UI before submitting again. A session
change or stale request cannot send into another conversation. Busy-session queue
controls follow the native agent's capabilities; unsupported operations remain
in Terminal or Native UI. Codex ordinary deferred follow-ups remain read-only
unless its native UI offers the supported promotion operation.

**Stop** interrupts the current turn and cancels recovery without terminating the
session; Escape anywhere in the workspace except Terminal does the same while
the selected session's turn is working, after first clearing a search being
typed. While recording voice, Escape stops the recording instead and preserves
its preview. Once the turn is interrupted, its prompt returns to an empty message
field for editing; a started draft is kept. Claude, Codex and Kimi use verified native
Escape; Claude sends no event for it, so HGS records the interruption once
Claude shows it. Stopped before its first reply, Claude rewinds the turn and
puts the prompt back into its own input instead; HGS then moves exactly that
text from Terminal to the message field (Ctrl+Y in Terminal brings it back) and
leaves any other input untouched. Supported DeepSeek hosts use native cancel.
Native queue behavior still applies. When Codex lists pending steers ("press esc
to interrupt and send immediately"), Escape submits them at once and any queued
follow-ups after that turn. Ordinary Codex follow-ups queued alone are not sent:
Codex interrupts the turn and returns them to its input in Terminal as one
unsent draft. Activity can send again once that input is empty, so submit, edit
or clear the draft in Terminal first. HGS notes the follow-ups the Codex panel
lists when Stop sends Escape; while Codex reports that turn interrupted and its
input holds exactly them, Activity says the queued follow-ups are back in Codex's
input. An edited draft, any other draft and follow-ups Stop did not see listed
keep the general notice to return Terminal to an empty prompt.
Uncertain interruption is not repeated. Stop notices name the original session
and computer even if you navigate away. If the turn has already ended or is
waiting for input, no interruption is sent and no action is needed; Zerus refreshes
the status and briefly shows that outcome. Other failures retain the diagnostic
and ask you to refresh status or check Terminal before trying again. The status
bar's **Dismiss notification** button clears a notice without changing the session.

When Claude suggests your next message after a turn, Activity shows it as the
empty message field's placeholder, as Claude's terminal does; Tab inserts it.
Sending a message, a new turn or a new conversation removes the suggestion.
Claude writes it with an internal agent, which is neither listed as a subagent
nor shown in Activity. Claude's other internal agents, such as the ones that
summarise a running subagent's progress, report without starting as a subagent
or naming an agent type; they are not listed either and never keep a finished
session Working.

File links retain their visible path and source position. Relative references
resolve against that session's folder. On Windows, references may also name a drive
path (`C:\work\a.cpp:12`, `C:/work/a.cpp` or `file:///C:/work/a.cpp`); a share
(`\\server\share\…`) never opens. The file dialog shows and copies a path on this
Windows computer with backslashes (`C:\work\a.cpp`); remote machines keep their
`/` separators. Local references can open the file or its parent folder; remote
references identify the host and offer **Download…**, **Download and open** and
an SSH terminal. **Download…** asks where to save. **Download and open** saves
automatically in the local Downloads folder under `Zerus Files`, then opens that
copy. A completed, unchanged copy recorded for that exact machine and remote path
is reused without another transfer, including after restarting Zerus. The dialog
then offers **Open local copy** and **Show file** (selected in Finder or the file
manager), even when the remote machine is offline. **Download again** explicitly
fetches an updated copy; opening a saved copy does not check whether the remote
original has changed. Missing or edited cached files are not reused. Files saved
by older versions without a recorded source are not guessed from their names.
Fresh downloads preserve existing names with numbered suffixes such as
`report (1).pdf`; simultaneous requests for the same source in one window share
one transfer.
Downloads use that machine's configured SSH profile and SFTP, stream directly to
disk and publish the destination only after a successful transfer. Errors remain
in the dialog; a saved copy can be opened with **Open local copy**. Offline or
unresolved relative references explain why downloading is unavailable. The
download continues if its dialog is closed, with a completion or error notice;
an explicitly requested open still happens after successful saving. The
120-second transfer deadline leaves an existing destination unchanged on failure.

**Settings → Files** controls the local downloaded-copy cache. **Never clear** is
the default. An optional size limit removes the least recently used completed
copies created by **Download and open**, without touching manual downloads, unrelated files or
copies whose contents were edited. An internal index tracks ownership; files
that have been moved or whose contents changed are no longer cleanup candidates.
Transfers in progress and recently used or completed copies are protected for
one minute so the default application can consume them. Usage may briefly exceed the limit, including a
single oversized file; periodic cleanup runs while Zerus is open and on its next
start. Enabling or reducing a limit requires confirmation. Cleanup and inventory
run outside the UI thread.

`hgs @host download --path REMOTE_PATH --output LOCAL_FILE` provides the same
transfer from the CLI, including empty files and paths starting with `~/`.
Display alone downloads or launches
nothing. Copying the path keeps the dialog open so you can copy first and then
choose an opening action. Desktop dispatch happens after the reference dialog is
destroyed so Wayland activation does not depend on a disappearing window.

Zerus opens files the way a browser treats downloads. Conversation attachments,
draft attachments, downloaded copies, local file references and their folders
open in their default application. Anything that would run a program is selected
in the file manager instead, with a notice that says why. That includes:
- file types that run programs, scripts or installers, such as `.exe`, `.bat`,
  `.ps1`, `.url`, `.app`, `.command`, `.pkg`, `.dmg`, `.desktop`, `.sh`, `.jar`
  and `.py`, plus types Windows itself rates as dangerous;
- macOS bundles;
- on Windows, names without an extension;
- on Linux and macOS, executable files without one, and programs or `#!/…`
  scripts recognized by their content.

A link is judged by its target. On Linux, when no file manager answers, Zerus
opens the folder instead of selecting the file.

Files from other machines carry the system's downloaded-file marker. These are
attachment copies opened from Activity, and every completed download as soon as
it is saved. The marker is Mark of the Web for the Internet zone on Windows and
quarantine on macOS, which keeps an earlier approval. SmartScreen, Office
Protected View and Gatekeeper therefore still apply when such a file is opened
later. Windows skips the marker when policy disables zone information. Linux has
no such marker, and cached attachment copies there are never executable.

### Questions and approvals

Activity exposes supported native questions with their full visible disclosure,
options, queue position and request time. It never submits a highlighted option
without a user action. Answers are scoped to the unchanged run, process,
conversation and request hash; uncertain input is not automatically retried.
Unsent freeform answers, choices and the current question page are also saved
locally. They restore only for the same machine/session, run, conversation and
question hash. Interrupted submissions remain locked until reviewed; confirmed
queued answers keep their submitted state after a GUI restart.

Questions the agent wrote render its Markdown like replies in Activity: the
prompt, details and option descriptions, at the card's text sizes. A link opens
only when clicked: web links in the default browser and file references in the
file dialog. Other link targets, raw HTML and images remain text. Approval and
trust disclosures and panels read from Terminal, such as folder trust and resume
prompts, are shown exactly as reported.

Question cards fit the current question's content, up to 600 px or 60% of the
Activity area's height, whichever is smaller. Longer prompts and options scroll
inside the card; freeform answer fields and actions stay visible. Short questions
remain compact, and resizing preserves answers and space to read Activity.

Explicit **Submit** on a supported Claude question replaces its native Other
draft with the form answer, including clearing it for a preset-only answer.
Polling and inspection never edit native drafts. The exact form payload and
matching native acknowledgement determine delivery; uncertain attempts are
never replayed automatically.

With Claude Code 2.1.296 or later on Linux, macOS and Windows, hgs also
registers its own `PermissionRequest` hook for AskUserQuestion and tool approvals,
`hgs __state answer-channel`, beside its usual hooks. The entry an earlier hgs
registered for AskUserQuestion alone is widened in place.
- It waits next to Claude's question panel and is bound to the one exact
  pending question. **Submit** hands the answer to Claude through this hook
  instead of the terminal, so the question can be answered at any terminal
  size. `hgs answer` receipts then report `transport` `claude_hook`.
- It answers only for the saved conversation, and one hook process owns each
  question.
- If the hook does not take an answer within three seconds, delivery falls back
  to the verified Terminal path (`claude_tui`).
- Once the hook has taken an answer, that answer is never sent another way. If
  Claude does not confirm it, delivery is uncertain, the card stays locked and
  further answers are refused until the question is resolved in Terminal.
- An answer, decline or cancellation in Terminal ends the hook, and so do a run
  change and its one-day deadline.
- Sessions started before the hook was registered and other agents keep
  Terminal delivery. On Windows the hook runs only in Claude Code's exec form;
  settings that still use the shell form, written for an older Claude Code,
  keep Terminal delivery. Tool approvals through this hook are described below.

A disabled **Submit** always shows why, including while a saved answer is still
loading.
Desktop question submissions, including skips and approval choices, require
**Submit with full context** confirmation when the prompt cache is cold or its
estimated lifetime has expired. Resuming with the full conversation can increase
token usage, cost or subscription usage. **Cancel** keeps the form answers.
Compaction or clearing can invalidate the pending question, so this confirmation
offers neither action. A changed question, session or draft cancels consent.
Kimi's native cold-cache chooser already provides the explicit cost and context
choices. Its selected row is submitted directly, without a second full-context
confirmation; the same identity and durable delivery checks still apply.
App-owned desktop and Android dialog action rows place **Cancel**, **Close** or
the equivalent dismissal at the far right, after the available actions. Long
Android rows wrap, keeping dismissal last and visible with larger text. Destructive
and full-context desktop confirmations default to Cancel; Android dialogs focus
their dismissal after the dialog window gains focus. Enter on a valid desktop
editing form may save or create; Escape/Back dismisses without submitting.
Accepted operations that cannot safely be abandoned keep their existing pending
locks; closing a review never repeats an uncertain command. Destructive buttons
name their action and the dialog identifies its original target. System file,
permission and installation prompts follow the operating system's own layout.

Unsupported or truncated native prompts remain in Terminal.
Claude wraps prompts, options, review answers, key hints and Bash commands to
the terminal width and splits words longer than a row. Zerus matches the wrapped
rows against the exact request at any width; only a clipped panel needs a
larger Terminal.

Claude keeps no scrollback for a question or approval panel taller than its
pane, so a long question or command can lose its title and beginning
everywhere. Kimi question dialogs and Kimi 2.1's startup folder trust prompt
can lose their title the same way when their disclosure is long. Kimi 2.2
pages its trust disclosure, or asks for a larger terminal, when its pane is
short; Zerus offers the trust choice only once the whole disclosure shows on
one page, and never chooses for you. A session no client views keeps tmux's
default 80x24 size, and so does a pane of the Windows session host. While a
recorded question or permission request waits and such a pane shows a clipped
panel, Zerus makes that pane taller, doubling its height up to 192 rows without
changing its width, until the agent redraws the complete panel. The exact
request match still decides; the next client that attaches sets the size
again. A pane a client views keeps that client's size, so its clipped panel
stays in Terminal, as it does under a Windows session host older than this
behavior. Only a question listing, such as `hgs inspect`, grows a pane, and
never while an answer to that run drives it: both hold the run's answer lock.

Claude tool approvals keep the session at **Needs approval** and appear in
Activity while Claude shows its chooser. When the complete panel matches the
agent's request, such as the Bash command, file name or fetched host, Activity
lists Claude's options verbatim after a short review delay. Options that reach
beyond this request, such as always allowing, blocking, session-wide grants or
mode switches, are marked and need a second **Confirm** click. Choosing a **No**
option ends Claude's turn as in Terminal; the composer reopens after Claude's
prompt is verified. A clipped or unrecognized panel, a mismatched request and
plan approval stay visible with **Open Terminal**. Claude reports no answer
given in Terminal, so Zerus settles it from the screen alone whenever a status
listing, `hgs inspect` or `hgs read` looks at the session: Claude's
interruption ending the turn records **No** or Esc, and the approved Bash
command shown running with its elapsed time records **Yes**. Until Terminal
shows either, and for an approved tool that is not a Bash command, the session
stays at **Needs approval** until Claude's next event. Only a session waiting
for such a request reads its terminal for this.

While hgs's `PermissionRequest` hook waits beside a main-agent approval, the
card answers through it, so a clipped panel no longer needs a larger Terminal.
- The hook is bound to one exact request: the saved conversation, and the one
  active tool call whose tool and input equal the request when the hook
  starts. Without exactly one such call, the request is answered on screen as
  above; a later identical call is another request.
- A paused session, a provider error, or a terminal that no longer belongs to
  the agent never lets a decision go through the hook.
- Claude sends the hook no signal for an answer in Terminal, so the card and
  the hook act only while Terminal still shows a chooser. A complete panel must
  be this request.
- The card always shows the whole request the hook answers: the Bash command,
  its description and any other field, or the whole input of another tool.
- Claude's own options appear only when a complete panel shows exactly that
  operation. That means the main agent's Bash command, with its description if
  any and no other field, and no other active Bash call. A file name or host
  proves too little. For example, a Read of another file with the same name may
  be queued in front of this one.
- Any other complete panel is answered on the screen as above, acting on what
  it shows. A clipped panel gives **Yes** and **No**. An unreadable terminal
  gives no hook card.
- A visible safety warning of any kind must belong to this request's command,
  through a proven panel or the visible end of the command in a clipped one.
  Otherwise there is no hook card.
- **Yes** allows this one tool call without changing any permission setting.
  **No** ends Claude's turn as in Terminal. **Tell Claude what to do instead**
  denies the call with your message, and Claude continues without it.
- Options that reach beyond this request go only through the verified panel,
  with the second **Confirm**. A **Yes** or **No** that the hook does not take
  within three seconds also goes through a verified panel; feedback it does not
  take is not sent.
- A decision the hook has taken is never sent again. Until Claude confirms it,
  the card stays locked and is answered in Terminal.
- `hgs answer` receipts report `transport` `claude_hook` or `claude_tui`.
- Automatic approvals use the hook only for a card whose complete Bash panel
  shows exactly that operation, with the same one-time choice.
- Subagent and workflow requests keep the screen path. Claude 2.1.296 runs
  subagents in the background and shows their requests outside the main screen.

Claude's Bash safety confirmations, including warnings about possibly empty
paths in `rm` commands, also appear in Activity with the native warning text.
A visible safety confirmation makes the session **Needs approval** even if
its permission hook was not recorded, and ordinary message sending waits for
the decision. Without a matching permission request, or when the full command
stays clipped and no permission hook waits for it, the card offers **Open
Terminal**. With a complete matching request,
Activity can submit the native **Yes** or **No** choice. Claude's unattended
denial countdown stays visible without replacing the request as it ticks.
Desktop keeps the card's scroll, chosen option and review delay through these
countdown updates. A changed command, warning or set of choices requires a new
review.

Bash approvals from Claude's subagents and workflows use the same Activity
choices when one recorded child permission request matches the complete visible
command. The answer is bound to that child request as well as the parent run and
conversation; a repeated command is a new request. Identical pending requests
from multiple children, missing hook evidence and clipped commands remain
read-only. Other child tool approvals still require Terminal. A child's answer
never interrupts the parent or clears a sibling's request; completion follows
that child's own native events. Parent progress alone does not settle child
permissions. Pending evidence is bounded without evicting an ambiguous request;
if its limit is exceeded, approvals remain in Terminal until the next native
session starts. Read-only native choices are visibly disabled, with **Open
Terminal** as their action.

Kimi plan approval (ExitPlanMode) keeps the session at **Needs approval** and
shows the complete plan from Kimi's own request in Activity, rendered as Markdown,
with Kimi's choices: **Approve** (or the two or three approaches the agent
proposed), **Reject** and **Revise** with a one-line feedback field. A plan longer
than 8,000 characters is shortened and names its saved plan file. Zerus answers
only while Terminal shows exactly that chooser, using its number key and, for
Revise, pasting the feedback before Enter; Kimi's recorded decision, including
the exact feedback, confirms delivery. A clipped or different chooser, or one
already in Revise feedback mode, stays read-only with **Open Terminal**.

Other Kimi tool approvals also appear in Activity, with the complete request from
Kimi's wire. Kimi's hooks cannot approve, so Zerus answers only in Kimi's own
panel, and only for a Bash command: the panel must draw that exact command,
working directory, description and danger warning row by row, wrapped as Kimi
wraps it, and the complete history of every agent of the conversation may hold
no other unresolved Kimi request, however old: nothing proves that Kimi no
longer shows one. A history longer than 8 MiB (4 MiB for another agent), a
record Kimi is still writing, a line that is not a JSON object with one string
type, an interaction record without its ID or of a kind Kimi does not write,
or a repeated request ID keeps the approval read-only. Types and IDs are
compared as decoded JSON. Every key and pasted feedback line first finds the same
unresolved request, with its active tool call, in the same run, process and
pane. The
choices are Kimi's: **Approve once**, **Approve for this session**, **Reject** and
**Reject with feedback**. The session choice asks for confirmation: Kimi then
stops asking about that exact command and also approves every other request
already waiting with the same action, which shows only a command's first 50
characters. Kimi's recorded decision confirms delivery. Other tools, commands
Zerus cannot draw exactly, a narrow or clipped panel, and an unverifiable history
stay read-only with **Open Terminal**. Automatic approvals never answer Kimi.

Claude Code can also ask its own questions without an agent tool call, such as
whether to switch models after a safeguard stop. Activity shows the question and
its options verbatim with **Open Terminal** while Terminal still shows them: no
tool result can acknowledge an answer, so Zerus never chooses one of these
options. When Claude waits for input and no request explains it, Activity shows
the chooser visible at the bottom of Terminal, such as **Session paused**, in
the same read-only way. A new prompt ends any earlier request.

Supported startup prompts include Claude workspace trust and auto-mode consent,
Codex folder access, **Hooks need review** and **Update available**, and Kimi
folder/MCP trust. These can appear before a conversation exists, but only with
verified startup identity.
A recognized panel must show its native title, folder, exact choices and key
hint. Its explanation is shown verbatim and pinned to the answer, so changed
native wording stays answerable: Claude workspace trust preserves its full
permissions warning, Kimi includes the visible MCP commands, and Claude's
auto-mode and resume panels keep their explanation and cost warning. Zerus adds
descriptions only for reviewed text. In a Git subdirectory or worktree, Codex
folder access names the repository root that Codex will trust. Claude wraps a
long workspace path across rows; the joined rows must still equal the launch
folder exactly.
Trust persistence belongs to the native agent.
Already-running Claude and Kimi sessions can retain the exact Bitwarden launcher
notices above the complete workspace-trust panel; Claude also accepts its
Keychain notice. These notices do not change the request identity or appear in
its disclosure.
Any other chooser before a conversation exists, such as a renamed option, an
unreviewed update command, a panel for another folder or unrelated output above
it, appears read-only with its visible text and **Open Terminal**. Zerus never
answers it, and messages wait until it is answered in Terminal.

Before the first turn, Claude's auto-mode prompt accepts absolute workspace paths
and native `~/…` abbreviations that match the verified launch directory, also
after the initial conversation is bound. Under a finished or stopped reply, the
bound conversation and that turn identify it instead. The offer must be the
complete panel at the bottom of Terminal: panel text quoted in the conversation is
never answered, and messages wait until the offer is answered.
Claude's **Resume this conversation?** chooser also appears in Activity when a
long conversation has gone cold, including between turns in a resident session.
The card preserves the inactivity, context size and five-hour usage warning.
**Resume** and **Start a new conversation** require an explicit choice; Zerus
never dismisses this warning automatically. If the native unsent draft has focus,
pick a row in Terminal first. Replies verify the exact run, conversation and full
panel before each key, and uncertain delivery is never replayed.
For Codex hooks, **Review hooks** opens the native browser in Terminal;
**Trust all and continue** and **Continue without trusting** are explicit choices.
Review input with an uncertain result is never retried automatically.
Codex's **Update available** picker shows both versions and the exact update
command. **Update now** switches to Terminal while Codex runs that command; Codex
then exits and the session needs an explicit resume. Codex leaves the picker and
input mode at once and prints `Updating Codex via` with the command before running
it. That announcement for the exact command, or Codex exiting, confirms the choice
within 15 seconds, however long the installer takes. Zerus does not wait for the
installer: the `hgs answer` receipt's `codex_update` is `installing` while Codex
runs it, or `exited`. Without either confirmation, delivery stays uncertain and
Enter is never sent again. **Skip** and **Skip until next version** continue
startup, and Codex records the dismissal itself. Only reviewed npm, Bun, Vite+,
pnpm, Homebrew and standalone Unix commands are recognized; other or clipped
commands stay in Terminal.

After **Update now**, the desktop shows a notice for that state: Codex is still
installing its update, or it has exited and the session needs a resume. An
older host's receipt has no `codex_update`; the desktop then only switches to
Terminal.

After a resume, Codex asks **Resume paused goal?** when the conversation's goal
is paused, stalled or limited by usage. Activity shows the goal from Codex's goal
database once the centered dialog over the transcript shows the same goal and
both choices; the session waits for this choice instead of Restoring
conversation. **Resume goal** marks the goal active, and Codex continues it by
itself when idle. **Leave paused** keeps it paused and the conversation waits
for your message. A dialog too narrow to read, or for another goal, stays in
Terminal.

Optional Codex questions preserve the regular composer and support **Skip**.
When Codex's own question panel is open, Zerus answers or skips there. Codex's
animated working status above the panel does not block an answer; any change
to the panel itself does. An open panel taller than its pane keeps its title
and scrolls the answers, naming the visible row in its key hints (`option 1/6`).
In a pane no client views, a question listing grows the pane as it does for
Claude until the whole list shows; a title taller than the pane stays in
Terminal. A long answer wraps in Codex's Other field, and Zerus compares that
field, wrapped rows joined, with the text it pasted.
Queue arrows navigate optional requests from newest to oldest while retaining
answers and the current multipart section. A required approval temporarily takes
priority. Confirmed answers appear as **You (answer)** in Activity. Codex optional
question replies show the question and your answer in separate sections,
preserving the literal answer text.

If an optional answer enters Codex's native queue, the question stays visible as
**Submitted / Awaiting agent** with the original answer locked. Submission means
the verified terminal received the answer; it does not mean Codex has recorded
it yet. This state survives polling and GUI restarts. The card disappears after
the exact native question reply appears in the conversation. Zerus never resends
the answer or promotes the queue automatically; **Send now** remains an explicit
native queue action. Delivery without confirmed submission still requires
checking Terminal before retrying.

### Models, effort, context, goals and subagents

The Activity model selector changes supported per-session model/effort settings,
leaving account defaults intact. Codex/Kimi options come from the selected
account's native catalog. Before the first message, verified empty native input
can allow selection without creating a conversation. Startup prompts, drafts,
unconfirmed resume and an already submitted first message block that operation.
Unverified states fall back to Terminal with an explanation.
A choice that the agent's status line already shows is confirmed without typing
anything into Terminal. A change that fails after the agent visibly read every
typed key and returned to its empty prompt leaves nothing unconfirmed, so another
change or a message can follow, including a new session's first message. When the
agent has not reacted to a typed key, further changes and messages wait for its
next native event: finish or cancel the change in Terminal and send a message there.

Codex model choices use the selected account's `models_cache.json` and sort by
ascending signed 32-bit `priority`, preserving source order for ties, as in
[Codex's native picker catalog](https://github.com/openai/codex/blob/rust-v0.162.0/codex-rs/models-manager/src/manager.rs#L170).
Missing or invalid priorities follow ranked entries in their original order;
a legacy catalog without priorities keeps its source order. This shared catalog
order applies to both desktop and Android without changing the current model.

Claude options are the models its `/model` picker lists, from the catalog Claude
Code caches in the account's profile. Zerus selects the model and effort in that
picker and applies them with its session-only choice (`s`). Its Enter, and a typed
`/model NAME` or `/effort LEVEL`, would save new defaults and are never used. The
change is confirmed by Claude's receipt in the exact conversation transcript and by
its effort indicator when shown. In a conversation that already has replies, Claude
warns that the next message re-reads the full history and uses more tokens: the
selector states this before **Apply**, and Zerus then accepts only that built-in
notice. A confirmation requested by a `PreModelSwitch` hook is declined and
reported, as is a picker without the session-only choice; use Terminal for those.
With Claude Code 2.1.296 or later, its `PostModelSwitch` hook names the exact model
of every switch at once: `/model` in Terminal, a change from Zerus, an automatic
switch or a resume. The selector shows that model until the transcript records a
later turn or `/model` receipt; the switch itself adds no Activity entry.
A stopped Claude session resumes with a choice made in Zerus through `--model` and
`--effort`; otherwise it starts with the account's settings.

Busy-session choices, and choices made while the terminal shows a chooser, remain
**Pending** until the agent is ready, including after navigation away. If a
chooser appears while the desktop applies a choice, the choice stays **Pending**
and the desktop tries once each time the session becomes ready again. A failed
or refused application, a new run and a new conversation end these attempts. A
message sent before application uses the current model.
**Save for resume** stores a stopped session's choice without starting it. A stale
queued setting cannot replace a newer choice from another device.
If a message or another client already consumed or replaced it, Zerus refreshes
the current settings without retrying the stale request or showing a failure.
`hgs [@host] settings SESSION --json` exposes the same scoped operation.

Native clear/compact operations are capability-gated and require exact identity.
When they are unavailable, `hgs inspect` reports why in `clear_context_reason` and
`compact_context_reason`, empty while the action is available. A busy session names
what keeps it from Ready: the main agent's turn and tools, and each working subagent
with its current tool, up to three, then a count. The actions follow the status the
session list shows, so Kimi profile groups still working after the main turn ended
keep them unavailable and are named by profile, such as `2 “code” subagents`.
Applying settings and forking a running session wait for the same Ready, and a
profile hook entry that Kimi's native subagent catalog has replaced holds back
none of them. A chooser recognized only on the terminal, such as Kimi's context
choice after an idle period, keeps clearing, compaction, forks and effort changes
unavailable until it is answered; `fork_reason` and `effort_change_reason` give the
same reason as `clear_context_reason`. A model settings change made meanwhile is
saved as **Pending** (`settings_apply_when` is `ready`), and no key reaches the
chooser.
The desktop shows the reason as the tooltip of the disabled **Clear context**,
**Clear session** and **Compact and continue** actions, and as a line in the
cold-cache confirmation.
Hosts from before these fields leave the actions disabled without a reason.
**Clear session** requires confirmation naming the session and host, preserves
unsent drafts and rechecks identity. While Claude shows an approval, question or
another chooser, **Clear session** asks for a separate **Dismiss and clear**
confirmation: Zerus presses Esc once, as in Terminal, so an approval is denied and
a question stays unanswered. It runs `/clear` only after the chooser closed and the
empty native prompt is verified, and never repeats an uncertain attempt. Startup
choices such as resume, permission mode and folder trust are answered in Activity
instead. Claude reports no hook when **Stop** ends its turn, so the session stays
**Interrupted** until its next message. **Clear session** is available there once
no tool or subagent still works and the empty native prompt is verified, as for
that message; a native draft keeps it unavailable and is named as the reason.
Compaction still waits for Ready. DeepSeek context reset is unavailable until
its native Session Controller provides the supported operation. A confirmed clear
adds an amber **Session cleared** boundary to Activity and keeps the earlier local
timeline above it: the agent no longer sees that history, but you can still read
and search it. Resuming or opening another conversation starts a new timeline.
Codex can defer its new-session hook until the next message; its verified empty
native reset panel also confirms the clear, including `/clear` in Terminal. Only
the header directly above the empty composer counts: the same header above an
earlier message and its answer does not. Old
context and prompt-cache values are hidden until the new conversation reports
native usage. The saved native identity changes only on the real hook, and an
unconfirmed command does not reset counters.
A stopped session offers **Start fresh…** instead, with the same confirmation, and
keeps **Send** available: it offers **Resume and send** with the full saved context,
**Resume, compact and send**, or **Start fresh and send**. The compact option waits
for the restored conversation to accept input, then runs native compaction and
delivers the unchanged message only after confirmed success. A failed start, an
unexpected conversation or three minutes without readiness cancel that send and
keep the draft.
**Compact and continue** sends only after confirmed successful compaction with the same
unedited draft; errors, cancellation or identity/draft changes cancel submission.
While awaiting startup or compaction, **Waiting to send…** keeps the accepted text
and attachments visible and locks edits. The field still supports selection and
copy. **Cancel send** restores editing and keeps the complete draft; it cancels
the automatic message, while startup or compaction continues. **Stop** interrupts
the agent. A GUI restart keeps the draft without resuming automatic submission.
Switching dialogs, changing filters or closing the workspace keeps accepted sends
pending on their original sessions. They continue in the background, verify the
same run/conversation and compaction request, and never submit a second copy.
Context metrics use native telemetry, without guessing a capacity from a model name.
Claude's window comes from its status line (see
[Tracking and durable state](#tracking-and-durable-state)): `session_usage.context`
gains `limit`, `limit_source` `native_statusline`, `limit_model` and
`limit_observed_at` only while the latest report belongs to the session's run and
conversation and names the model that the inspection reports. After a model change
the window stays hidden until Claude reports the new model; after a clear Claude
2.1.296 reports at its next update, usually the next reply, and a resumed run
reports when it starts. Compaction changes the used context, not the window.
Claude's recorded compaction boundary replaces the earlier context size with its
reported summary size. It invalidates the earlier prompt cache estimate; a new
warm countdown requires fresh native usage with a reported TTL.

**Tasks & agents**, **Details** and **Worktrees** occupy the resizable inspector.
Its width, tab and open state are local; toggling it preserves Activity, drafts and
Terminal attachment. Successful native task/plan tools supply recorded pending,
in-progress and completed items. Tool failures and Stop do not complete tasks.
There are up to 100 items per list and 65 lists, with truncation disclosed.
Old data shows **Last recorded**; unavailable history is not reconstructed.

Goals remain independent of turn state. Codex uses `goals_1.sqlite`, Claude uses
`goal_status` in the exact transcript, and Kimi uses native `goal.create/update/clear`
wire events. DeepSeek exposes its native phase and continuation permission.
Only recorded usage and budgets are shown; missing data is never inferred from a
prompt. Ready does not complete a goal.

Subagents open their own Activity and return through **Main activity**, preserving
the parent's draft. Individual child IDs and profile groups are distinguished;
unknown counts remain unknown. An unknown child never keeps its parent from Ready.
A Claude child that stops on a provider error, such as a usage limit, gets no
SubagentStop: its StopFailure, or a `failed` entry for its current start in the
workflow journal with no newer child event, settles it as **Error** with that
message, and a later event of the same child resumes it. Ordinary child
results/errors stay out of the attention count, while explicit input/approval
requests remain visible. DeepSeek
continuable children can receive messages through the native API with parent
verification; other adapters and one-shot children may expose history only.
The expanded session tree hides children whose latest reported turn is finished
or failed by default. Session and Overview cards then show only the active count, omitting
an empty indicator when no active or uncertain children remain.
**Settings → Sessions → Show completed subagents** restores completed rows and
active/total card counts. The **Tasks & agents** inspector always shows the full
reported roster and its active/total counts, including completed turns.
Unknown states, paused agents and input/approval requests remain visible.
An open child's Activity and draft stay in place when its turn finishes. A child's
draft keeps its row visible after returning to main Activity. Finished turns are
not proof that the agent's task or process has ended.

Claude child tracking reconciles hooks with exact native history, including
workflow children. A final assistant `end_turn`, a successful `toolEndsTurn`
result bound to its exact owned tool call, or a final **Request interrupted by
user** row can settle a turn even when `SubagentStop` is missing. Its **for tool
use** variant settles only when it follows the owned error results of the
stopped calls; an unbound marker changes nothing. A tool-use interruption that an
earlier hgs version recorded as new child work settles from the same native row
unless a newer hook arrived. Matching
workflow `started`/`result` attempts independently confirm the same reply.
Later native child activity can recover a missing start; newer hooks take
precedence. Silence and a completed parent never prove child completion.
Reconciliation preserves existing reply and read identities. `hgs inspect`
reports each child's `tracking_evidence`: source, event kind, event time,
native row identity and, when verified, workflow result confirmation. Missing,
partial or ambiguous native evidence leaves the last reported state intact.

Codex child tracking verifies the child and parent IDs in the account-owned
rollout, then reconciles typed turn starts, successful completions and explicit
interruptions. An interruption without a turn ID requires a matching start in
the bounded native history. Kimi's version 2 session catalog identifies direct
children individually, including several children using the same profile;
their own wire turn events establish current work or completion. Older,
delayed or ambiguous catalogs retain profile-hook counts. A removed catalog
entry becomes unknown rather than a fabricated completed result.

DeepSeek uses each child's exact native API entry within one host generation.
A child never inherits its parent's running status. Missing, contradictory or
cross-generation observations remain unknown; desktop and Android counters
retain the `+` uncertainty marker. OpenCode child sessions are not shown in this
version ([OpenCode controls](opencode.md#controls-and-limits)). Inspection reports native
tracking evidence without changing reply/read identities or restarting agents.

**Fork session…** creates an independent native conversation in the same folder,
next to the original session in its group. Files remain shared. The CLI is
`hgs [@host] fork SESSION [-n NAME] [-d] [--archive ID]`; support depends on the agent.
DeepSeek uses the native fork of a supported resident host
([DeepSeek](#deepseek)).

Codex may defer `SessionStart` until the fork's first message. Activity enables
Send once Zerus verifies that the supervised process owns an independent child
history linked to the source conversation and its native composer is ready.
The child ID is confirmed by the agent's own event; the source session stays intact.
On Windows, verification searches only the selected account's history and requires
the same supervised process to hold exactly one matching child history open.
Incomplete, unreadable, ambiguous or exhausted verification keeps Send unavailable.

<a id="projects"></a>

## Projects and P2P synchronization

A project has a stable ID, name, color, folders on multiple computers and session
membership. Drag sessions between projects without moving folders or replacing
agent processes. Drag headers, use the context menu or Alt-Up/Down to reorder
projects. The project order is shared: every computer in the swarm and the
Android app show the same order, and the latest change from any desktop replaces
the others without a review. Until someone reorders, each desktop keeps its own
order and Android lists projects by ID. Empty-project visibility, collapse state,
session order within a project and default project are local.
**All swarm projects** explicitly shows the full shared catalog.

A newly discovered session appears in the local default project until a shared
assignment arrives. Discovery alone does not publish that fallback as a project
assignment, so another desktop can launch the session into its chosen project
without racing an automatic Inbox assignment. Launching or explicitly moving a
session publishes its project choice, including a deliberate move to Inbox.
Update the desktops that observe sessions to receive this behavior; existing
catalog assignments and unresolved conflicts remain available for review.

Folder entries contain a machine, path and optional label. Browse works for local
and remote folders, including ordinary non-Git directories. The desktop folder
browser offers **New folder** on the selected remote computer. Enter only the
child folder name directly in the list and press **Create** or Enter; Escape
cancels the inline entry. The list context menu and Ctrl+Shift+N (Command+Shift+N
on macOS) open the same entry. The **Dialog…** button beside **New folder** opens a separate
name-only dialog with the parent shown above it. A missing path entered in the
browser supplies that form's parent and initial name; the parent must already
exist. Once
creation is confirmed, choose **Use this folder** to select it for the project
or session. Browsing and cancelling the creation form do not create anything,
and existing paths are never replaced. This requires an updated `hgs` on the
selected computer; older peers can still browse existing folders. If creation
cannot be confirmed, the browser checks that exact path without repeating
creation. An available folder can be selected immediately, including an empty
folder or one that already existed; an unsuccessful check retains the creation
error and **Go** remains available. Creating a folder does not initialize Git
or start an agent.

The CLI equivalent is `hgs [@host] dirs --create <path> [--request-id UUID]`.
It creates one directory and returns the canonical path plus the exact request
identity as JSON. It never creates missing parents or repeats an uncertain
operation. Local creation rejects `--dry-run`; remote dry runs only print the
transport command.

The desktop folder
list starts in alphabetical name order, ignoring case; column headings change
the sort. Sorting preserves the selected folder and does not reorder the shared
catalog. The divider below the list adjusts the folder/worktree heights and
remembers that choice locally; additional window height goes to the folders.
Worktree actions share its heading row, leaving more space for the folder list.
Right-click a folder and choose **Move to** or **Copy to**, then the destination
project. Move transfers its catalog link; Copy keeps the original and adds a link
to the other project. The computer, path and label are preserved, including for
offline computers. If the destination already has that folder, its existing link
is reused. Files and session project assignments stay in place. These catalog
changes use the normal Swarm synchronization.
An untouched **Inbox** selection in New session follows an unambiguous project
whose folder is confirmed on the selected computer. Desktop, Android and iOS
use exact canonical folder membership or a complete, current worktree catalog;
folder names and path prefixes do not assign projects. Explicit project choices,
including explicitly choosing Inbox, win. Multiple matching projects or missing
verification keep the current selection. An untouched Inbox stays a local fallback,
so later native project discovery cannot be overwritten by launch bookkeeping.
Older saved desktop launch intents retain their original placement semantics.
Desktop also checks existing saved folders on that exact computer against the
CLI-confirmed canonical path, including local Windows drive and UNC folders
that remain outside the POSIX shared catalog. It does not resolve saved aliases
or infer from a parent folder. A native project hint must name the same known
Swarm catalog; a foreign hint never selects a same-ID local project.
A new tracked CLI session also infers its initial project from that computer's
existing catalog; resume, fork, adoption,
manual assignments and explicit unassignment are never reassigned. This optional
metadata step does not initialize a catalog, join a swarm or create a repository.
The general **New session** action inherits the selected session's project,
computer and matching project folder. For a linked worktree, a verified catalog
selects the main checkout instead. Choosing another folder, project or computer
cancels this automatic selection; explicit folder and worktree launch actions
keep their requested path.
Project switches in **New session** keep the machine and select that project's
folder. Only a path explicitly chosen through **Browse…** stays pinned per machine
within that dialog;
closing it clears this temporary choice. Selecting a listed folder unpins it.
A folder outside the project shows a warning and is added only after a successful
launch with the matching launch ID. Cancellation and launch failure add nothing.
A worktree of a project folder on that computer is not outside the project, so
starting there adds no folder; **Choose existing…** offers it again. When an
automatic worktree is generated from an outside folder, the session starts in
the worktree and the outside folder, not the worktree, joins the project.

New launches immediately show **Starting…** until the matching session appears.
Controls stay disabled before confirmation. Errors remove the placeholder and
show a reason; **Hide launch status** hides it while preserving a late session's
project placement. A session that ends before it appears and leaves no record,
such as an agent that cannot start after the launch reported it started, also
removes the placeholder: a notice shows its exit code and last lines, as its
computer reports them in `ended_launches` ([JSON and external
clients](#json-and-external-clients)), and stays when the launch command
reports afterwards; a hidden launch status ends silently but still shows a
later launch error.
Existing names and unrelated sessions are never repurposed.
**Open terminal window** is off by default and remembered separately from the tray
open mode; DeepSeek uses its embedded native interface.

To join a swarm, configure SSH access and open Zerus on the initial machines so
existing projects import once. Projects shows **Connect project catalogs…** when
an accessible computer has no project-sync connection. Session access over SSH
alone does not connect project catalogs: the guide opens **Connect computer**
with the missing computer selected. Choose **Preview connection**, review project
mapping and **Join and merge projects**. Nothing is paired or joined by opening
the page. The same flow remains under **Projects → Swarm… → Connect computer**.
Names alone do not merge projects. Existing swarm membership wins by default;
turn off that preference to compare conflicting assignments. HGS backs up the
catalog before a merge. **Connect this member** adds a route to an existing member.

There is no permanent leader: each member stores the shared catalog and can relay
changes. One configured SSH connection is enough for exchange in both directions;
the other side needs no reverse SSH connection. Learning a machine ID through the
catalog does not create local transport access to it.

Shared fields are machine IDs/names, project IDs/names/colors, folder locations,
session membership and the phone notification timing. SSH hosts, users, ports,
keys, proxies, credentials, transport bindings, local order, filters,
collapse/appearance and the default project stay local. File contents and agent
history are not copied. Offline hosts do not hide projects with locally configured
access.

The catalog and local bindings live in `~/.config/hgs/swarm/catalog.json`; do not
copy that complete file between machines because it contains node identity and
local transport bindings. `hgs swarm export` exports only shared fields. Original
Zerus settings stay in `workspace/organizationBeforeSwarm`; import/merge backups
use `~/.config/hgs/swarm/backup-*.json`.

The independent `hgs-swarm` service (`com.hgdev.hgs-swarm` on macOS) makes no network
requests without members. Local changes normally trigger exchange within three
seconds; unchanged peers are checked every 30 seconds, backing off to five minutes
on failure. Zerus reads the local catalog every five seconds without SSH. Offline
members catch up later. Independent fields merge; conflicts remain in the Swarm
review UI. A session's missing/Inbox placement is a fallback: a concurrent choice
of one known live concrete project wins without review, and identical concrete
choices coalesce. A newer explicit move to Inbox or unassignment still wins by
causal identity. Different concrete projects and unknown/deleted project targets
remain reviewable; project mappings are applied before join comparisons. Local
passive observations never publish membership, including during import. All
underlying versions are retained, so the next explicit choice consumes every
observed candidate. Project/folder deletion records still prevent resurrection;
this version does not prune old journal records.

The worker records its version and process in `~/.config/hgs/swarm/worker.json`
and exits when an upgrade replaces its executable, so `Restart=always` or launchd
`KeepAlive` starts the new version. A worker started before this mechanism keeps
running after an upgrade and cannot read a catalog that holds the phone
notification timing. `hgs swarm get`, `preference`, `sync`, `join` and inbound
`exchange` restart such a worker, or one from an older version:
`systemctl --user try-restart hgs-swarm.service`, or
`launchctl kickstart -k gui/$UID/com.hgdev.hgs-swarm` on macOS. They try at most
once every ten minutes and only for the default configuration directory, never
under `HGS_CONFIG_DIR`, and never touch agents, tmux servers, DeepSeek hosts or
`hgs-recovery`. `hgs swarm get` reports `worker: {running, current}`; when a
running worker stays outdated, Settings → Mobile connection asks you to restart the
sync service. Windows has no service manager for the worker; the desktop app runs
it there ([Windows](#windows)).

**Swarm → Conflicts** explains the affected session, archived session, project or
folder. Compare the values and the computers that saved them: projects, project
names and colors appear as the project labels and computers as the machine badges
used elsewhere, with **this computer** beside this desktop's badge. A computer the
catalog does not list stays text with its identifier. **Shown now** marks
the temporary display choice; it does not mean the conflict has been resolved.
Select a row to preview the result, then use **Keep this project** or the matching
action. Project assignments change catalog placement without restarting agents
or removing conversations. Folder/project removals affect the shared catalog,
leave files and sessions intact, and require confirmation. The decision is shared
with connected computers and reaches offline members when they reconnect. Closing
the window postpones the decision. New conflicting versions clear the selection
and require another review, including if they arrive during a confirmation.

```sh
hgs swarm get
hgs swarm preview mac
hgs swarm sync
hgs swarm disconnect mac
```

Disconnect stops outbound exchange through that connection while retaining the
catalog and session access. It does not deny inbound exchange from an existing
member; SSH controls transport access. `HGS_SWARM_SERVICE=0 ./install.sh` skips
worker installation.

### Phone notification timing

`hgs swarm preference get --json` prints `{"mobile_delivery": "away"}`;
`hgs swarm preference set mobile_delivery away_10 --json` changes it for every
member. Values are `immediate`, `away` (default), `away_5`, `away_10`, `away_15`
and `away_30`. Concurrent changes settle on one value without a review; the next
change from any computer or the phone replaces them. A machine joining a swarm
adopts the swarm's existing choice. Without a swarm the local catalog keeps it the
same way.

Members running an older hgs keep synchronizing projects, folders and sessions but
do not receive this choice or the shared project order (they keep their own
order); Settings → Mobile connection names the computers this
one synchronizes with directly until they are updated. A member reached only
through another computer is named in Settings on the computer that connects to it.
An older machine cannot preview or join a swarm whose catalog already contains the
choice; update it first.

An older member does not relay the choice either: a machine that synchronizes only
through it receives the choice after that member is updated. Each synchronization
with an older member exchanges the full inventory, because its catalog never
matches a catalog that holds the choice.

Once a delivery choice exists, an older hgs on the same machine cannot read the
swarm catalog, and leaves it untouched, until hgs is updated again.

### Legacy CLI folder aliases

`~/.config/hgs/projects.local` retains `NAME=FOLDER` aliases for CLI compatibility.
The old infrastructure-owned `~/.config/hgs/projects` is no longer read. Aliases
import into Zerus once per machine; later project edits belong in Projects. An
alias whose folder that machine cannot take (not an absolute path in its form)
starts no project. Projects names it, and the machine imports again at the next
start of Zerus.

```sh
hgs project ls --json
hgs project add infra ~/work/infra
hgs project set infra ~/work/other
hgs project rm infra
```

Alias names reject spaces, `/` and `=`; logical project names are not restricted
that way. Absolute paths remain available directly from the CLI.

### Cloning a repository

In desktop **New session**, choose **Working folder** for an existing checkout or
ordinary folder, or **Git repository** to prepare a new checkout. Project,
computer, agent, account and session name stay in this one form. The Git source
accepts an SSH or HTTPS URL from GitHub or another Git server. **Clone to** shows
the suggested complete destination; edit it to rename the target folder or use
**Browse…** to choose its parent. A manual destination stays when the URL changes.
Parent folders are remembered separately for each computer.
Switching sources keeps the shared controls and action buttons in place.

Select an existing logical project, Inbox, or **New project…**, then use the
single **Clone and start** action. Progress, access errors and recovery controls
stay inline. Switching source modes or opening the form never starts a clone.
**Projects → Clone repository…** offers standalone preparation without starting
an agent.

**Clone and start** checks the selected computer's installed CLI, Git, parent
folder, destination and repository access before cloning. Access uses that
computer's existing SSH configuration and Git credential helpers; passwords and
tokens embedded in repository addresses are refused. Configure missing access on
that computer, then try again. These checks run in the background and create no
checkout. Cloning begins only after explicit confirmation. Existing destinations,
including empty folders and symlinks, are preserved: choose another name or use
**Browse…** to select an existing checkout.

A confirmed clone joins the selected project even if the agent cannot start.
Its confirmed project choice, including Inbox, takes precedence over automatic
folder-based project detection.
The normal **Auto** / **None** worktree choice applies to the new checkout. Failed
launches retain the checkout for another session. Ordinary folders remain
supported without Git initialization.

Cloning has a ten-minute Git deadline and never automatically retries an
uncertain operation. Private native receipts and local pending-request metadata
retain the original identity across dialog or application restarts. **Check
result** reads that original receipt; a recovered checkout requires an explicit
**Use checkout** or **Use checkout and start** action. Failed or partial checkouts
are kept for inspection. A timeout or lost connection is not permission to remove
files or run the clone again. **Choose another destination…** leaves the original
checkout and native receipt in place and lets you prepare a different clone.

The CLI is `hgs [@host] repository check --url URL --parent PATH --name NAME
--json`, followed by `repository clone` with the same fields and an explicit
`--request-id UUID`. `hgs [@host] repository status --request-id UUID --json`
reads its durable receipt. Reusing a request ID with identical fields returns
the original result without repeating Git; different fields are refused.
Remote acquisition requires the exact selected CLI's `repository-clone-v1`
capability. Older peers continue to support ordinary folder selection.

The native operation pins the parent and new destination through Git and checks
their identities before accepting the result. Unix Git starts in the held
destination directory, so replacing its pathname cannot redirect checkout writes.
An unpredictable private staging folder is pinned before atomic publication of
the destination name; its fallback `staging_path` is reserved in the receipt before
creation. A failed publication can leave that staging folder for inspection.
Windows creates and holds the new directory atomically and retains its ancestors
without write/delete sharing; a conflicting handle or reparse point fails closed.
Zerus never removes a replaced or partially populated directory.

On Unix, a separate native worker owns Git's deadline and watches a wrapper-owned
pipe. Ending the GUI/SSH wrapper closes that pipe and cancels the worker's own Git
process group, including its descendants. The original receipt remains
unconfirmed after abrupt wrapper termination; status and duplicate requests never
restart the clone. Windows retains the existing kill-on-close job containment.
The complete encoded receipt is limited to 16 KiB; large Git errors are truncated
within that envelope, and status and duplicate requests return the same saved
result. Local paths, including Windows drive, UNC and device paths, are refused.
A single-letter SSH alias must use an explicit address such as `ssh://c/repo`.

### Worktrees and ordinary folders

New session keeps **Auto** available when the selected computer confirms the
working checkout, even if the repository has more checkouts than its bounded
catalog can list. Missing, bare, stale or unconfirmed folders remain ineligible.
Opening the form or choosing a folder creates no worktree; creation starts only
after **Start session**.

Session cards show a checkout line of the form `folder / branch`, such as
`zerus / work-1a2b`, preceded by a small Git branch icon; **detached HEAD**
replaces a missing branch. A linked worktree names its main checkout folder
rather than its own generated folder. Ordinary folders without Git metadata
show only their name, preceded by a folder icon. The opened session's header and
**Details** show the same line, marked `· worktree` for a linked worktree; the
full folder path stays in the header tooltip and **Details**.

A running Codex session with pending optional questions, ones the agent keeps
working without, shows an amber question-mark chip with their count (`99+`
above 99) right after its status badge, or after a separate new-reply badge, on
its Sessions and Overview cards; a collapsed strip tile shows a small amber `?`
mark. Its tooltip and the card's accessible description say how many
are waiting. The chip shares the amber of attention but never changes the
session's status, the
Needs attention filter, counters or badges, and it is hidden for paused,
stopped, archived or offline sessions and while the count is unknown. Open the
session to answer or skip the question in Activity.

Existing Git worktrees appear in Projects and the session inspector. Clicking a
checkout filters Sessions while preserving the open conversation and draft.
In **New session**, **Auto** creates a generated `work-…` branch from `HEAD` and a
sibling worktree when **Start session** is pressed, then starts there. Its path is
shown before launch. Opening the form and checking folders never create anything.
**None** starts directly in the selected working folder; ordinary non-Git folders
use this mode and never initialize Git. The adjacent options menu offers
**Choose existing…** and **Create manually…**. Choosing an existing checkout keeps
its path, machine, project and account. Explicit worktree launch actions also use
that checkout directly. Bare or unavailable copies cannot launch; ordinary Browse
remains available even without Git or after metadata errors. The project gear
opens folder configuration, and each agent choice includes its provider icon.
The folder status area stays empty unless it has a warning or an error.

**Create manually…** in this form, or **New worktree…** in Projects, lets you
choose a new branch, base ref
(default `HEAD`) and unused destination under an existing parent directory. The
CLI is `hgs [@host] worktrees create --path /repo --branch feature/task
--destination /repo-task --base HEAD --json`. Zerus also verifies `--common-dir`
and a request ID. Creation uses normal `git worktree add -b`, including configured
checkout hooks, with no force, fetch or deletion. The ref is resolved before
creation. When the base is a local branch with a remote upstream, such as `HEAD`
on `main` tracking `origin/main`, the new branch inherits that upstream so its
session card can verify publication; Git's default `simple` push mode still
refuses a bare `git push` to the differently named branch. Commits, tags,
detached `HEAD`, local upstreams and `push.default=upstream` or `tracking` leave
the new branch without an upstream. The five-minute operation can leave partial results after failure;
inspect the destination before retrying. Cancelling the session form preserves
the created folder and branch. Closing or archiving a session never removes them.
Automatic creation requires a fresh verified repository on the selected computer.
The form stays locked during creation, starts only after its matching receipt,
and never repeats a failed or unconfirmed request. Its destination remains
available for inspection or an explicit direct-folder launch.

Catalog reads are on demand, with a 30-second repository cache; `--refresh`
bypasses it. Identity is machine plus canonical Git common directory, not equal
paths or repository names. Limits are three seconds, 1 MiB of Git output and
512 worktrees. Errors distinguish `not_repo`, `git_unavailable`, `metadata_error`,
`timeout`, `too_large` and `folder_unavailable`; previous data is explicitly stale.
The UI caches at most 64 snapshots and 8 MiB. Cleanup is described in
[the cleanup design](superpowers/specs/2026-10-10-worktree-cleanup-design.md).

### Cleaning up worktrees

**Worktrees…** in the Projects heading opens a cleanup workspace across visible
projects and computers. Repositories linked from several projects or subfolders
appear once per computer; filters preserve exact project and machine identities.
Four groups separate **Cleanup candidates**, **Review work**, **In use** and
**Not checked**. Main checkouts and ordinary folders are hidden by default.
Search, sort and refresh share one compact toolbar. **Filters** opens project,
computer and state controls and the main-checkout toggle; its count shows active
filters without taking height from the workspace. Folder names are emphasized,
while a separate muted row with a Git branch icon identifies the branch.
For a single-computer scope, the coverage line names that computer and the
redundant computer column is hidden.
Search matches folder, branch, project and related session names; sorting offers
largest folders, oldest Git activity or folder names. Sizes are apparent file
sizes, not a promise of physical disk space reclaimed; incomplete measurements
are marked as lower bounds.

Catalog reads run two at a time, and repository reviews run sequentially in the
background. Coverage reports checked/known worktrees and incomplete catalogs.
After the first bounded repository review, unchecked known rows are checked
individually, including rows beyond the native 64-row review limit. Catalogs
remain bounded at 512 entries; this is not a scan of every folder on a computer.
Offline snapshots remain readable and selectable for review; removal stays
unavailable until the owning computer supplies a fresh check.
Browsing data is retained for one hour, across closing and reopening the manager.
The coverage line shows when catalogs were updated; **Refresh** rereads them and
clears the selected cleanup candidates. The open manager refreshes hourly, and
worktree creation/removal invalidates the affected computer/repository cache.
External Git changes can be picked up with **Refresh**. **Select candidates** uses
checks less than five minutes old. Selecting rows is independent of deletion
eligibility: Ctrl/Cmd-click, Shift-click and checkboxes select multiple folders in
any group. **Review cleanup** always checks the selected targets afresh; native
removal repeats those checks under the repository and session locks. A cached
browsing verdict never authorizes deletion.

Selecting a folder loads its files and commits after a short navigation debounce.
The inspector distinguishes loading, an empty list and a failed read with retry.
Its refresh button rereads the selected folder, including cached file previews.
Changed/untracked file names, tracked diffs and up to
30 original commits absent from the base history are available. Inspection and
file previews are cached in bounded application memory for an hour, never written
to the settings file. The base comparison is a
two-way diff and can include later changes on the base. **Local files** previews
ignored entries and their contents; **Sessions** opens the exact related
conversation on its owning computer. Reads are bounded (200 changed files or
directory entries per page and 128 KiB per text preview), with truncation shown. **Browse all files** and **Next** continue beyond
the largest ignored entries or the first directory page. Binary
files are identified, and symlinks are not followed. Git refs are local snapshots;
inspection never fetches. No-base and failed checks are reported explicitly.

The inspector keeps its longer metadata under **Details**. Local file names use
the available column width; browsing and pagination share one small navigation
row. The empty text preview stays hidden until a file is opened, and both the
file list and the opened preview keep usable, resizable space. Related sessions
have session icons and retain their exact conversation targets.

**Select candidates** selects only currently visible, verified cleanup candidates.
Folders with local files outside Git require individual selection, even when
Git reports them clean and merged. **Review cleanup** shows the exact selected
folders across repositories and computers, branch handling and why any folder
must stay. Folders outside the candidates group can be selected and removed too.
Local data needs a separate explicit checkbox; deletion checks its reviewed
metadata fingerprint again. A missing folder only loses its Git registration.
After complete success the dialog closes and the overview shows the result.
Partial results and errors remain open; uncertain replies are not replayed. **Stop after this folder**
lets the current operation finish and leaves the remaining folders untouched.

Integration evidence distinguishes ancestor commits, identical file trees and
cherry-pick-equivalent patches. Equivalent content is useful when reviewing
squash/cherry-pick workflows, but only Git ancestry permits optional branch
removal. Original branches otherwise remain available.

Manual removal is explicit and is reviewed on the computer that owns the
checkout. **Clean up…** in Projects or the session inspector lists the linked
worktrees of the repository as **Ready** (preselected), **Review** (removable but
with notes such as local ignored files, unmerged commits, archived sessions or activity in the last
24 hours), **Missing** (the folder is gone; only Git's record is forgotten) or
**Blocked**. The button shows the number of ready and missing worktrees; that
review runs at most every five minutes while the panel is visible and never
removes anything. A worktree row's menu offers **Remove worktree…** or
**Forget missing worktree…** for that checkout alone.

A worktree is protected when it is the main checkout or bare, is locked,
contains another worktree or Git repository, has initialized
submodules, has a detached commit that no branch, tag or remote branch contains,
or is used by a process of this user or by a running, paused or saved Zerus
session (archive the sessions first). Git's own removal would silently delete a
nested worktree inside an ignored folder; Zerus refuses it. An incomplete local
file inventory, unreadable session bindings or special files also block removal.
Uncommitted and untracked files require a separate **Permanently discard**
confirmation in the manager. That consent is bound to the exact file contents
and staged Git blobs (bounded to 64 MiB and two seconds), and does not override
any other protection. A changed or incomplete proof requires a new review.

Confirmation lists the folders, the ignored data deleted with them (such as build
output) and any branches that will be deleted. Each removal then repeats the
review under the repository lock used by creation, compares the reviewed
fingerprint and runs `git worktree remove`, one worktree at a time. It uses
`--force` only for that explicit content-bound discard, never automatically.
**Also delete merged branches** (off by default, remembered locally) uses
`git branch -d` only for branches merged into the main checkout's branch or
`origin/HEAD`; other branches are kept. A failure is reported for that worktree
and never retried automatically.

After **Terminate**, explicit **Move to archive**, or a confirmed clean native
exit, the owning CLI automatically removes only verified Zerus-created
checkouts that are clean, have no local ignored data, have no remaining session
or process, and are proven integrated by ancestry, identical tree or equivalent
patches. It never fetches or deletes the branch. Ownership requires both a
private creation receipt and the matching nonce in Git's per-worktree metadata;
older or externally created worktrees remain manual. Folder/branch naming is
not ownership evidence. Automatic cleanup has bounded checks and never changes
a successful archive into a failed termination. A lost removal acknowledgement
is reported as unconfirmed, with no automatic replay.

The desktop reports successful automatic cleanup, or keeps **Folder kept:
<folder>** after Terminate/Archive (**Cleanup unconfirmed:** when the outcome is
unconfirmed). This global reminder names the retained checkout; its tooltip and
accessible description identify the source session, computer, full folder and
reason. It can remain while another session is selected and does not expire
after a minute. The action is hidden while another known session or a new run
uses the folder. Review still checks the current files and requires explicit
confirmation before removal. Archives keep the exact branch/head needed to
recreate an automatically removed checkout on explicit **Restore**; a changed
branch or occupied destination is refused. Browsing and dry runs never recreate
a folder. Plain Pause never cleans up.

The cleanup review always shows integration evidence, even when local files
remain. Git integration uses local refs, not a fresh remote check. The selected
folder explains unsaved changes, local data loss and why automatic cleanup kept
it. **Inspect files and commits…** opens the bounded file browser, including
ignored files; a cache-like filename alone is not proof that data is disposable.
**Keep folders** makes no removal. Local-data and unsaved-work deletion require
separate consent. Manual removal keeps conversations and keeps branches by
default; restoring an archive may require recreating its working folder.

The CLI is `hgs [@host] worktrees review --path PATH
[--worktree WORKTREE] --json` and `hgs [@host] worktrees remove --path WORKTREE
--common-dir DIR --fingerprint HASH [--ignored-fingerprint HASH]
[--discard-fingerprint HASH] [--delete-branch]
--json`. Explicit inspection is `hgs [@host] worktrees inspect --path WORKTREE
--common-dir DIR [--file RELATIVE_PATH [--offset N]] --json`. Machines with an
older `hgs` must be updated before cleanup is available there.

## Machines and accounts

**Machines** edits local SSH aliases and overrides in
`~/.config/hgs/machines.local.json`, leaving the shared `config` intact. Blank
fields inherit OpenSSH configuration. Effective values come from `ssh -G`
without connecting; `hgs machine resolve ALIAS` exposes the same result.
**Test connection** checks SSH and hgs/tmux versions. **Open SSH terminal** supports
interactive host-key verification. **Install hgs** builds the CLI remotely from
a selected checkout while preserving existing state. Unsaved machine drafts do
not change configuration; **Discard draft** removes the draft explicitly.
Zerus lists a machine's sessions and connection state under its alias, even when
that machine calls itself something else (its `HGS_SELF` or host name).

**Phones**, below the machines and in Settings → Mobile connection, lists the
phones paired with every workspace this computer reaches: through its own mobile
connection, or through one enabled peer that has one (one hop, like phones reach
peers). Each phone shows its platform, when it was paired, its last activity
(recorded at most every five minutes) and the computer whose invitation it used;
older pairings show "—". **Pair a phone…** shows a single-use QR code with the
server and code for manual entry; it expires after ten minutes and closing the
dialog discards it. Scan it in Zerus on the phone through Machines → Pair another
workspace. **Revoke…** asks for confirmation, then removes that phone's
credential, queued requests and push registration; the phone loses every computer
of the workspace at once. A computer whose hgs predates phone management is named
and never asked. The same operations are available as
`hgs [@host] mobile phones|invite|revoke-phone <id> --json`; a computer without
a mobile connection answers `{"configured": false}`. Requests go to the relay
through the system `curl` with the computer credential on its standard input;
curl ignores the user's `.curlrc` and environment proxies and never follows
redirects. The desktop passes `--workspace ID --server URL` from the list it
shows, so a gateway whose mobile connection changed meanwhile refuses to invite
or revoke for another relay or workspace, and a code for another workspace is
never shown. A revocation that fails or cannot be confirmed stays reported until
**Refresh**; it is never retried automatically.

On Windows, these phone-management commands pass a nonempty `CURL_CA_BUNDLE`
path explicitly to curl's `--cacert`, so Schannel verifies the relay using that
PEM CA bundle. If the variable is absent or empty, curl keeps its default
trust behavior. An invalid bundle or failed certificate verification refuses
the request; there is no fallback to an unverified connection.

Multiple machine filters narrow both sessions and content search. Label color,
Bright/Soft mode and appearance previews are local. Color pickers cancel on an
outside click or Escape; only explicit confirmation saves a color. Project
color changes remain staged until **Apply**. **All machines** clears the
restriction. A temporary connectivity error retains the last snapshot and makes
unavailable controls explicit.

The machine chooser shows each computer's session count, including zero, and an
All machines total. Counts follow the current session state, project and folder
scope before machine selection or text search; child rows and unconfirmed launch
placeholders do not add sessions. An open chooser updates counts without replacing
its checkboxes or changing the selection. Offline computers retain their last
known session inventory; live-work filters still require current work evidence.

<a id="accounts"></a>

### Accounts

Accounts collects provider profiles from connected machines. Verified provider
account ID, or email with organization, groups the same account across hosts;
matching display names alone do not. Providers, organizations and conflicting
IDs remain separate. Local results appear first; remote profiles arrive
incrementally. Known rows and selection survive refresh, failure and offline
hosts. Limits use one current account snapshot, never the sum of machine usage.

On Android, the refresh status below the toolbar remains readable after a quick
sync. **Refresh details** lists each workspace's current read/apply stage, last
received report and failures; retained sessions and counters stay visible when
a read fails. Phone sync reads machine reports, while providers refresh their
usage independently. Account cards show the machine's provider refresh state,
saved/current report and provider update time, including after phone sync ends.
An unavailable quota report or an expired short-lived Claude usage token does
not mean the native account is signed out. Explicit native sign-in status takes
precedence; older authentication problems are labelled **Last reported** when
their report is stale or offline. Viewing accounts never refreshes Claude's
credentials on Zerus's behalf.

**Add account** creates a separate profile on the selected host and starts native
sign-in. Default/native profiles retain existing authorization. Its agents use New
session's rows: a large colored provider logo, the agent and what adding does.
A removed profile can be added again with its saved sign-in. Choosing another agent
never resizes the dialog or moves its fields: **Account** and **Account name** stay
in place and are disabled when they do not apply, and the hint keeps room for its
longest text. OpenCode has one native configuration per machine: **Rename…**
changes only its name in Zerus, **Remove from…** takes it out of Accounts and New
session there, keeping its configuration, credentials and sessions, and Add
account brings it back. Its row is unavailable while it is listed or not
installed. **New session**
shows one list of **Account and agent** pairs for the chosen computer on desktop
(including macOS) and Android. A large colored provider logo spans two text lines: the
account email and compact cached limits, or **limits n/a**. Desktop reuses the
status footer's compact meter widget, including the selected usage indicator;
Android shows the same usage/elapsed rails and pace colors. Default labels, profile
IDs and quota error details are omitted from these rows. The account list uses
the same provider identity and organization grouping as **Agent accounts**, with
bindings only from the selected computer. Aliases of one account share a row;
conflicting account IDs, organizations and unidentified profiles stay separate.
The selected, remembered or default local profile remains the exact launch binding.
Rows show cached limits at once. On desktop, opening the chooser also asks for
current limits of the chosen computer's accounts, except those checked within
the last five minutes, and **Refresh** asks for all of them again. Replies update
rows in place: rows group once per loaded catalog, and the list keeps its rows,
selection and scroll position. Changing a selection never probes providers.
A profile's credential fingerprint follows its signed-in account, so token
refreshes (Kimi's every 15 minutes) and Claude Code's settings writes keep cached
status and limits; sign-out or another account discards them. **Run shell** is a single desktop
separate choice. The last successfully launched agent and account per computer
remain remembered; without that agent on the computer, the first agent is chosen.
Focusing the list while accounts load never chooses **Run shell**. **Make default on…** sets
the default for new sessions; explicit `--account` wins, and attach, resume and fork
retain the original profile. Ordinary agent commands outside `hgs` keep their
native sign-in. Use `hgs [@host] account default native-claude` to restore the native
Claude default; removing a profile also removes its default assignment.

```sh
hgs account ls
hgs @mac account add work --provider codex --label Work
hgs @mac account login work
hgs @mac codex /path/to/project --new --account work
hgs account copy native-codex --from @local --to mac --as codex-work --label Work
```

`accounts.json` stores metadata and selected permission modes; authorization stays
in the profile's own directory. Managed profiles clear inherited API tokens;
native defaults keep the user's normal environment. Tokens are not displayed in
UI, and the mere presence of a file does not prove valid sign-in. Claude managed
profiles support claude.ai login; shared Claude Console authorization is not
isolated by this mechanism. Completed native sign-in finalizes profile setup
without altering permissions or workspace trust.

Managed Claude profiles inherit behavioral settings from `~/.claude/settings.json`,
including permission default, model and effort, at launch/resume. Profile overrides
remain. Auth, API keys, provider endpoints, directory overrides and other hooks are
not copied. OAuth, token renewal, limits and connectors stay native.

Managed Codex, Claude and Kimi profiles also use the user's MCP servers. Each
launch/resume merges `mcp_servers` from `~/.codex/config.toml`, `mcpServers` from
`~/.claude.json` or `~/.kimi-code/mcp.json` into the profile, including their
headers. A server that the profile added, changed or removed keeps its own state;
the profile's `.hgs-inherited-mcp.json` records only digests of what was inherited.
Provider sign-in, hooks and other settings are not shared this way.

**Permissions → Change…** sets provider defaults or bypass for future launches
and resume on that host. Explicit launch flags take precedence. Current agents
continue unchanged; account labels describe launch settings, not every running
session's effective permissions. Bypass maps to Codex full access without sandbox,
Claude `bypassPermissions`, Kimi Never Ask/`--auto` and DeepSeek's native
`danger-full-access`. The CLI is `hgs [@host] account permissions ID --mode
provider|bypass`. Permissions do not accompany copied sign-in credentials.
Resident legacy DeepSeek bridges also support this mode through the official
authenticated web API, without restarting the host or changing other sessions.
Capability checks precede new binding creation. If permission setup is not
confirmed, sending stays disabled; explicitly resume the same session to verify
and finish setup. A confirmed native preset is not applied again after an
uncertain acknowledgement. Already-running sessions keep their existing mode.

**Copy existing sign-in over SSH** copies Codex/Kimi credentials and necessary
provider/model settings into a new profile, without replacing another one or
copying history, tools or hooks. Claude's system Keychain cannot be transferred:
sign in on the destination. Providers can require new sign-in even after copying.
**Remove from…** requires confirmation naming the account and machine. Native
history and active session ownership remain separate from catalog removal.

On macOS, HGS does not unlock Keychain. The per-user
`com.hgdev.hgs.user-session` service allows a same-UID HGS process started through
SSH/tmux to join the existing graphical security session before Claude login,
inspection or launch. It passes system session authority, not passwords or tokens,
starts on demand and exits after a minute idle. A logged-in desktop session is
required; screen lock differs from logout. If Keychain is actually locked in that
session, unlock it there. Authentication diagnostics distinguish these cases.

<a id="local-models"></a>

## Local models and OpenCode

Ollama runs model inference on the chosen agent computer. The native harness
owns tools, permissions and conversation state. Codex can use a local Ollama
model with its existing adapter; see the [qualified local-model example](local-models.md).

Choose **OpenCode** in New session to use OpenCode 2 through its shared
background service. Select an offered `provider/model` (keyless free models are
listed when the service offers them) and, where the model has variants, an
effort, or retain the default model. Supported: text and voice conversations,
queued messages, Stop, native questions and permissions, status and history,
model and effort change, rename and forgetting the Zerus binding. The desktop
Terminal tab opens OpenCode's native TUI on the existing service and conversation,
also available as `hgs a NAME --existing --run-id RUN`. Disconnecting closes that
client; the service and conversation keep running. Opening Terminal never starts
or replaces a service and disables the attach client's automatic updates.
This interactive capability is separate from pane snapshots: mobile Terminal
snapshots remain unavailable. Pause/resume, attachments, compaction, fork and
child sessions are unavailable.
See [OpenCode versions, service and controls](opencode.md).

From the CLI, `hgs [@host] opencode DIRECTORY --new -n TAG --model PROVIDER/MODEL`
starts a session. `--effort VARIANT` needs `--model` and must be one of that
model's `effort_options` (listed by `hgs account ls`); when omitted, the
model's default is used. A launch starts the shared service when none is
running, and `hgs [@host] account start native-opencode` starts it explicitly
(**Start OpenCode**). Inspection, listings and sending never start it. OpenCode's
official user install, `~/.opencode/bin`, is discoverable from GUI, service and
SSH launches; an `opencode` found earlier on PATH keeps precedence.

`hgs [@host] account favorite native-opencode --model PROVIDER/MODEL --favorite on|off --request-id UUID` stars a model on that computer (at most 64; stored in `opencode-favorites.json` beside the account catalog). A retried request identity returns its first change, with today's favorites list.

`hgs [@host] account models native-opencode [--location DIR] --json` lists every available model of that computer and folder (connected providers only, at most 10,000 models and 4 MiB; `models_truncated` reports a cut). It never starts OpenCode; with the service stopped it reports `not_running` and, for the home folder, the last known short list.

<a id="deepseek"></a>

## Official DeepSeek Harness

The native adapter is pinned to `@deepseek-ai/dsh@0.2.1-rc.2`. Install it on the
agent host, sign in and choose **DeepSeek Harness** in New session:

```sh
npm install -g @deepseek-ai/dsh@0.2.1-rc.2
hgs account login native-dsh
hgs dsh /path/to/project --new -n work
```

Zerus launches the official native engine and its tools, access rules and history.
A versioned adapter connects its Session Controller through a private Unix socket.
The official web host uses a separate `hgs` profile through `--patch`; sign-in and
secrets stay in `DSH_HOME`. The web endpoint is loopback-only; remote access uses
SSH. `~/.npm-global/bin` is discoverable from service and SSH launches.

A new session waits for its first message. API-key `deepseek-official` connections
work independently of account sign-in. If `deepseek-account` needs login, Activity
shows **Sign in** and keeps the draft until native authorization finishes.

**Native UI** opens the official web interface and the host workspace's native
session list, including sessions created through Zerus. Native cookie exchange
supplies authentication, and ports/tunnels are discovered automatically. The
header always opens it in your browser. On Linux, the optional embedded view
shows it as a tab beside Activity: navigation preserves its screen and draft,
embedded cookies and cache are memory-only, and questions and approvals for
Zerus-managed sessions remain in Activity (**Respond in Activity** points there
when needed). The embedded view is the plugin
`<prefix>/lib/hgs-tray/plugins/libhgs-tray-native-ui.so` (`/usr/lib/...` from
Arch packages, `~/.local/lib/...` from `tray/install.sh`), which alone needs Qt
WebEngine (`qt6-webengine` is an optional dependency on Arch). The GUI loads it
when a DeepSeek session is first shown. Without it, or on macOS, there is no
Native UI tab, and Sign in, Open terminal and the other Native UI actions open
the browser instead; Zerus on Windows does not offer the native UI
([Windows](installation.md#windows)). Builds choose with the CMake option
`HGS_NATIVE_UI_PLUGIN` (`AUTO` by default: built when Qt WebEngine is found).
`hgs native-ui SESSION --json` returns a private connection URL containing a token;
keep that output private.

Activity supports creation, messages/images, busy-session submission, questions,
approvals, per-session model/effort, pause/resume, rename, goals, todos and child
views. Children are continuable and run independently of the turn that created
them: the engine records a child's native identity when it creates the child,
and stopping the parent ends the parent's turn while the child keeps working. A
new child view stays empty until the child's first step. A one-shot child stored
by 0.2.0-rc.2 and cancelled before its first step stays listed with an empty
view. A child without a valid identity reports native corruption. A
child view shows the child's own model, which reading never changes. Request
receipts prevent duplicate submission. Uncertain delivery requires inspection.
Model changes do not replace profile defaults. A child keeps the model it
inherits and takes messages only through its parent; the adapter refuses model
changes, messages, compaction and interrupts addressed to the child itself.
Stopping one session does not terminate neighboring agents or the shared host.
Closing Zerus leaves the host running; Forget retains native history. Linux user
systemd runs the host in an independent scope so GUI service restart cannot kill
the engine.

**Fork session…** uses the official Session Controller fork. It copies the
conversation through its latest completed turn into a new native session of the
same profile and folder (including its permission preset),
named after the fork, and binds the exact session ID the engine returns; the
source keeps its identity and history. The engine, not Zerus, holds the new live
agent until its host next starts: Zerus sends its messages, while its questions,
approvals and Pause stay in Native UI until then. A fork needs a completed reply.
If the reply is lost, forking again under the same name adopts the result from the
same host; after a host change Zerus refuses to repeat it. A native failure after
creation can leave an unbound session in Native UI.

Content search covers bound DeepSeek sessions through the native store's
literal, case-insensitive event scan, which works without the optional full-text
index. Results contain the person's and the agent's text only; injected context
and reasoning are excluded. Searching never starts, resumes or reloads a host.
Hosts are asked at once, each scanning for at most 5 seconds; a stopped host, an
older adapter or a host that does not answer within 7 seconds reports that
history as unavailable.

The official Schedule plugin (**Automation tasks** in Native UI Plugins) is
listed and managed per exact session. The web bundle of 0.2.1-rc.2 includes it,
and the profile Zerus creates layers that bundle, so it is on unless the
profile's own patch disables it:

```sh
hgs schedules dsh/project/work            # tasks of this session as JSON
echo '{"request_id":"UUID","expected_run_id":"RUN","expected_conversation_id":"session-ID",
  "expected_generation":"GENERATION","action":"update","id":"schedule-ID",
  "expected":{...record from the list...},"title":"New name"}' | hgs schedules dsh/project/work --json
```

`action` is `delete`, `update` (the native compare-and-set; `title`, `prompt`
and the native `change` timing object) or `history` (`limit` 1-100, optional
`before`). Results keep native codes such as `schedule_conflict` and
`schedule_not_found`. None of these activate the session. Without the plugin the
command refuses and listings omit `schedules`.

Account transfer stays in Native UI. Native context reset awaits a supported
Session Controller operation. Resident adapters advertise each operation; one
started by an older Zerus keeps fork and schedules in Native UI and its history
unavailable to search until its next ordinary start. Other harness versions need
contract verification; updates never restart live hosts or agents automatically.
`python3 tests/test_dsh_native.py -v` exercises the official engine with a local
keyless model, covering history, lifecycle, questions, settings, fork, search,
schedules and browser login.

## Automatic recovery

**Settings → Automatic recovery** shares rules for temporary overload, rate limits,
session usage limits and network failures. Automation is off by default. Each class has its own delays:
`15, 30, 60, 300, -1` stops after four attempts; `15, 30, 60, 0` repeats the previous
60-second delay indefinitely after the first two attempts; `-1` alone does nothing.
Small jitter is applied. Class counters persist within an episode when errors change.
Temporary overload covers HTTP 500, 502–504 and 529, model capacity errors and the
520–524 gateway failures that Cloudflare reports for an unreachable or timed-out
provider, often without a response body.
Kimi's explicit HTTP 429 message about its rolling 5-hour window uses the
**Rate limit** schedule, including when the message calls it a quota and offers
an upgrade. It provides no reset timestamp, so recovery does not invent one.
Monthly quota, billing and sign-in failures still require manual action.

**Session usage limit** is separate from a temporary rate limit. Its default mode,
**Wait for reset**, schedules a continuation at the provider's reset time, using
the failure's date and reported timezone. The toolbar shows that reset time.
If the deadline is unknown, ambiguous or was already reached when the turn failed, recovery needs attention
instead of starting short retries. **Retry on a schedule** uses the same delay
syntax as other classes: `300, 0` retries about every five minutes until stopped,
even before the reported reset. Explicit native Retry-After deadlines still apply.
Existing shared policies keep their settings and gain the reset mode by default.

Native agent retries finish first. Claude, Codex and Kimi receive an explicitly
marked `[HGS automatic recovery]` continuation in the same conversation; HGS does
not replay the original request or tool command. The newer DeepSeek adapter retries
the failed model request; an older host uses continuation messages. Sign-in,
balance, other quota and context exhaustion require manual action. Questions, new input,
Stop, session termination and identity changes cancel waiting; a native draft
blocks delivery. Unknown providers do not receive automatic input.

A continuation goes only to an agent that the failure stopped. When the agent
keeps working after a provider error that followed an accepted continuation, the
episode returns to **Recovery in progress** with the same attempt count: nothing
further is scheduled or delivered while it works, and its answer completes the
episode. An account has one recovery request in flight at a time, until the agent
accepts the continuation and makes progress, so a long working turn does not hold
the account's other sessions at **Waiting to retry**. From that progress the job
records `resumed_at`, and desktop and mobile show the turn as ordinary work rather
than **Recovery in progress**. The episode stays open until the turn ends, so a
further provider failure in that turn reschedules on the same attempt budget.

Provider failures replace Working or Compacting with a specific state such as
**Usage limit reached**, **Model at capacity** or **Sign-in failed** and appear
under **Needs attention**. Activity explains the stopped turn and offers
**Open Terminal** and, for exhausted quota, **Refresh usage**. Restore account
access or wait for the reset, then explicitly send a message to continue. Drafts
are kept; refreshing usage never retries a turn. Account percentages alone do
not change a running agent's state. Failed refreshes and offline snapshots are
labeled as last reported usage.

Claude's **What do you want to do?** usage-limit menu appears in Activity even
when the failed turn or a background shell leaves hook state at Error or Working.
Choose the native stop, wait-and-continue, or usage-credit option explicitly;
there is no preselected answer or automatic approval. Zerus verifies the exact
process, conversation and complete current panel before each navigation key and
one confirmation. Usage-credit setup continues in Terminal for any further
billing consent; unfamiliar account choices are shown read-only there.

After an early reset, fresh complete account usage below the limits shows
**Usage available** beside the composer and explains how to continue. An open
native limit menu still needs an answer, and a stopped turn still needs a new
message. Stale, offline or failed usage reads cannot produce this notice, clear
the native failure or trigger a retry. Claude's own wait continues under its
native rules; Zerus does not change the shared recovery policy or enable credits.

Codex failures come from the exact conversation's native error journal and
structured failed turn completions, including compaction failures. Tool output
and quoted error examples cannot change the session state. New native progress
clears the failure; changing the account counter alone does not.
Matching Codex log and completion records retain their exact failure references,
so delayed duplicate evidence keeps the same recovery episode, deadline and
attempt budget. Separate native turns remain separate even for identical errors.
A previous cancellation caused by switching those sources can resume only with
confirmed matching native evidence and zero delivery attempts; user cancellation,
new input, interruption, changed identity and uncertain delivery still stop recovery.

A chip in the toolbar above the message field shows the countdown; its popover
shows the attempt number, **Attempts**, **Retry now** and **Cancel retry**. Retry now respects a known Retry-After and the reset deadline in reset mode. Policy edits apply to new
episodes; already scheduled episodes retain their delays and session-limit mode. Disabling recovery
cancels automatic continuation. Uncertain delivery stops the episode rather than
sending Enter again.

An independent `hgs-recovery.service` / `com.hgdev.hgs-recovery` worker runs without
GUI. Custom installs can run `hgs recovery worker`. Episodes and receipts live on
the agent host under `sessions/recovery/`, guarded by policy revision and a single
executor lock. CLI operations are `hgs [@host] recovery get|set|sync|action`, with
JSON writes on stdin. Adapters register explicitly in `src/state/recovery.rs`.
On Linux, client policy refreshes start the configured user service without
restarting an active executor. A detached worker remains the fallback when no
installed service or usable user manager is available; refreshes start none
while a worker runs. The service's own worker (the job's process as launchd
reports it, or the unit's main process as systemd reports it, not another
process in the unit's control group; only when systemd cannot answer does the
control group decide) never exits because another worker holds
the executor lock, which would make the service manager start it again every 10
seconds: it waits for the lock, and a detached worker hands over to it between
rounds.

Open Zerus clients synchronize policy versions over HGS/SSH every 15 seconds;
offline machines catch up later and executors keep their saved local policy.
Revision and writer identity establish deterministic order. Saving a stale form
requires Reload. No external cloud service is involved.

<a id="processes"></a>

## Processes launched by agents

**Processes** is off by default; enable it in **Settings → Processes**. It appears
beside Activity and Terminal, or Native UI for DeepSeek. When disabled, its list,
count and output are not polled; normal Activity, fleet and account updates continue.
The small **Background** summary is part of ordinary fleet polling even
when this tab is disabled. Full process history and output remain opt-in. A
background session's context menu offers **Show running commands** when Processes
is enabled.

**Live shells / processes** shows verified shells and independent command trees.
Recorded tool calls appear separately, with completed history collapsed. Equal
command text does not prove that a tool record and OS tree are the same task.
Rows expose owner, state, elapsed time and available folder/progress/output. OS
trees show PID and child count; their stdout is not independently captured.
The active count uses the maximum of confirmed tool records and OS trees, not
the sum of overlapping sources. Unconfirmed records do not count as active.

| Agent | Observation and output | Targeted control |
|---|---|---|
| Claude | Hooks, Bash/tool_result, backgroundTaskId and native output files | Verified live OS trees |
| Codex | Hooks and rollout CommandExecution, including code-mode and write_stdin | Verified live OS trees |
| Kimi | Hooks, main/child process-task registry and tasks/ID/output.log | Verified live OS trees |
| DeepSeek | tools/execute and ctx.jobs in the native bridge | Session-owned native jobs |

**Stop selected** verifies every selected target, run, conversation and ownership
before dispatch. Native jobs also require host generation and job identity. OS
trees require PID/start-time and ancestry checks, use TERM and escalate the selected
tree to KILL after two seconds if needed. Linux uses PID handles; macOS rechecks
start times. The agent and neighboring trees are excluded. Stale generations and
offline snapshots cannot stop a newly reused target. Requests are sequential,
with no automatic retry; identity changes cancel unsent ones. **Stopping** remains
until completion is confirmed. Whole-turn Stop in Activity is a separate action.

`hgs [@host] processes SESSION --json` reads records. `--output ID` and `--stop ID`
require `--run RUN --conversation ID`; native jobs additionally use
`--generation GENERATION`. Archives accept `--archive ID` for read-only history.
Older DeepSeek hosts retain history until their ordinary next start; live registry
and targeted stopping need the newer bridge. GUI updates do not restart hosts.

Default intervals are 2.5 seconds for open Processes, 30 seconds for background
counts, ten seconds for Unconfirmed output and 0.5 seconds before a loading
indicator. Detailed polling covers only the selected visible session. Output is
read only for the visible selected command, immediately on opening, and refreshed
on meaningful state/output changes. Disabling the feature uses
`hgs inspect --skip-processes`; explicit CLI reads remain available. Terminal/Native
UI inspection also follows the background interval. These reads use local state,
OS/native registry or SSH, never a model call.

Lost foreground hooks become Unconfirmed rather than staying Starting through
another command. Codex completion beyond the normal 8 MiB transcript window can
be read incrementally up to 16 MiB per poll and cached privately. Completion needs
the exact call ID in the verified conversation; absence of an event is not success.

## Terminal, SSH and clipboard

The built-in Terminal is a real PTY client attached to an existing tmux session,
locally or over SSH. It supports ANSI/VT, UTF-8, colors, fullscreen applications,
keys, selection, scrollback, paste and resize. Connect, Disconnect and Reconnect
change only that client. Closing the window detaches it and leaves the agent alive.
The client uses `hgs a SESSION --existing [--run-id ID]`; disappearance does not
restore a session automatically. Ordinary `hgs a SESSION` retains Saved restore
behavior. The vendored terminal engine is libvterm 0.3.3 under MIT; builds do not
download it. Rendering updates changed cells and the cursor rather than scanning
the full screen for every small change.

Dropping files into Terminal inserts paths without Enter, quoted for the session's
machine: in single quotes on Unix machines, and with backslashes for a session on
this Windows computer, in double quotes when a path has spaces or shell-special
characters, or in single quotes when it has `$` or a backtick, which PowerShell
would expand inside double quotes. Remote drops first copy files to the session host, limited to eight
files, 10 MiB each and 20 MiB total. The copy is bound to the session's run and
conversation. An untracked shell (`hgs sh -d`) has neither: its copy is bound to
the live session's creation time and refused when a tracked run owns the session.
Changing tab, session or connection during transfer cancels insertion.
**Open externally** and **Copy connect command** retain access from other clients.

If SSH drops, tmux on the remote host continues running. Reattach normally. If the
local tmux server dies, its sessions are gone; `hgs` restores the terminal mode and
reports this instead of leaving the alternate screen active:

```text
hgs: the tmux server died; terminal restored, sessions on this box are gone
```

A client failure while tmux remains alive does not trigger terminal reset sequences
that could corrupt the normal screen. Saved conversation bindings can still be
restored after server loss, but tmux is not a reboot-persistence mechanism.

On Linux, tmux built with systemd support puts each pane in a `tmux-spawn-*.scope`
that is `PartOf=` the unit holding the tmux server, so stopping that unit ends every
pane, whatever its `KillMode=`. When `hgs` runs in a user service manager unit (the
desktop GUI, the mobile connector, the recovery and swarm workers, a desktop
terminal) and no server answers on the selected socket, it runs the `new-session`
that starts the server through `systemd-run --user --scope`, in a transient
`hgs-tmux-<uuid>.scope` that holds only tmux and is collected when the server
exits. Stopping or restarting the unit that launched the first session then leaves
the agents running; stopping that scope itself ends the server and its agents. A
running server is never moved, restarted or replaced: one started earlier, by an
older `hgs` or by a plain `tmux` command, stays in its unit until it exits.
Outside a user unit (an SSH login, macOS), without a reachable user manager or
`systemd-run`, or when systemd cannot create the scope, `hgs` starts tmux as
before.

The optional `tmux.conf` binds Ctrl-V to `hgs paste` for the remote image clipboard
bridge. At attach, `HGS_CLIENT` records the viewing host; an SSH hop forwards it
through `--client`. The session host fetches PNG from the client's `hgs clip`, puts
it in its own native clipboard and forwards Ctrl-V so the agent reads its usual
clipboard. Local sessions need no SSH. Errors still forward the key, with a short
status and a best-effort line in `${XDG_STATE_HOME:-~/.local/state}/hgs/paste.log`.
The log is not rotated automatically. Clipboard synchronization is specific to
the image-paste bridge, not general desktop synchronization. A remote host needs
a working graphical clipboard environment; choose the viewing peer explicitly
when several clients attach to the same session.

New sessions created through a peer obtain the host's graphical environment rather
than inheriting an unrelated SSH display. Terminal client environment and macOS
graphical security context are handled separately from agent conversation state.

### External terminal modes

Linux defaults to Konsole with `auto`, `tab`, `window` or `clipboard`. Without
Konsole in `PATH`, it opens the desktop's default terminal through
`xdg-terminal-exec`, or `x-terminal-emulator` on Debian and Ubuntu; the
Konsole-specific `tab` and `window` modes do not apply there. macOS offers Terminal.app or clipboard. A custom
`--terminal` template substitutes `{cmd}`.
Konsole tab insertion needs its security-sensitive D-Bus API and an available
window; automatic mode may open a new window when a usable tab is unavailable.
Enabling that API grants other session-bus processes control of terminal tabs.
Terminal.app remains the native macOS backend; alternate terminal bundles need a
supported explicit launch template, not an executable path guessed from a bundle.

Windows offers `auto`, `tab`, `window`, `console` and `clipboard`. `auto` and `tab`
open a tab in the most recent Windows Terminal (`wt.exe`) window and `window` a new
Windows Terminal window; Windows Terminal starts `hgs.exe` directly. When `wt.exe`
is missing or would change an argument (it re-joins the command line and expands
`%NAME%`), the session opens in a console window instead; `tab` and `window` report
that, `auto` does not. `console` always uses a console window, hosted by
`hgs-tray.exe --console-run`: it starts `hgs.exe` with the exact arguments and no
shell, and stays open with the exit code until Enter when `hgs` fails. A
`--terminal` template (`terminal=` in `tray.conf`) takes the place of every mode
but `clipboard`: Zerus starts the template's own program with `{cmd}` replaced by
the PowerShell command, and a console program gets a console window of its own.
Name a shell that runs that command, for example
`powershell.exe -NoExit -Command {cmd}`. A Linux template from a shared
`tray.conf`, such as `foot -e bash -lc {cmd}`, does not work on Windows. A
`tray.conf` shared with Linux keeps its mode; a macOS mode such as `terminal`
falls back to `auto` with a warning.

Clipboard mode copies the shell-safe connect/restore command instead of launching
a terminal. Explicit **Open terminal** still opens one without changing that mode.
Linux uses its desktop clipboard; macOS and Windows use the native clipboard.
New-session **Open terminal window** remains an independent preference.

On Windows, copied and displayed commands are written for PowerShell: arguments
are single-quoted when needed, `'@host'` is always quoted, and a quoted `hgs.exe`
path gets the `&` call operator. When Windows PowerShell 5.1 would pass an argument
incorrectly (an empty one, a double quote, or a space before a final backslash),
the command is copied as `Start-Process -FilePath … -ArgumentList … -NoNewWindow -Wait`
instead. **Open shell in session folder** starts PowerShell (`pwsh.exe` when
installed, otherwise Windows PowerShell) in a folder of this computer: in a Windows
Terminal tab or window that starts there, or in a console window started there when
`wt.exe` is missing or would change the folder (it expands `%NAME%`). A
`--terminal` template receives `Set-Location -LiteralPath FOLDER` as `{cmd}`.

The tray config lives at `~/.config/hgs/tray.conf` unless overridden; on Windows
`~` is `HOME` when set, otherwise the account's profile folder (normally
`%USERPROFILE%`; unlike `hgs.exe`, the desktop app ignores a changed
`USERPROFILE`). CLI flags win at startup; a later tray-menu selection takes effect
immediately and writes the config for subsequent runs. A later launch with the
same explicit flag wins again.

<a id="installation"></a>

## Installation, configuration and diagnostics

See [installation.md](installation.md) for dependency and install commands.
`~/.local/bin/hgs` must exist on peers because SSH calls use that path.
The peer's login shell may be any POSIX shell or fish. Arguments are quoted so
both read them alike, and commands with shell logic, such as the capability
checks of `wait`, `hooks` and `guide`, `hgs machine check` and
`hgs machine ssh --directory`, run under `sh -c`.

`hgs status` only reads. For this computer and each peer it shows the hgs
version, live sessions by what they need (working, needing attention, ready),
stopped and archived sessions, and whether the desktop app, swarm worker,
recovery worker and mobile connector are running. This computer also shows the
running `hgs` executable. A peer whose hgs predates `status` still reports its
version and sessions; update it to see its services. `--local` limits the report
to this computer, `@host status` to one peer, and `--json` prints the same data
for scripts. `hgs version` and `hgs @host version` print `hgs X.Y.Z`, like
`--version`.
Configurations are data assignments, not executable shell scripts:

```sh
HGS_SELF="workstation"       # aliases that identify this host
HGS_PEERS="macbook build-01"  # SSH aliases to poll and target
HGS_TAB=1                    # enable terminal title updates
HGS_TAB_COLORS="macbook=#80ff80"
```

The CLI config is `~/.config/hgs/config`; `HGS_CONFIG_DIR` and `HGS_STATE_DIR` permit
isolated setups. `HGS_SSH` names the OpenSSH client for every peer connection
(dry runs still print `ssh`); without it `hgs` runs `ssh` from `PATH`, and on
Windows `%SystemRoot%\System32\OpenSSH\ssh.exe` first. Runtime storage, `HGS_*`
variables and application/service IDs stay compatible with existing installations. Source is [MIT](../LICENSE); see
[third-party notices](../THIRD_PARTY_NOTICES.md) for dependency licensing.

The GUI installer updates only Zerus and its desktop integration. It never
restarts native agents, tmux servers or DeepSeek hosts. A changed build replaces a
running Zerus through `tray/update-installed.py`, the only supported live update:
Zerus quits as its Quit action does, so drafts are saved, then starts again through
systemd or launchd. Do not stage GUI copies in `~/Applications` or `~/.local/bin` by
hand. The update keeps one rollback copy in `~/.local/share/hgs/desktop-previous`,
restores it when the new GUI does not start, and confirms the running image,
unchanged nonempty drafts and native agent and tmux processes. It records the
installed builds in `~/.local/share/hgs/build-provenance.json`, then removes hidden
`.zerus-*` staging folders left beside the GUI by earlier hand-made updates once
they are an hour old, hold only installer copies and no process uses them. A
failed check leaves Zerus running and prints the private draft snapshot it kept. Zerus quits
gracefully on SIGTERM, SIGINT and SIGHUP; an older running build that does not
is closed from its tray menu, or the installer asks you to quit it first. Use
`HGS_TRAY_RESTART=0 ./tray/install.sh` to stage a GUI update while keeping a running
workspace and its unsaved drafts. The new UI starts on the next launch.

Linux installs the binary under `~/.local/bin`, a systemd user service under
`~/.config/systemd/user` and a launcher under `~/.local/share/applications`.
The launcher is not a second autostart entry. The service belongs to
`graphical-session.target`, restarts after crashes and does not loop on exit code 4
(system tray unavailable). Compare `systemctl --user status hgs-tray.service` and
`journalctl --user -u hgs-tray.service` when startup fails.

On macOS the installer uses Qt/Homebrew, builds and ad-hoc-signs an app bundle,
and installs it under `~/Applications/hgs-tray.app` with
`~/Library/LaunchAgents/com.hgdev.hgs-tray.plist`. A graphical login is required
for the tray. Signing precedes bundle replacement; matching code hashes avoid
unnecessary replacement. Ad-hoc signing is not a Developer ID distribution
signature. LaunchAgent crash recovery is independent of native agent engines.
The LaunchAgent runs with `ProcessType` `Interactive` and the GUI thread requests
user-interactive QoS, as for an app started from Finder. Without that key launchd
starts a daemon and clamps all its threads below ordinary CPU load, niced load
included: a busy machine then starves the whole window until the main-thread
watchdog aborts it, and no QoS request from inside the process lifts the clamp.
`launchctl print "gui/$(id -u)/com.hgdev.hgs-tray"` shows `spawn type = interactive`.
At startup the GUI reads the highest priority its main thread may reach: a login item
(`XPC_SERVICE_NAME` `com.hgdev.hgs-tray`) limited below 31 offers **Fix login item**,
which runs the bundled `zerus-setup --autostart` only after confirmation.
Finder, the Dock and Spotlight start the bundle without the LaunchAgent's `--hgs`
and with PATH `/usr/bin:/bin:/usr/sbin:/sbin`; the desktop then finds the CLI in
the standard install directories listed under **Desktop option** below.

```sh
launchctl list | grep com.hgdev.hgs-tray
~/Applications/hgs-tray.app/Contents/MacOS/hgs-tray --selftest --hgs ~/.local/bin/hgs
```

A PID means the agent is running; `-` means it exited and the next field is its last
exit code. `mac-install.sh` requires an explicit target SSH alias, installs source
under `~/.local/src/hgs` and builds on that host. `--no-tray` updates only the CLI.
No reverse connection is created by default. Explicit peer setup uses
`HGS_MAC_PEER`, `HGS_MAC_PEER_HOST` and optional `HGS_MAC_PEER_USER`,
`HGS_MAC_PEER_KEY`, `HGS_MAC_PEER_COLOR` through local configuration.

| Desktop option | Purpose / default |
|---|---|
| `--hgs PATH` | CLI executable; otherwise `hgs=` in the tray config, PATH, then `~/.local/bin`, `/opt/homebrew/bin` and `/usr/local/bin`; on Windows `hgs.exe` next to `hgs-tray.exe`, then PATH, and the stable CLI once local sessions are enabled ([Windows](#windows)) |
| `--local-poll MS` | Local tray polling, 2000 ms |
| `--peer-poll MS` | Peer tray polling, 60000 ms |
| `--terminal TEMPLATE` | External terminal launcher with `{cmd}` |
| `--icon-color COLOR` | Override tray mark/counter foreground |
| `--open-mode MODE` | Linux auto/tab/window/clipboard; macOS terminal/clipboard; Windows auto/tab/window/console/clipboard |
| `--selftest` | Headless hgs connectivity check |
| `--sessions` | Open the workspace; on Windows also the default |
| `--background` | Start in the tray only; the default on Linux and macOS. Not together with `--sessions` |

A single-instance socket forwards repeated launches to the running GUI: one that
would open the workspace opens it there.
On Linux and macOS, concurrent launches take turns on it, and a launch such as
`hgs-tray --sessions` leaves it to the `hgs-tray` systemd or launchd service
while that service runs or restarts: it waits up to 30 seconds for the
service's GUI and opens the workspace there. It starts its own GUI only when
the service stops or does not answer in time. A GUI that a launch started while
the service was stopped keeps the socket, and a later service start then exits
without a GUI of its own.
The desktop version in the rail opens **About**; its tooltip shows the complete
installed version, including the Dev build number or local source identity.
**Settings → About** also
shows the configured `hgs` CLI version, read when About opens; an unavailable or
unrecognized response remains explicit instead of using a compiled-in value.
Closing a workspace never owns agent process lifetime. Local machine settings,
credentials, runtime journals and personal connection values belong outside the
repository.

<a id="windows"></a>

## Windows

On Windows, `hgs.exe` and `hgs-tray.exe` manage remote Linux and macOS machines as
a control console. Once local sessions are enabled on the computer, `hgs.exe` also
runs agent sessions there, in a session host of the user on ConPTY
([Windows Phase 2](superpowers/specs/2026-10-13-windows-native-host-design.md)).
See [installation and limitations](installation.md#windows) and
[testing the session host](windows-host-testing.md).

**Local sessions.** `hgs session-host install | status | stop | uninstall [--json]`
exists only on Windows and manages only the computer it runs on; for another
machine, `hgs @host session-host` exits with code 2. `install` enables local
sessions: it stages the stable CLI `%USERPROFILE%\.local\bin\hgs.exe` and a host
image below `%USERPROFILE%\.local\share\hgs\host`, then writes the `local-sessions`
marker in the configuration directory. `status` reports the marker, the staged
files and the running host without starting one; `stop` stops an idle host;
`uninstall` refuses while sessions run, then removes the marker and leaves the
staged files. The marker alone decides, so a stopped or crashed host leaves the
computer enabled with its saved sessions. The host starts with a launch, resume or
fork, never with a listing, and exits a few seconds after its last session. Every
session runs the stable CLI's `hgs.exe --run`, which starts the agent; tracked
Claude Code, Codex and Kimi sessions get hooks that name the stable CLI. `hgs a`
needs a console such as Windows Terminal, not the pipes of the mintty Git Bash
window, and detaches with `Ctrl+B d`.

**Commands.** Commands for another machine (`hgs @host …`) work as on Linux; the
remote command stays `~/.local/bin/hgs` with the same quoting as on Linux. Until local sessions
are enabled, `ls`, `machine`, `project`, `swarm`, `presence` and `session-host`
work locally. `account ls` and `account default` without an ID print an empty
catalog, and `account copy` works between two remote machines. Every other local
command, such as `a`, `send`, `inspect` of a local session or a launch without
`@host`, exits with code 2:

```text
hgs: Local sessions are not enabled on this computer; enable them on the Machines page or run hgs session-host install, or choose a remote machine.
```

With local sessions enabled, local session commands run this computer's sessions
in the session host. `paste`, `clip` and `notify` still exit with code 2
(`hgs: clip is not supported on Windows`), DeepSeek sessions and voice messages are
refused with their own messages, and OpenCode is not supported there. The DeepSeek
Native UI commands (`native-ui`, `account login native-dsh`, `a dsh/…`) and the
mobile-peer commands exit with code 2 in both cases, each with its own message.

**Listing.** Until local sessions are enabled, the local element of `hgs ls --json`
and `hgs ls --json --local` is a control-only box with `"local_sessions": false`,
`projects: {}` and `sessions: []`; plain `hgs ls` prints
`(control console: sessions run on remote machines)` for this computer, and
`--dry-run ls` prints nothing for it. With local sessions enabled, it lists this
computer's projects and sessions as on Linux. Either way it carries `local_host`,
the session host as found ([JSON](#json-and-external-clients)), and its metrics
include CPU, memory and uptime; load averages are null. The computer's name is
`HGS_SELF` when set, otherwise its lowercase `COMPUTERNAME`. Configuration and
state live under `HOME` when set, otherwise `%USERPROFILE%`, in the usual
`.config\hgs` and `.local\state\hgs` folders.

**OpenSSH.** Windows OpenSSH never exits after it has used a synchronous pipe for
one of its standard streams, the kind that Rust's standard library, Python and
PowerShell create. It never sends the end of a non-empty file or of `NUL` given
as input, so a remote command reading its input waits forever, and it writes an
output file from its start, over what `>>` keeps. Every stream that carries data
between `hgs.exe` and `ssh.exe` is therefore a console or an overlapped pipe:
`hgs.exe` relays its own input and output unless they are a console, so `< NUL`
ends the remote input and `>> log` appends. Scripts must not run `ssh.exe` with
such streams directly: call `hgs.exe @host …`. `hgs.exe` runs in a kill-on-close
job object, so its `ssh.exe` and `tar.exe` children end when it is killed. Windows
OpenSSH has no connection sharing, so every poll is a full SSH handshake.

**Interactive links.** `hgs @host a SESSION` runs `ssh.exe -t` on the inherited
console. Ctrl+C and Ctrl+Break reach the remote program; `hgs.exe` ignores them while
it waits. Windows OpenSSH runs the `LocalCommand` that marks an established link
through `cmd.exe`, as `type nul > "%%HGS_LINK_MARKER%%"`: the marker path travels in
that environment variable, so a `%` or another special character in the temporary
folder is never expanded. Each link uses a `hgs-link-<pid>-<random>` folder in
`%TEMP%`. A closing console window removes its folder. A client the desktop app
kills leaves its folder behind until the next interactive link, which removes the
folders of ended processes.

**Embedded terminal.** The built-in Terminal runs `hgs.exe @host a SESSION
--existing`, or `hgs.exe a SESSION --existing` for a session on this computer, in a
Windows pseudoconsole (ConPTY). Disconnect closes the pseudoconsole, waits up to a
second and then ends `hgs.exe` and `ssh.exe` through a job object; the session
keeps running on the remote machine or in the session host.

**Machine setup.** `hgs machine setup` (**Install / update HGS…**) needs a source
checkout on Windows. It packs the checkout with the system `tar.exe` and refuses one
whose `install.sh` or `hgs_state.py` has CRLF line endings; `.gitattributes` keeps
`*.sh` and `*.py` in LF. Identity files in `machines.local.json` may be Windows
paths or `~\…`.

**Swarm worker.** Windows has no user service manager. The desktop app runs one
hidden `hgs.exe swarm worker` and restarts it after an exit. The delay doubles
from one second up to five minutes and resets after a run of a minute or more. A
kill-on-close job ends the worker with the app, also after a crash. Without the
desktop app, run `hgs swarm worker` in a terminal or `hgs swarm sync` when needed.
`hgs swarm get` reports `worker: {running, current}` as elsewhere.

**Desktop app.** It uses the `hgs.exe` next to `hgs-tray.exe`, then the one on
`PATH`; `--hgs` or `hgs=` in `tray.conf` overrides both, and the Linux and macOS
fallback to `~/.local/bin` and Homebrew does not apply. Without `--hgs` or `hgs=`,
each start first asks that `hgs.exe` for `session-host status`, before the tray
icon appears; once local sessions are enabled, the app then runs the stable CLI
instead, after `session-host install` when that `hgs.exe` differs from the staged
one and is at least as new. A small *Starting Zerus…* note shows while these
checks take longer than half a second. Settings are in
`%APPDATA%\hgdev\hgs-tray.ini`, with drafts beside it, and one instance runs per
user. When the local box reports `local_sessions: false`, the app offers remote
machines only: in New session, Overview, the tray menu, Accounts, Projects and
search. The Machines page lists this computer as a control console with
**Enable local sessions** (`hgs.exe session-host install`, after a
confirmation). In both modes, this computer's page has no SSH settings or
actions; it shows the session host's state, PID, ConPTY, integrity level and
Windows session. Without remote machines, New session, Accounts and automatic
recovery send you to Machines: to add a remote machine and, where
Machines offers **Enable local sessions**, to enable them. New session then has
**Open Machines**, which only selects this computer there. Automatic recovery
settings of a control console are saved to the first reachable remote machine, and
it is listed at most once every 15 seconds. With local sessions enabled, New
session and Accounts offer this computer without DeepSeek and OpenCode, the shell
entry is **Run PowerShell**, voice messages to its sessions are disabled with the
reason as the tooltip, and it is listed at most once every 5 seconds. In both modes,
Settings → Mobile connection configures the bundled outbound Rust connector on
Windows, including before local sessions are enabled. Start and Stop are explicit;
closing the GUI preserves the connector, session host and native agents. The
first Windows transport controls existing local Codex/Claude/Kimi sessions and
refuses mobile creation, path/worktree/Git/file operations, peers and voice/speech.
Windows desktop presence is not recorded, so desktop activity does not delay
phone alerts. See [Windows gateway setup](mobile-deployment.md#windows-gateway).
Attention notices are tray notifications with the Zerus icon and a taskbar
badge. A start without options opens the workspace, because Windows 11 first
hides a new notification
icon in the overflow; `--background` starts in the notification area only, as a
Startup-folder shortcut should. Starting `hgs-tray.exe` again opens the running
app's workspace, unless with `--background`. After a main-thread watchdog abort,
Windows Error Reporting restarts the app with the same settings in the background
(`--background`, never `--sessions`): a window that opens by itself after a crash
would be a surprise.

**Updates.** The portable zip has no installer or self-update. Stable releases
after 0.42.2 and nightly releases carry the same tested zip. A portable
installation starts on the stable channel; choose Nightly to hear about nightly
builds too. Earlier portable builds saved Nightly as their default, and that saved
choice is kept. A zip that winget installed as `ufna.Zerus` offers its
`winget upgrade` command on the stable channel instead of the download link;
winget carries stable releases only. Settings → Updates reports
a stable release or nightly with a newer product version than the running app and
links to its release page; nightlies of the same product version cannot be told
apart. Stable 0.42.2 and earlier have no Windows build and are never reported as
an update, even when newer: Updates says so and points to the nightly channel.
Dev builds have no Windows build either: on that channel Updates says so, never
reports an update and offers no dev download. The zip is published on GitHub
only: the zerus.dev mirror neither stores it nor lists it in the v1 desktop feed,
and the v2 Dev catalogs have no Windows artifact.

## JSON and external clients

### Retained session metadata API

The desktop Graph destination has been [retired](retired-features/session-graph.md).
Its last implementation is preserved by a named Git tag for a separate future
iteration. The desktop no longer collects or renders Graph data.

`hgs [@host] graph snapshot --json`, `graph changes --json` and `graph receipt
--json` remain bounded native compatibility APIs for released clients. The
host-owned metadata index covers exact saved conversation/run identities,
archives, native children, confirmed forks and verified submission receipts.
It does not change source bindings, send input or change read marks.

Each call reads at most 32 new or changed metadata records and checks at most
512 cached stamps, with a 2 MiB ordinary input budget and a
50 ms cooperative work budget. One manifest may finish within a 16 MiB hard cap;
directory inventory is capped at 10,000 source files. These are bounds, not a
measured latency guarantee. Partial or capped inventory never removes unseen
sources. A malformed source retains last-good facts and reports a coverage gap.
The derived SQLite file is capped at 32 MiB; exhaustion fails visibly instead of
deleting source history or quietly dropping evidence.

Replies contain a native `machine_id`, index `epoch`, monotonic `revision`, typed
`items`, `coverage`, `has_more` and an opaque `cursor`. Continue a snapshot with
`snapshot --cursor CURSOR --json`; after its final page use
`changes --after CURSOR --json`. The optional `--limit` is 1–200. Pages have a
1 MiB metadata budget. A snapshot changed concurrently or a rebuilt index returns
`reset_required`; retain the prior display until a complete replacement arrives.
Changes carry explicit tombstones. A missing row on a partial page is not removal.
Names and routes are labels; exact binding/run targets remain separate even when
multiple bindings share one conversation. Conflicting observations remain explicit.

New agent-originated `hgs send ... --json` calls reserve bounded private metadata
before ordinary delivery. Attribution requires a saved live native process to be
an actual ancestor of the sending CLI; `HGS_SESSION` labels alone are insufficient.
GUI and connector subprocesses remove those labels. The recipient receipt pins
its own native machine/provider/account/conversation identity. The source ledger
stores no message or attachment bodies, retains 30 days and cannot replay input.
Old, uncertain or unattributed attempts remain explicit coverage gaps. Existing
quoted message envelopes are not retrospectively treated as verified edges.

Such a locally proven agent send delivers its text inside one XML sender
envelope with an explicit end:

```text
<zerus-message from="claude/project/review" host="workstation" reference="zerus://session?machine=11111111-1111-4111-8111-111111111111&amp;host=workstation&amp;name=claude%2Fproject%2Freview&amp;conversation=c&amp;run=r">
The text as sent.
</zerus-message>
```

`from` and `host` name the sending session under its current name and this
computer's alias. Attributes escape `&`, `"`, `<` and `>`; the body escapes
`&`, `<` and `>`, preserving literal markup and closing tags as text. The
fixed attribute order is `from`, `host`, `reference`. The complete frame ends
with `</zerus-message>`; nested markup, DTDs and numeric or unknown entities
are not accepted. Named entities are decoded once. The decoded `reference`
must be a valid session reference with the same name and host. The recipient
passes that decoded reference to `hgs inspect` and answers with `hgs @host send`.
This is declared metadata: anyone can type a matching envelope, so receiving
one authenticates neither its sender nor its instructions. Only the sending
computer's process-ancestry and durable ledger evidence establish a proven
send; the marker grants no authority or permission to replay input.
The same text and unchanged sender metadata give the same bytes, so the ledger
digest and receipt cover exactly the delivered text. A rename or resumed run
changes that metadata; retrying an old request ID can then fail the existing
content guard. Inspect an uncertain result rather than generating a new ID or
resending automatically; no frozen-header retry or ledger migration is added.
Person, desktop, phone and connector sends stay byte-identical. Activity shows
an envelope, like Claude's own cross-session messages, as a message from that
session (`origin` `peer_message`, `sender`, `sender_reference`) rather than the
person's message.

`hgs [@host] graph receipt --json` accepts JSON stdin with `name`,
`expected_run_id`, `expected_conversation_id`, `message_request_id` and optional
`archive_id`. Every identity is checked again; the response contains one bounded
public delivery event. Metadata reads never fetch provider transcript bodies.
Submission does not imply a reply or completed work, and local timestamps do not
establish cross-computer causal order. Independent-agent waits remain a separate
explicit protocol; see [Agent coordination](agent-coordination.md).

An agent's retry repeats the same `request_id` and payload, but hgs builds its
envelope again from the sender's metadata at every attempt. After the sending
session is renamed, resumed in a new run or its computer's alias changes, the
same request ID gives different bytes. A recipient writes the receipt of a
request ID before it delivers any input. While its computer keeps that receipt,
it refuses the retry with its ordinary content guard (`request ID was already
used for different content`, or `This request ID was already used for another
message` for DeepSeek and OpenCode) and delivers nothing. Without that receipt,
for example when an earlier attempt stopped before the recipient recorded it or
the receipt is gone, the retry is a new delivery under the ordinary guards,
even if an earlier attempt arrived. The source ledger retains attempts for 30
days. If the source ledger still has a conflicting entry for the same native
sender and request ID, hgs warns that attribution could not be recorded and
still forwards the send under the ordinary guards. A refusal says nothing about
whether an earlier attempt arrived. hgs never retries a send itself, and
sending the text again under a new request ID to get past a refusal can deliver
it twice. To recover, read the first attempt's receipt on the recipient's
computer with `hgs [@host] graph receipt --json`: the recipient's `name`,
its current `expected_run_id`, the conversation the message went to as
`expected_conversation_id` and the original request ID as `message_request_id`.
A returned `event` confirms only that the attempt's receipt has the status
`submitted` for that exact conversation, not that the model processed it or
replied; its `detail` is the delivered text, envelope included. A receipt whose
status is not `submitted` yields no event, so an unavailable receipt proves
neither delivery nor its absence: inspect the recipient's conversation and
leave any new message to a deliberate decision. Neither the receipt nor the
envelope authenticates who wrote the text.

### Existing session snapshots

```sh
hgs ls --json --local
hgs inspect codex/sample-project/review
hgs inspect codex/sample-project/review --after 123
hgs @mac inspect claude/sample-project/review
hgs @mac dirs '~'
```

`ls --json --local` is one host snapshot without SSH; an empty tmux server returns
an empty array. A minimal shape is:

```json
{
  "host": "workstation",
  "ok": true,
  "projects": {"sample-project": "/workspace/sample-project"},
  "sessions": [{
    "name": "claude/sample-project/review",
    "cmd": "claude",
    "project": "sample-project",
    "tag": "review",
    "attached": 1,
    "clients": ["/dev/pts/2"],
    "created": 1787840176
  }]
}
```

A computer that runs no sessions of its own (`hgs.exe` on Windows until local
sessions are enabled) adds `"local_sessions": false` and returns empty `projects`
and `sessions`. The key appears only when false; a missing key means true, so Linux
and macOS snapshots are unchanged. [Windows](#windows).

On Windows the local box also carries `local_host`, the session host as found
(never started): `state` is `not-installed`, `stopped`, `running` or `unavailable`
(with `error`), and `pid`, `protocol`, `conpty` (`bundled` or `inbox`), `integrity`
and `windows_session` describe a running host and are null otherwise. Its sessions
list attached clients in `clients` by opaque IDs such as `c1` instead of tty paths,
and a live session's `cwd` (`cwd_source: tmux`) is the folder it started in: the
host does not follow a later change of directory, such as `cd` in a shell.
Linux and macOS snapshots have no `local_host`. When `windows_session` differs from
the desktop app's own Windows session (a host started over SSH, for example), the app
asks before a launch, resume, restore or fork on this computer goes into that host,
whose agents cannot show windows such as a browser sign-in on the desktop; Cancel
keeps drafts and dialogs as they were.

Tracked live sessions add resumability and activity. Saved sessions use
`state: paused|stopped`, zero attached clients and an empty clients array.
Archives add `archive_id` and `archived_at`; missing state remains compatible with
older live-session snapshots. `ok: false` indicates an unavailable host.
A peer's entry in `hgs ls --json` and the answer of `hgs @alias ls --json` forward
that machine's own listing, so their `host` is the name the machine gives itself
(its first `HGS_SELF` name, otherwise its host name), which may differ from the
alias; an unreachable peer's entry names the alias. Key a peer by the alias you
asked for, as Zerus does; the local entry's `peers` lists those aliases.

A launch with `--launch-id` that no session or record of the snapshot carries, and
whose session existed before the snapshot was taken, has ended: such launches
appear newest first, at most 32, in `ended_launches`, as `{launch_id, name, code,
output}`. `code` is the exit code of the session's supervisor, or null when no
supervisor reported one (a command that runs without one, a supervisor that was
killed); `output` holds the last 8 lines (2 KiB) of the session's screen, followed
by the error that stopped the supervisor (on Windows, as the screen shows both
once the session's output has drained). A detached launch records its launch ID
once the terminal has created the session, and a supervisor that ends within two
minutes without a confirmed conversation records how; a run that confirmed one
leaves its record to carry the launch ID instead. These records live in
`launches/` of the hgs state directory for a week, as long as the desktop keeps a
launch placeholder. The key is omitted when no launch has ended.

A running Zerus desktop records input idle time and screen lock with
`hgs presence set --json` (`{"idle_seconds": 12, "locked": false}` on stdin) every
30 seconds and when the screen locks, unlocks or the owner returns. The listing then
adds `desktop_presence: {idle_seconds, locked, locked_seconds}`, aged to the moment
of listing. It is omitted when the record is more than two minutes old (Zerus
closed, crashed or the machine slept) or written by a clock that has since moved
back. The record is `desktop-presence.json` in the hgs state directory and holds
no other desktop information. The listing also includes
`preferences: {mobile_delivery}` from the shared catalog, or the default when the
catalog is missing or unreadable.

Every session also has `attention_signature` and `read`, either of which may be
`null`. `attention_signature` is a lowercase hex SHA-256 of what currently needs
the owner: run, conversation, the phase when it is `approval`, `input` or
`error`, the attention identity, pending question identities (the IDs and hashes
the mobile connector publishes as `mobile_attention`) and actionable subagent
requests. An approval or input request also contributes `attention_started`, the
time it began, so requests without a native ID (a permission prompt without a
tool call, a tool known only by name) stay distinct; the end of a compaction and
a prompt repeated with the same native tool IDs, tool and input keep that time.
Each external child approval carries its own `attention_id`. It is `null` when nothing needs attention and
does not change on status-only updates. A listing indexes Codex question
transcripts within a shared one-second budget (and a short slice per session),
saving its progress so the next listing resumes; it never scans a long
transcript unbounded. While that index is incomplete, or held by another
process, the signature includes an explicit "questions unknown" component: it is
non-null, never equals an acknowledged signature, and `hgs read` does not
acknowledge it, so an unknown state never hides a question. `read` is the
host's mark for the current conversation:
`{conversation_id, reply_id, attention_signature, at}`. A reply is unread while
`reply_id` differs from `read.reply_id`; attention is unacknowledged while a
non-null signature differs from `read.attention_signature`.

<a id="optional-questions-in-listings"></a>
Every session also has `optional_questions`, the questions Codex asked with
`request_user_input_async` that still wait for the person while the agent
continues without them: `{count, ids, latest_at}`. `count` is exact; `ids`
holds the `question_id` of the newest 32, oldest first, and `latest_at` is when
the newest was asked (seconds since the epoch, or `null` when not recorded).
Without any it is `{"count": 0}`, as for saved, archived and non-Codex sessions.
A question stops counting once its answer appears in the conversation, it is
skipped, or its answer was submitted (**Submitted / Awaiting agent**); one asked
in an earlier run of the same conversation counts until then. Only the main
conversation is indexed: async questions of Codex native subagents are not
counted. The value is `null` while the question index is incomplete or held by
another process (as above) or cannot be read: unknown, never a count of 0, so a
reader keeps the last value it had for the conversation. A listing from an
older hgs has no `optional_questions`; peers forward theirs unchanged. Only
identities leave the host, never question text, and no value depends on the
time of the listing. Optional questions are not attention: they never change
`phase`, and the count does not affect `attention_signature`, which keeps
covering their identities as described above. Pending blocking requests of other
providers stay in `phase` and `pending_question_count` is not used for these
questions.

Kimi sessions report `schedules: {count, recurring, next_at}` folded from the
native cron journal: pending jobs the agent scheduled for itself, how many of them
recur, and the earliest next run in seconds since the epoch (`null` when no
expression can be evaluated). The next run is the first local-time minute matching
a job's five-field expression after its last fire or creation; Kimi's small jitter
is not modeled, and the value never depends on the time of the listing. The object
is `null` while the journal cannot be read. While `schedules.recurring` is
positive, turns opened by those jobs without a prompt from the person do not
advance `reply_id`; a prompt queued into such a turn makes its reply the person's.
`reply_scheduled` is true when the reported reply ended a scheduled turn, which
happens once no recurring job remains or while the count is unknown. DeepSeek
sessions report the same `schedules` object from the active tasks of the native
Schedule catalog while that optional bundle is loaded, with the earliest stored
target as `next_at`; delivered reminders remain ordinary replies. Other
providers report false and omit `schedules`.

```sh
printf '%s' '{"run_id":"…","conversation_id":"…","reply_id":"12:1787840176.5","attention_signature":null}' |
  hgs @mac read codex/sample-project/review --json
```

`hgs [@host] read` applies each non-null field only while it equals the session's
current value in the same run and conversation; other fields are reported as not
applied and the mark does not change. The current value is what a listing would
show at that moment: a Claude approval already answered in Terminal is settled
from the screen first, so its old signature is no longer acknowledged. It prints
`{"ok": true, "applied": {"reply_id": …, "attention_signature": …}, "read": …}`;
an unknown session prints `{"ok": false, "error": "not_found"}` and exits with
status 1. Marks live in `read-marks.json` in the hgs state directory, at most
eight conversations per session, and follow `hgs rename`.
Repeating an already-stored current mark reports it as applied without changing
its timestamp or rewriting the file. Existing-owner writes retain other owners;
admitting a new owner prunes abandoned entries using the current binding set.
Unreadable or malformed existing storage is refused rather than reset. The
global v1 JSON still has population-dependent parse and changed-write costs.
OpenCode renames publish the new binding and its unchanged v1 read marks before
retiring the old binding, in shared-read writer order. If a confirmed native title
change is followed by a local persistence failure, the scoped action remains
uncertain; inspect the retained bindings before choosing another action. An
uncertain receipt never authorizes another native rename automatically.

`inspect` includes conversation, phase/activity, prompt/last message, tools,
subagents, events and cursor. Initial reads return recent events; `--after` advances
the cursor. Reset it when conversation ID changes. `history_truncated` discloses
journal limits; `tracked: false` identifies an unbound session. Folder/Git fields
include `cwd`, `cwd_source`, `cwd_canonical`, `git_root`, `git_common_dir`, branch,
worktree, detached state and metadata status. Git metadata has a bounded read and
15-second cache, independent of pause eligibility.

`hgs [@host] git-status --path /absolute/checkout --json` provides independent
read-only publication evidence for the GUI. It supports worktrees and subfolders,
caps Git output at 1 MiB and the whole check at three seconds, and never prompts
for credentials. Ahead/behind counts compare HEAD with its configured local
tracking ref; `remote_state: verified` requires that ref's OID to match an exact
read-only `ls-remote` result. `changed` and `missing` do not imply synchronization.
`no_upstream` requires a completed lookup that finds no configured upstream, and
a local tracking ref counts as absent (`changed` while the remote branch exists)
only when its lookup completes without finding it. A failed or timed-out upstream
or tracking ref lookup leaves `remote_state: unknown`.

Subagent count fields distinguish active, completed, total, completeness and
source; unknown totals are null. Up to six active/unreviewed previews accompany
the available full roster. Profile groups without individual IDs remain groups.

`hgs [@host] send SESSION --json` accepts stdin containing a UUID `request_id`,
`text`, `expected_run_id`, `expected_conversation_id` and optional attachments
`{name, mime, data_base64, reference?}`. Unique `[Image #1]` / `[File #2]` references
place attachments at their first marker; otherwise they append. IDs come from a
fresh inspection. `status: submitted` confirms native input delivery, not a model
reply. Saved receipts make identical requests idempotent. An uncertain result
needs manual inspection before another request. `agent_id` is supported only for
verified DeepSeek continuable children. Questions carry request identity/hash
through `pending_questions`; answers use the exact unchanged request.

## Validation

Use [CONTRIBUTING.md](../CONTRIBUTING.md) for standard checks. Integration tests
use synthetic fixtures and isolated tmux servers; do not run them against live
agent state. A separately installed native CLI or real desktop may be required.

```sh
cargo build --locked
cargo test --locked
HGS_TEST_BIN="$PWD/target/debug/hgs" bash tests/test_hgs.sh
HGS_TEST_BIN="$PWD/target/debug/hgs" python3 tests/test_pause.py
HGS_TEST_BIN="$PWD/target/debug/hgs" python3 tests/test_accounts.py
HGS_TEST_BIN="$PWD/target/debug/hgs" python3 tests/test_account_lifecycle.py
```

On macOS, the last suite can also verify SSH/tmux entry into the graphical security
session and needs the installed HGS desktop session service. The Mach protocol
can be checked without accessing Keychain:

```sh
clang -Wno-deprecated-declarations tests/test_macos_session_protocol.c \
  -framework Security -framework CoreFoundation -lbsm -o /tmp/hgs-session-protocol-test
/tmp/hgs-session-protocol-test
```

Process coverage includes exact call identity, background tasks, stale generations,
independent output cursors, targeted stopping and offline/read-position behavior.
Native DeepSeek tests use a local keyless model rather than paid API requests.
