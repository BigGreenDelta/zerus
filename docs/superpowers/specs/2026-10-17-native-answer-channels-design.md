# Native answer channels with an unviewed-terminal guarantee

Status: design for owner review (2026-10-17). Tracking: `zerus-0yrj`; approval
layer A: `zerus-6uhz`.

## Problem

Zerus answers native agent questions and approvals by reading the agent's
terminal and typing keys. Before any key, the CLI verifies that the complete
panel on screen is exactly the pending request. That verification fails when the
panel is taller than the pane.

Agent panes are created detached. Until a client attaches they keep tmux's
`default-size`, 80x24 (the Windows host mirrors it as `host::SIZE`). A Claude
question with several wrapped option descriptions, or an approval for a long
command, does not fit in 24 rows. Its heading scrolls off and the CLI reports
`can_answer: false` ("Claude's question is not fully visible. Open Terminal to
check it."). Opening Terminal attaches a client, the pane takes the client's
size, the agent redraws and the next inspection accepts the panel. Users
therefore see a disabled Submit until they visit Terminal.

Evidence (2026-10-17):

- Owner reports from the macOS GUI (questions) and from a Mac session (a
  dangerous `rm` approval showing "Zerus cannot read this approval completely").
- Never-attached agent panes on the Linux host are 80x24; the reported session
  became 88x35 after a GUI Terminal attach.
- Isolated real Claude 2.1.296 with a localhost API: four Cyrillic options at
  80x24 give `can_answer: false`; after `resize-window` to 88x35 they give
  `true`. Claude 2.1.296 leaves no scrollback (`history_size=0`), so the
  clipped rows cannot be recovered from tmux history.
- Closed `zerus-d74` had already reproduced the rejection at 95x24.

A fixed larger size only moves the limit: any panel can outgrow it.

## Goals

- A pending question or approval that the agent can accept is answerable from
  Activity and phones regardless of terminal size, wrapping, colors or
  animation.
- Every answer reaches the native agent at most once. Unknown delivery is
  reported as uncertain and is never retried automatically.
- When Zerus cannot answer, the card always says why. A disabled Submit never
  comes with an empty reason.
- A person working in Terminal keeps full control. Zerus never resizes or
  types into a terminal that a client is viewing.
- Existing API v1, `hgs answer` receipts, durable request identities and
  status semantics stay compatible.

## Non-goals

- Changing how panels without a native channel are verified. Exact full-panel
  verification remains the rule for screen delivery.
- Automatic approvals policy. Existing rules (one-time grants, durable audit
  reservation) apply unchanged to any new transport.
- Replacing the TUI with a provider SDK or headless mode.

## Decision

Owner decision 2026-10-17: **C as the base, A on top as the guarantee.**

- **C, native answer channels.** Where an agent exposes a structured answer
  path, Zerus answers through it, without the screen. Each agent gets its own
  adapter.
- **A, unviewed-terminal growth.** For panels that exist only on screen, and as
  the fallback whenever a C channel is unavailable, Zerus grows the height of a
  pane that has no attached client until the panel fits. It then verifies the
  panel exactly as today.

Answer routing per request: use C when its channel is live and bound to the
exact request; otherwise use screen delivery after A; otherwise report a
specific reason.

## Panel inventory

| Panel (card `source`) | Agent | Channel C | Layer A |
|---|---|---|---|
| `hook` (AskUserQuestion) | Claude | PermissionRequest hook, verified by spike | fallback |
| `claude_tool_approval` | Claude | PermissionRequest hook (allow or deny), main agent (P3) | in `zerus-6uhz` |
| Claude's own questions (`refusal_fallback_preference`) | Claude | PermissionRequest without PreToolUse, to verify | fallback |
| `claude_folder_trust`, `claude_permission_mode`, `claude_resume`, `claude_usage_limit`, terminal safety, auto-mode chooser | Claude | none known | only path |
| `codex_async` | Codex | none on 0.160.1–0.162.1 (see Other agents) | only path, with A |
| `codex_folder_trust`, `codex_hooks_trust`, `codex_update`, `codex_goal_resume` | Codex | none known | only path |
| `kimi_wire` questions | Kimi | none: the TUI resolves questions in process (`zerus-n88z`) | only path |
| `kimi_folder_trust`, `kimi_cache_hint` | Kimi | none known | only path |
| OpenCode, DeepSeek native requests | OpenCode, DeepSeek | already native APIs | not applicable |

## C for Claude questions

### Verified behavior (spike, Claude 2.1.296)

The spike used a real Claude 2.1.296 TUI, a localhost API and an isolated tmux
socket. The hook script was throwaway.

1. A `PermissionRequest` command hook (matcher `AskUserQuestion`) that prints
   `hookSpecificOutput.decision = {behavior: "allow", updatedInput: {questions,
   answers: {<question text>: <answer>}}}` answers without any keys. The model
   receives exactly those answers. With an immediate answer the panel never
   shows.
2. The hook and the native panel run in parallel. The panel is visible 0.5 s
   after the request while the hook still waits. Whichever answers first wins.
3. If the person answers in Terminal first, Claude ignores the waiting hook
   and does not terminate it: the process was still alive 2 s after the turn
   ended and received no SIGTERM.
4. If the hook reaches its `timeout`, the panel stays for the person. A
   timeout of 100000 delivered an answer at 75 s, so there is no hidden 60 s
   cap. Longer waits are unverified.
5. A `PreToolUse` hook with `permissionDecision: "allow"` and `updatedInput`
   also answers. It runs before anyone sees the question, so it cannot carry a
   human answer.

The agent-approval session's approval spike (`zerus-pdmm`, same setup) adds the
following:

6. When the hook times out, and when the person declines in Terminal (Esc or
   No) while the hook waits, Claude sends the hook SIGTERM. A Terminal Yes
   sends no signal. A Terminal decline writes an `is_error` `tool_result` ("The
   user doesn't want to proceed with this tool use...") for the same
   `tool_use_id`. Every Terminal outcome therefore leaves a transcript
   `tool_result` for the bound tool use.
7. A silent journaling hook next to the deciding hook does not interfere; the
   decision applies.
8. `PermissionRequest` carries `cwd`, `effort`, `hook_event_name`,
   `permission_mode`, `permission_suggestions`, `prompt_id`, `session_id`,
   `tool_input`, `tool_name`, `transcript_path`, and `agent_id` for children.
   It has no `tool_use_id`. `prompt_id` covers the whole user prompt and is
   shared by concurrent requests, so it is not an identity.

### Hook registration

- hgs adds a second `PermissionRequest` entry to the account settings it
  already manages: matcher `AskUserQuestion`, command `hgs __state
  answer-channel`, and a long timeout (proposed 86400 s; see open questions).
  The existing journaling entry keeps its 5 s timeout and its behavior.
- Registration follows the `versioned_events` pattern. It is written only for
  a Claude version read from the native CLI and qualified for this channel
  (2.1.296 first). It is pruned before an unqualified version starts.
- Sessions started before registration keep screen delivery until their next
  ordinary start. Nothing is restarted for this.
- The channel process exits immediately, with no output, when it is not
  running inside a tracked hgs session (`HGS_SESSION` and `HGS_RUN_ID` must
  match a live record). Hooks shared by the account therefore never affect
  Claude started outside Zerus.

### Rendezvous

All state lives under the session state directory in
`native_answers/<sha256(run_id)>/<channel_id>/`. Every file is written
atomically. `channel_id` is `sha256(run_id, conversation_id, question_id,
question_hash)`.

The channel process:

1. Binds the request. The event's `session_id` must equal the saved
   conversation; a Claude of another conversation that inherited the session's
   variables, or an event without one, never binds. `PermissionRequest`
   carries no `tool_use_id`, so the process selects the single pending
   main-agent `hook` card of this run whose recorded `tool_input` equals the
   event's `tool_input` exactly. With zero or several candidates it exits
   silently, and screen delivery remains.
2. Takes sole ownership of the channel directory with `owner.lock`, held until
   it exits. A second worker for the same question, or a channel an earlier
   worker already used, exits silently without writing anything. Only the
   owner writes `waiting.json` (protocol version, pid, process start time, run,
   conversation, question id and hash, `started_at` and its own deadline) and
   `closed.json`.
3. Polls every 100-200 ms (no sockets, so it is portable to Windows) and exits
   silently when any of these holds:
   - the card is no longer pending, or a transcript `tool_result` exists for
     the bound `tool_use_id` (answered, declined or cancelled in Terminal);
   - the run or conversation changed, the record says the session ended, or
     the parent Claude process is gone;
   - its deadline passed.
   It writes `closed.json` with the reason before exiting.
4. Claims an answer by atomically renaming `answer.json` to `claimed.json`.
   Then it re-checks steps 1 and 3, writes `emitted.json` (with the answer
   `request_id`) and only after that prints the decision and exits.
5. Handles SIGTERM (hook timeout, Terminal decline) by writing `closed.json`
   with the reason and whether `emitted.json` exists, then exiting. Its own
   deadline is shorter than the configured hook timeout, so Windows, which
   sends no SIGTERM, closes the same way.

`hgs answer` for a `hook` card, under the existing per-run question lock and
`question_receipts/<request_id>.json`:

1. If `waiting.json` names a live channel process (pid plus start time) for the
   same identity, it writes `answer.json` with the request id, answers and
   digest, and the receipt becomes `in_progress` with transport `claude_hook`.
2. It waits up to 3 s for `claimed.json`. If the answer is not claimed in
   time, it retracts it by atomically renaming `answer.json` to
   `retracted.json`. Exactly one of claim or retract can succeed. After a
   successful retract it falls back to screen delivery, if the screen
   verifies.
3. A claimed answer belongs to the channel and never goes to the screen.
   `emitted.json` (written before the decision is printed) within 3 s leads to
   the confirmation wait: `PostToolUse` or `QuestionAnswered` for the bound
   `tool_use_id`, whose transcript `tool_result` matches the delivered
   answers, makes the receipt `answered`. Anything else after a claim, such as
   a close, a dead claimant or no confirmation within the delivery budget, is
   `uncertain`; it is never retried and never sent through another transport.
4. While `answer.json`, `claimed.json` or `emitted.json` exists, the question
   is locked: inspection reports it unanswerable with that reason, and any new
   `hgs answer` request is refused without sending anything. Only an unclaimed
   (retracted) offer or a channel closed without a claim leaves the screen
   path open.
5. The same `request_id` returns the stored receipt, as today.

### Answer mapping

- A single choice maps to the option label.
- Multiple choices map to labels in source order. The exact join and escaping
  must be qualified against the native result text, as the spike only covered
  a single choice.
- Other maps to the person's text. Control characters are rejected as in
  existing validation.
- Several questions map to one `answers` object keyed by each exact question
  text. Duplicate question texts make the channel refuse that card, and screen
  delivery remains.
- `updatedInput.questions` is the hook event's own `tool_input.questions`,
  passed through unchanged.

### Races

| Situation | Outcome |
|---|---|
| Terminal answer first | Channel sees the `tool_result` and exits; the Zerus card resolves as answered in Terminal; a later Zerus submit fails with "already answered". |
| Two viewers submit (Mac GUI, phone) | The per-run lock serializes them; the first claims; the second sees a pending claim or receipt and is rejected. |
| Channel process killed or timed out | `waiting.json` no longer names a live process; inspection and answer use screen delivery after A. |
| hgs upgraded while a channel waits | The old process keeps its protocol version; `hgs answer` writes only to a matching version or falls back to the screen. |
| Compaction, clear, model switch, run change | Identity checks fail; the channel exits; the card is refreshed. |
| Claude ignores a printed decision | `emitted` without confirmation leads to `uncertain`, and the panel stays in Terminal for the person. |

## C for Claude approvals

The same channel answers `claude_tool_approval`. The agent-approval session
owns this adapter (`zerus-pdmm`). Its spike verified the following on Claude
2.1.296:

- Allow maps to `behavior: "allow"` without `updatedPermissions`. The tool
  runs once and no settings file changes.
- No maps to `behavior: "deny"` with `interrupt: true`. This matches the TUI
  No: "Interrupted · What should Claude do instead?", with no `tool_result`
  and no Stop hook.
- "No, and tell Claude what to do differently" maps to `behavior: "deny"` with
  the person's message and without `interrupt`. The model receives it as an
  `is_error` `tool_result`, and the turn continues.
- A dangerous `rm` in the main agent under `bypassPermissions` also emits
  `PermissionRequest`, so safety confirmations use C. Only a
  `claude_terminal_safety` panel without any hook stays on the A path.
- Binding uses the active `PreToolUse` (no `PostToolUse` yet) with exactly
  equal `tool_name` and `tool_input`; its `tool_use_id` becomes the identity.
  Identical inputs waiting at the same time are ambiguous: the channel exits
  silently and the screen remains. Children also match `agent_id`.
- Claude runs two Bash calls from one message sequentially. Two Reads outside
  the project give two concurrent `PermissionRequest` events ("1 of 2"), and
  each hook process's decision reaches its own `tool_use_id`.
- A Terminal answer neither kills the hook nor signals it; a late decision is
  ignored.
- The channel carries only the one-time Allow and the two Deny forms of the
  hook request. Broader TUI choices ("always allow", auto mode) exist only on
  the screen and keep their explicit second Confirm there.

Automatic approvals keep their durable private audit reservation before
delivery. The channel is only another transport.

### Implementation (P3, `zerus-pdmm`)

- One channel worker serves both kinds. The registered group has no matcher,
  and an AskUserQuestion-only group from an earlier hgs is widened in place.
  Protocol files and `route`, `deliver`, `own`, `offer` and `claim` are
  unchanged.
- hgs records each active main-agent `PreToolUse` as the tool name and a
  digest of its input, keyed by `tool_use_id`. Turn boundaries clear the
  records, and more than 64 make every binding ambiguous. A request binds only
  to the one record that began before it.
- Each poll and the claim check the exact bound identity: process, run,
  conversation, tool use, input and request time. A later identical request is
  not the bound one and does not close it.
- A Terminal answer sends the hook no signal. Inspection, `hgs answer` and the
  worker's claim therefore also require Terminal to show a chooser. A complete
  panel must match the request and no recorded child request; a clipped panel
  counts only while no child request is recorded.
- The worker takes an approval offer only while that proof holds, through the
  same identity and capture as `hgs answer`. Until then the offer stays
  unclaimed, and `hgs answer` retracts it with nothing sent.
  - A retraction retires that channel, so the request continues on the screen.
  - A refused agent identity never lets a decision go, before or after the
    claim. This covers a pause, a provider error, another process and a
    terminal of another run. After a claim the answer then stays uncertain and
    locked.
  - Only a capture that fails after the identity holds proves nothing and never
    blocks the claim. Claude ignores a decision that arrives after a Terminal
    answer.
- A hook binds only to a request recorded in the last ten seconds. It binds
  only to the one active tool call with the event's tool and input that began
  before the hook started, as the first record it reads shows it. PreToolUse is
  journaled before Claude asks. Without exactly one such call, the hook exits
  and the screen remains. A later identical call is never adopted.
- Before and after the claim, the screen is matched against the record that
  was just checked.
- The card always discloses the whole bound request (`request_body`): every
  field of a Bash input, or the whole input of another tool. It never shows
  another request's panel as the operation the hook grants.
- The card's options:
  - Claude's options are listed verbatim, and `panel_verified` is true, only
    when a complete panel proves the exact operation (`proves`). That means the
    main agent's "Bash command" title, the full command, the description if any,
    and no other input field. The bound call must also be the only active main
    Bash call (`channel::alone`), because the command match ignores how
    whitespace wraps.
  - The legacy file-name or host match is not proof. A Read of another file
    with the same name can be queued in front, shown as "1 of 2" while the hook
    waits for the second.
  - Any other complete panel is left to the screen path, which acts on what it
    shows.
  - A clipped panel gives one-time Yes and No. An unreadable terminal gives no
    hook card.
  - Its free-text field, "Tell Claude what to do instead", maps to deny with
    the message.
  - Broader options, and the keys after an unclaimed offer, use only a proven
    panel.
- A visible safety warning is added, as its rows below the last fence, only
  when it belongs to this request: the panel proves it, or, while the bound
  call is the only active Bash call, the visible command rows of a clipped
  panel end this command. Any diagnostic gutter below a visible chooser's last
  fence that cannot be attributed and shown this way means no hook card,
  whatever its text.
- When the screen path answers a panel of another queued request, its
  pre-existing bookkeeping settles the recorded request. That request then
  waits in Terminal. This path is unchanged.
- Automatic approvals accept `claude_hook` only with `panel_verified`, so only
  for a proven Bash panel. This is the same one-time grant as on the screen
  path.
- Children are not answered through the channel. In Claude 2.1.296 they run in
  the background, their requests appear only in the agents view, and switching
  that view terminates the waiting hook. Claude then repeats `PreToolUse` and
  `PermissionRequest` for the same tool use. No screen proof can show that a
  child's request still waits.

### Windows (`zerus-sser`)

- Windows registers the same group, in Claude Code's exec form only
  (`command` names the stable CLI, `args` `__state answer-channel`): Claude
  starts hgs itself, so the hook process is the channel worker and its
  recorded pid and start time identify it. Settings that keep the shell form
  of an older Claude Code get no channel, since Git Bash would run between
  Claude and hgs. hgs never wrote the AskUserQuestion-only group there.
- No SIGTERM reaches the worker. A Terminal answer, decline or cancellation
  ends it through the record, the transcript or Claude ending the hook
  process; its own deadline stays below the hook timeout.
- A rename is not exclusive on Windows, so claim and retract take
  `handover.lock`. While holding it, a rename refused because a scanner or a
  reader without delete sharing holds `answer.json` is tried again for up to
  one second; a refused rename moved nothing. The outcome rules are unchanged.

## A, unviewed-terminal growth

Implemented first for approvals in `zerus-6uhz` (agent-approval session) and
then reused here.

- Backend primitive: `TerminalBackend::grow_unviewed(pane, rows) -> bool`.
  - tmux acts only when `session_attached == 0` and the window has no
    window-local `window-size`. It runs `resize-window -y ROWS`, then
    `set -wu window-size`, so the next client sets the size again. Width never
    changes.
  - The Windows session host answers a `grow_unviewed` request only while no
    client is attached to the pane: it keeps the width, never shrinks, and the
    next attaching client sets the size again (`zerus-5dox`). A host that
    predates the request answers `unsupported`, which the backend reports as
    `false` (fail closed).
  - tmux 3.8 behavior was verified on isolated sockets: after unsetting, the
    size persists while detached; an attach takes the client's size; a resize
    while attached reverts to the client's size.
- Shared helper next to `question_terminal`: given a pane and a parser that
  reports a typed `Clipped` result, double the height (24, 48, 96, 192 as a
  shared cap), wait up to 1 s per step for the agent to redraw, and re-parse.
  The decision still comes only from the existing exact full-panel match.
- Every screen parser (Claude questions and approvals, Codex and Kimi panels,
  startup choosers) reports `Clipped` instead of a free-text visibility
  error.
- Growth runs only during inspection (the listing path), only while a
  recorded request is pending. It takes the per-run answer lock without
  waiting, so it never runs in the middle of an answer; if the lock is busy it
  is skipped and the next poll retries. Delivery never grows a pane: it
  delivers only what inspection showed and verified, so a person never
  approves a screen they did not see.
- With a client attached, nothing is resized. The reason tells the person to
  enlarge Terminal, which they are looking at.
- If the panel is still clipped at the cap, the reason says the request is too
  large for Zerus to read and must be answered in Terminal.

## Inspection contract

- `can_answer` is true when a C channel is live and bound to the exact card, or
  when screen verification succeeds after A.
- `answer_transport` adds `claude_hook`. Existing values keep their meaning.
  Older GUIs and phones only check `can_answer`, so they remain compatible, and
  `hgs answer` chooses the route.
- `answer_unavailable_reason` is never empty when `can_answer` is false. Specific
  reasons: client attached and too small; too large at the cap; channel
  unavailable and screen unverifiable; session not live.
- The desktop card shows the reason instead of "Your choices are ready to send"
  whenever Submit is disabled. A GUI regression test pins this. The current
  `toString` fallback keeps an empty string; close that latent gap.

## Other agents

Codex and Kimi get the same adapter interface (`bind`, `available`, `deliver`,
`confirm`, `close`). Each adapter starts with its own spike on a pinned
version, like the Claude spike:

- Codex: whether hooks or the app-server can answer `codex_async` questions,
  and approvals while the TUI runs.
- Kimi: whether the wire protocol already observed for `kimi_wire` questions
  can carry answers. Kimi 2.1.1 answered no (`zerus-n88z`): its TUI runs the
  agent engine in process and resolves questions and approvals only through its
  own panels; hooks can only deny, `wire.jsonl` is output, and the local server
  API belongs to a separate `kimi web` process. Kimi panels use screen delivery
  with A.

Until an adapter is qualified, these panels use screen delivery with A.

Codex result (`zerus-zeb2`, 2026-10-18, pinned 0.160.1 and 0.162.0 plus
0.162.1, isolated homes with `--no-daemon`): no channel. A synchronous
`PermissionRequest` hook runs before Codex draws its approval panel, which
shows only after the hook returns without a decision, so a waiting hook would
hide the Terminal choice. With `async: true` the panel shows at once but the
hook's decision is ignored. The decision carries only allow or deny with a
message; `updatedInput`, `updatedPermissions` and `interrupt` fail closed, so
hooks cannot answer questions either. hgs runs Codex without its app-server
daemon, so no second client exists. `codex_async` questions keep screen
delivery with A: an open panel taller than its pane scrolls its answers and
names the visible row in its key hints, which marks it clipped.

## Testing and qualification

- Unit tests:
  - rendezvous claim and retract races with concurrent fake channel and
    answer processes;
  - identity binding, including duplicate inputs;
  - answer mapping;
  - receipt idempotence;
  - growth loop with a fake backend (grow, attached pane, cap, busy lock).
- Native tests with real Claude on pinned versions (2.1.296 and retained
  ones), a localhost API and isolated sockets:
  - single, multiple, Other and several-question answers through the channel;
  - an 80x24 pane with a panel taller than the pane, answered through the
    channel without growth (the `zerus-0yrj` regression);
  - Terminal answer first: the channel exits and no late answer arrives;
  - channel killed mid-wait, then screen fallback after A;
  - hook timeout, then screen fallback;
  - double submit from two clients;
  - session end while waiting;
  - A-only panels at 80x24 (trust, permission mode, resume).
- GUI test: disabled Submit always shows a non-empty reason.
- The full CI matrix runs the native suites on Linux, macOS and Windows. A
  skipped native suite is not compatibility evidence.

## Rollout

| Phase | Owner | Content |
|---|---|---|
| P0 | agent-approval session | `grow_unviewed` plus approvals on A (`zerus-6uhz`), merged first |
| P1 | agent-question session | Shared growth helper; typed `Clipped` for all screen parsers; reasons never empty in the GUI. Immediate relief for `zerus-0yrj`. |
| P2 | agent-question session | Claude question channel C, versioned registration, rendezvous, native coverage |
| P3 | agent-approval session | Claude approval channel C |
| P4 | to assign | Codex and Kimi spikes; adapters where feasible |
| P5 | to assign | Claude's own questions through C; host and Windows parity for A |

Each phase ships independently through the normal CI gate. P1 alone already
removes the reported symptom for unviewed panes. P2 and P3 make the main paths
independent of the screen.

## Open questions

1. The maximum hook timeout Claude accepts, and its behavior beyond 600 s.
   This needs a one-time long qualification per pinned version.
2. Whether a running Claude reloads hook settings, or only reads them at
   start.
3. Exact multi-choice and Other result formatting through `updatedInput`.
4. Windows: hook process lifetime and parent-exit detection under the
   Windows host. Answered for Claude Code 2.1.296 (`zerus-sser`, 2026-10-18):
   in the native Windows suite a Terminal Yes or Esc ended the waiting
   worker, a killed worker left the approval to the verified screen, and no
   worker outlived its session or the suite's host.
