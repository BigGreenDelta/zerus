# Overmind — design (stage 1: core)

Date: 2026-10-11. Status: draft for review. Branch: `overmind`.

This document fixes the architecture of Overmind as a whole and specifies its
first stage, the core, in detail. Stages 2–4 get their own specs; the decisions
already taken for them are recorded at the end so the core leaves room for them.
It supersedes `2026-10-09-shepherd-prompt-queue-design.md`.

## Problem

While agents work, the user keeps having the next ideas. Today they end up in a
separate notebook, are typed into the wrong session, or are lost. The user wants
Zerus to hold them as tasks whose content is a ready prompt, hand them to
sessions by hand or automatically, and eventually work through a manager agent
that is always available, including from the phone.

## What Overmind becomes

Overmind is one system with four levels. Each level builds on the previous one.

1. **Task tracker.** Tasks are prompts. A prompt written in a session composer
   can be saved as a task instead of being sent.
2. **Session and project queue.** Tasks are pinned to a session or kept in a
   project pool and sent to a session by hand ("Send", "Send next").
3. **Autopilot.** A session with autopilot on receives matching tasks
   automatically, with service prompts before work, between tasks, for status
   checks and for done checks. Automatic sending exists only here.
4. **Manager.** Named manager sessions talk to the user, create and refine
   tasks, follow the autopilot, intervene on request and delegate all work so
   they stay free. Voice from the mobile app comes later.

Delivery order: **Core (levels 1–2) → Autopilot (3) → Beads link → Manager (4).**

The process model comes from [vikunja-mcp](https://github.com/ufna/vikunja-mcp):
stages with gates enforced by narrow tools, a journal on every card, Done
reserved for humans. Vikunja itself is not used. vikunja-mcp is pull-based (the
agent asks for work); Overmind is push-based: Zerus hands a task to a session,
and the agent reports through gated tools. "Is the task done" is answered by an
explicit report, not guessed from idleness.

## Decisions taken in discussion

| Topic | Decision |
|---|---|
| Storage | Own database in `hgs`; beads and similar trackers are linked through adapters (stage 3). |
| Placement | One logical database replicated to all of the user's machines. |
| Dispatch authority | Each machine sends tasks only into its own sessions. |
| Matching (stage 2) | Project plus requirement labels. |
| Lifecycle | Default workflow as in vikunja-mcp; workflows are data and can be edited per project. |
| Agent channel | CLI with gates, MCP as a thin wrapper; caller identity from the session environment. |
| Context between tasks (stage 2) | Configurable `keep`/`compact`/`clear`, default `clear`. |
| Settings | Stored in the shared database, layered: built-in → profile → project → session. |
| Beads (stage 3) | Linked tasks: import by filter, two-way status for linked tasks, local tasks exported only on explicit request. |
| Manager (stage 4) | Named managers with configurable scope; user-facing, always free, delegates work; never replaces algorithmic dispatch. |
| Composer "Add as task" | Default target: Queue of this session, pinned to it. |
| Level 2 | Manual sending only; automatic sending is always the autopilot. |
| Desktop board | Kanban by stages in a project-level **Tasks** tab next to Activity, Terminal and Processes. |
| Replication | Per-machine SQLite event log synchronized over the swarm channel. |
| CLI | One namespace: `hgs overmind …`. |
| Mobile | Full parity with the desktop in stage 1. |

## Architecture overview

```
 Desktop GUI ──┐                       ┌── Android app
 (Tasks tab,   │ hgs overmind …        │   (relay ops overmind_read/write)
  composer)    ▼                       ▼
          ┌───────────────┐   relay   ┌───────────┐
          │ hgs overmind  │◀──────────│ connector │ (gateway machine)
          │  CLI + gates  │           └───────────┘
          │  workflow eng.│
          │  event store  │◀── MCP stdio server ◀── agent in a session
          └──────┬────────┘
                 │ swarm SSH channel: event exchange, blob fetch,
                 │ bundle delivery to the session's host
          ┌──────▼────────┐
          │ other machine │  same store, same CLI
          └───────────────┘
```

Every machine holds a full replica. Reads are always local. Writes are local
events. Only sending a task into a session runs on the session's host.

## Part 1. Concepts and data model

### Task

| Field | Meaning |
|---|---|
| `uuid` | Identity. UUID v4, or the client `request_id` of the creating request (the phone and any idempotent client). |
| `ref` | Display reference `om-` + the shortest unambiguous base32 prefix of a hash of `uuid` (at least 5 characters), like a short Git SHA. |
| `project` | Swarm project id. Optional only for tasks pinned to a session outside any project. |
| `title` | Defaults to the first line of the prompt, at most 200 characters. |
| `prompt` | Text, at most 64 KiB, same character rules as `hgs send`. |
| `attachments` | Up to 8 images, 10 MiB each, 20 MiB total, stored by SHA-256. |
| `stage` | A stage of the project workflow. |
| `priority` | P0–P4, default P2. |
| `rank` | Manual order inside a stage (fractional index string of at most 64 characters 0-9A-Za-z, compared as bytes). A task keeps the order of its column when it changes stage: the transition carries a rank at the end of the target column. Ranks appended to a column grow by one character per 62-fold more tasks. A column whose ranks tie or run out is renumbered evenly when a person reorders it. |
| `labels` | Plain labels (`ui`) and requirement labels (`agent:codex`, `machine:<name>`). Reserved: `blocked`, `epic`, `epic-ready`, `bug`, `reviewed`, `review-failed`. |
| `pinned` | Session the task is reserved for: machine uuid + session name. Follows renames. |
| `assignee` | Session that took the task into an active stage. |
| `reviewer` | Session the task was sent to for review, if any. |
| `resume_stage` | Active stage to return to after Your Call. |
| `relations` | `parent` (epic), `follows` (predecessors), `related`. |
| `links` | External tracker links (stage 3). Reserved now. |
| `journal` | Ordered entries with frozen ASCII markers. |
| `flags` | `delivery_uncertain`, `conflict`, `orphaned`. |

Journal markers: `[created]`, `[edited]`, `[moved]`, `[claim]`, `[sent]`,
`[sent?]`, `[unclaim]`, `[spec]`, `[worklog]`, `[review]`, `[needs-human]`,
`[answer]`, `[blocked]`, `[decompose]`, `[filed-by-agent]`, `[attach]`,
`[comment]`, `[done]`, `[rejected]`, `[conflict]`. Markers are matched by code and
never translated. The prose after a marker follows the project language.

### Actors

| Actor | How it is recognized |
|---|---|
| `human` | GUI, phone or CLI not running inside a Zerus session. Carries `machine` and, from the phone, a device label. |
| `agent:<session>` | `HGS_SESSION` and `HGS_RUN_ID` match a live binding on this host and that run has a live, matching tmux pane: the same check hook events pass. |
| `autopilot` | Stage 2. |
| `manager:<session>` | Stage 4. |

A process with a session environment that fails the check is refused, never
treated as human. The check is a guardrail against agent mistakes, not a
security boundary: an agent that strips its environment is not stopped.

## Part 2. Storage, replication and conflicts

### Local store

`<state root>/overmind/` (`$HGS_STATE_DIR/overmind`, by default `~/.local/state/hgs/sessions/overmind`), mode 0700:

- `overmind.sqlite3` (0600, WAL):
  - `events` — append-only log, unique `(origin, seq)` and `id`, with a local
    insertion counter `n` that serves as the change cursor;
  - task, workflow and settings caches — derived, rebuilt from `events` at any time;
  - `receipts` — `request_id` → hash of the command and its body, and the
    result, for idempotent writes;
  - `meta` — the store's own origin id, a UUID created with the store;
- `blobs/<aa>/<sha256>` (0600, immutable) — attachment bytes;
- `rev` — a small file replaced atomically after every committed change; the
  desktop watches it.

### Events

```json
{"id": "uuid", "origin": "store-uuid", "seq": 41, "lamport": 1203,
 "at": 1760170000, "v": 1, "actor": {"kind": "human", "machine": "uuid"},
 "scope": "task|project|global", "task": "uuid", "project": "id",
 "kind": "transition", "base": "event-id-or-null", "request_id": "uuid",
 "payload": {}}
```

`origin` is the store that wrote the event, not the machine: each store has
its own origin id, created with the store, and numbers its events from `seq` 1.
`actor.machine` names the machine. A store that was lost and created again
therefore never collides with events written earlier, even when it writes
before its peers refill it. Copying a whole store directory to a second
machine, or restoring an old copy of one, is unsupported: the copy writes under
the same origin, and its peers refuse the colliding events and report them.

`lamport` is the maximum seen plus one, so a causally later event always sorts
later. Kinds: `task_created`, `field_set`, `transition`, `journal`, `rank_set`,
`label_set`, `relation_set`, `pin_set`, `reviewer_set`, `flag_set`,
`attachment_added`, `workflow_set`, `settings_set`, `session_renamed`,
`task_deleted`.

### Folding events into state

Events of a task are applied in the total order `(lamport, origin, seq)`.

- `field_set`, `rank_set`, `pin_set`: last writer wins per field. An accepted
  transition that changes the stage also sets the rank it carries.
- `journal`: appended; order is the total order.
- `transition`: compare-and-set. `base` is the last accepted transition the
  writer saw. A transition is accepted only if `base` equals the current head
  and the workflow gate passes for the state at that point. Concurrent
  transitions with the same base: the first in total order wins; the others
  become `[rejected]` journal entries with the reason.

When an event arrives that sorts before events already applied, the task is
folded again. If that overturns an accepted transition that already had an
external effect (a `[sent]` entry), the task gets the `conflict` flag and a
`[conflict]` entry, and it appears in the human attention list. Nothing is
resent or undone automatically: a prompt already delivered cannot be recalled.

The fold itself enforces authority. It refuses human-only event kinds from non-humans (field, rank, label, relation, pin, reviewer, delete), flag changes except by people and the autopilot, workflow changes by non-humans, and agent journal or attachment events on Done or parked tasks. A task creation that is unreadable or newer than this build yields a read-only task instead of disappearing. A task is also read-only when one of its events sorts before its creation, or when the log holds a shared event newer than this build that shapes it (its project's workflow, a session rename). A person can clear the conflict flag.

### Replication

- Version vectors: "from origin X I have everything up to seq N". Every store
  holds a contiguous `seq` prefix of each origin. Events of every origin are
  replicated alike, including older origins of this machine. An event at a
  position a store already holds is skipped when it is the same event and
  refused when it is another one; refused events are reported in `status`.
- The existing swarm worker (`hgs-swarm.service`) runs an Overmind exchange
  with each peer in its loop, through a new internal command over the same SSH
  route. One call pushes and pulls, so it works when SSH goes in one direction
  only. Batches are at most 4 MiB.
- The nudge is the `rev` file: the worker compares `<state>/overmind/rev` on
  every pass (3 s) and exchanges at once when it changed, like a changed catalog
  digest. There is no extra socket or signal.
- An exchange continues while the peer still lacks events and ends when a round
  makes no progress or after 8 rounds (the worker continues on its next
  pass); events a peer did not accept are reported in `status`.
- A local write nudges the worker to exchange with reachable peers right away.
- Blobs are not part of the exchange. They are fetched by hash on demand, in
  ranges of at most 512 KiB verified by SHA-256, when a card is opened or a task
  is delivered to a session host. A peer accepts at most 8 pushed images per
  call, 10 MiB each and 20 MiB in total.
- Peers advertise `overmind:1` in swarm `hello`. Peers without it are skipped.
- Events carry `v`. A version that meets an event kind or payload version it
  does not know keeps the event, shows the task read-only, and refuses to write
  to it.
- Deletion is an event. Tasks and their events are not purged in stage 1; blobs
  of deleted tasks are removed when nothing references them.

### Compatibility

Every machine must fold the same events into the same state. Any change in how
an existing event kind folds requires a new event version; a machine that meets
a newer version keeps the event, shows the task read-only and refuses to write
to it. The fold never treats an origin as a machine: a session rename applies
only when its actor's machine is the renamed session's machine.

### Who writes what

Any machine writes task edits, transitions allowed to its actor and journal
entries. **Only the machine hosting a session writes the `claim` that hands a
task to that session**, together with `[sent]`. Two machines can therefore
collide only by claiming the same task at once. With manual sending that needs
two people pressing Send within the sync delay; stage 2 adds a guard for pool
claims by the autopilot.

## Part 3. Workflow engine

### Roles

The engine knows stage roles, not stage names: `backlog`, `ready`, `active`,
`review`, `needs_human`, `done`, `parked`. A workflow is a JSON document:

```json
{"version": 1,
 "stages": [{"name": "Backlog", "role": "backlog"}, "..."],
 "rules": [{"action": "advance", "from": ["Design"], "to": "Build",
            "actors": ["agent"], "requires": ["spec"], "own": true}],
 "evidence": "git_sha_or_text",
 "session_wip": 1}
```

Reviewer independence is a fixed invariant, not a setting. Rules may not name unknown actor kinds. `@resume` targets are allowed only on `answer` rules from `needs_human` stages. Stage names are 1–40 printable characters without leading or trailing spaces and do not start with `@`.

Validation: exactly one `done` stage, at least one stage for each of `backlog`,
`ready`, `active`, `review`, `needs_human`; every stage is reachable from a
task-creation target.
A change that removes a stage holding tasks must name a stage to move them to.

### Default workflow

Stages: Backlog (`backlog`), Queue (`ready`), Design (`active`), Build
(`active`), Review (`review`), Your Call (`needs_human`), Done (`done`), Icebox
(`parked`).

| From | To | Actor | Requires | Effect |
|---|---|---|---|---|
| — | Backlog | human; agent via `file` | agent: `breaks` (one sentence: what goes wrong if never done) | `[created]` or `[filed-by-agent]` |
| — | Queue | human | — | `[created]` |
| Backlog | Queue | human | — | triage |
| Queue | Backlog | human | — | — |
| Queue | Design | human (Send) on the session's host | target session; WIP and predecessor checks | `assignee`, `[claim]`, then `[sent]` |
| Design | Build | agent, own task | `spec` | `[spec]` |
| Build | Review | agent, own task | `worklog`, `evidence`; `root_cause` if labelled `bug`; predecessors at Review or later | `[worklog]` |
| Design, Build | Your Call | agent, own task | `question` | keeps `assignee`, `[needs-human]`, alert |
| Your Call | previous active stage | human (Answer) | `answer` | `[answer]`, the answer is sent to the assignee session |
| Backlog, Queue, Design, Build, Your Call | Backlog | agent, own task (`return`) | `reason` | unassign, label `blocked`, `[blocked]` |
| Design, Build | — | agent, own task (`decompose`) | ≥ 2 subtasks | children to Queue with `parent`; parent to Backlog as `epic` |
| Review | Review | reviewer (`review approve`) | `report` | label `reviewed`, `[review]` |
| Review | Build | reviewer (`review needs_work`) | `report` | label `review-failed`, `[review]`; the GUI offers "Send rework" |
| Review | Done | human | — | `[done]` |
| Design, Build | Queue | human (take back) | — | unassign, `[unclaim]` |
| any except Done | Icebox | human | — | — |
| Icebox | Backlog, Queue | human | — | — |
| Done | Queue | human (reopen) | — | clears verdict labels |

Rules carried over from vikunja-mcp:

- Done is reachable only by humans, enforced at one chokepoint for every write
  path, with a test over all commands and MCP tools.
- A task in Done or Icebox refuses every agent command.
- Re-entering an active stage clears `reviewed` and `review-failed`.
- An epic is never sent, claimed or reviewed.
- `follows` predecessors block claim and the move to Review until each is at
  Review or later; a human may override a claim with confirmation.
- Session WIP: by default a session holds one active task. Sending a second one
  needs confirmation in the GUI (`wip_exceeded`), which the human may override.
- Reviewer independence: `review-send` records the `reviewer` session, which
  must not be the assignee session. Only that session or a human may record
  the verdict.
- Evidence policy `git_sha_or_text`: when the session directory is a Git
  repository, `evidence` must be a commit that exists there (`git cat-file -e`
  on the session host); otherwise free text.
- Refusals name the field and how it arrived (`missing`, `empty`, `invalid`).

## Part 4. CLI, MCP and the agent channel

### CLI

`hgs [@host] overmind <command> [--json]`. Write commands take JSON on stdin,
carry a `request_id` and are idempotent through `receipts`, which are keyed by
the command and its request: the same `request_id` with another command or body
is refused. `@host` is for people: inside a session environment
`hgs @host overmind` is refused, and agents use `hgs overmind` on their own
machine. Commands are flat. Commands accept CRLF text and store it with LF. Malformed or oversized requests are refusals with code `invalid_request`.

Human and general commands: `board`, `show`, `create`, `edit`, `move`,
`reorder`, `pin`, `unpin`, `label`, `link`, `comment`, `send`, `review-send`,
`rework-send`, `answer`, `resolve-delivery`, `done`, `reopen`, `delete`,
`workflow get|set`, `templates get|set`, `status`, `attachment`, `rev`, `mcp`.
`send` and `review-send` take `session: {machine?, name}`; `machine` defaults to
the local computer. Peers advertise `overmind:1` in the `features` of swarm `hello`.

Agent commands: `mine`, `show`, `comment`, `advance`, `call-human`, `return`,
`decompose`, `file`, `attach`, `attachment`, `review`. `attachment` is open to
people and to the task's assignee or reviewer session.

Commands accept a task as a full `uuid`, a `ref` or any unambiguous `om-`
prefix. A `ref` can grow longer as tasks arrive, so an older, shorter `ref` that
became ambiguous returns `ambiguous` with the candidates instead of guessing.

`hgs overmind --help` inside a session with a valid agent identity lists only
the agent commands. An agent may act only on tasks assigned to its session; the
session a task was sent to for review may also comment on it, attach images to
it and record its verdict, and `file` records a new task.

Results are JSON. Agent-facing text is English. Responses to agents may carry a
`note` with the next expected step, because guidance at the moment of use works
better than rules loaded hours earlier.

### MCP

`hgs overmind mcp` is a stdio MCP server named `overmind` that calls the same
functions as the CLI: `my_tasks`, `get_task`, `comment`, `advance`,
`call_human`, `return_task`, `decompose`, `file_task`, `attach_file`,
`review_task`, `download_attachment` (returns a local path, not base64). It
inherits the agent's environment, so identity works the same way.

The server is attached when Zerus launches an agent, without editing the user's
global agent configuration: `--mcp-config` for Claude Code, `-c mcp_servers.*`
overrides for Codex; Kimi and DeepSeek are verified during implementation and
fall back to the CLI. A setting turns this off. Running sessions are never
restarted to gain MCP; templates name the CLI commands for them. Launch argument
changes need isolated real-agent contract coverage for the reviewed agent
versions, as for any native lifecycle change.

### Templates

Four templates render the text sent to a session: `dispatch`, `review`,
`rework`, `answer`. They live in settings (built-in → project in stage 1;
profiles and sessions in stage 2). Variables: `{task.ref}`, `{task.title}`,
`{task.prompt}`, `{task.stage}`, `{task.journal_tail}`, `{review.report}`,
`{answer.text}`, `{tools}` (MCP or CLI hint). Default `dispatch`:

```
[Overmind om-k3f9x] <title>

<prompt>

---
You own this task (stage: Design). Write a short spec and advance to Build;
when done, advance to Review with a worklog and evidence. Need a decision:
call_human. Tools: MCP "overmind" or `hgs overmind --help`.
```

Rendered text must satisfy `hgs send` rules (not starting with `/` or `!`,
at most 64 KiB). Attachments go as message attachments with `[Image #n]`
references.

## Part 5. Sending a task into a session

`send` always starts on the caller's machine: the desktop runs it locally, the
phone runs it through the connector on the gateway.

1. The caller does not check the gates against its replica when the session is
   on another machine; the host checks them (step 3). A refusal therefore costs
   an SSH round trip, and an offline host shows `host_unreachable`. For a local
   session the gates are checked in step 3 as well.
2. If the session lives on another machine, the bundle is an exchange. A store
   holds a contiguous `seq` prefix of every origin (Part 2), so it cannot take
   "all events of one task" alone. The caller first runs the normal exchange
   with that host, so the host holds everything the caller holds, then pushes
   the blobs of the task's attachments and runs the command there over the swarm
   SSH route. The cost is at most the 4 MiB batches the worker would send anyway.
   The forwarded command runs as a person on the host (`Actor::human(host)`), so
   the host writes the claim, as Part 2 requires.
3. On the session's host: gate check against the merged state, then the existing
   `checked_input` preflight (run and conversation identity, pause, blocking
   dialogs). A failure returns the reason; nothing is written.
4. Write `claim` with `base` and `request_id`.
5. Deliver with `input::submit`, using the same receipt and identity rules as
   `hgs send`. `recovery::cancel_for_input` applies as for any user input.
6. Write `[sent]` (receipt `submitted`), `[unclaim]` with the reason (no receipt:
   definitely not sent), or `[sent?]` plus `delivery_uncertain` (anything
   else). Overmind never resends. The human resolves an uncertain delivery
   with "It was delivered" (records `[sent]`) or "Not delivered", which undoes
   only what the uncertain delivery started, decided from the latest `[sent?]`
   entry: a send (the entry references its claim) takes the claim back and
   returns the task to Queue, a review clears the reviewer, and a rework or an
   answer only records a `[comment]`; the task keeps its session and an answer
   stays recorded.

Confirmations are refusal codes. `send`, `review-send`, `rework-send` and
`answer` take `confirm: [codes]`. A confirmable refusal has field `confirm` and
state `missing`: `agent_busy`, `wip_exceeded`, `predecessors_open`,
`pinned_elsewhere`. Other refusals are never confirmable.

DeepSeek native sessions are refused with `session_unavailable` in stage 1: their
input does not go through `input::submit`.

The input request ID is the Overmind request ID. A send interrupted after its
claim resumes that claim without new gates or session checks. `review-send`,
`rework-send` and `answer` skip gates and their intent write when an input
receipt already exists. A failed delivery is "not sent" only when no input
receipt exists; anything else is uncertain. An answer is recorded before it is
typed; one that was not delivered is sent again by repeating the request with
the same `request_id`, which the refusal names.

Sending while the agent works is allowed after confirmation; the native agent
receives it as a queued message. `review-send`, `rework-send` and `answer`
follow the same path with their templates. `hgs send`'s request schema does not
change.

Deferred: the `orphaned` flag for tasks whose assignee session was terminated or
archived (tracked in Beads), and the rename-ordering question `zerus-89h.2.4`.

## Part 6. Desktop GUI

### Tasks tab

- A main tab **Tasks** after Activity, Terminal and Processes.
- The tab belongs to a project, not to the session. A project selector inside
  the tab chooses it; the choice persists. Switching sessions never changes the
  board, its scroll position, filters or open card.
- On first use the tab opens the selected session's project.
- When the selected session belongs to the shown project, its tasks are
  highlighted and a strip shows "This session's queue: N · Send next". A session
  from another project highlights nothing.
- Columns follow the project workflow. Done and Icebox start collapsed. Your Call
  is a warning strip above the board.
- Cards show `ref`, title, labels, session tag and markers: ▶ working,
  ✓ reviewed, ✗ needs work, ⚠ conflict or unconfirmed delivery.
- Dragging is allowed only for transitions a human may make; a refused drop
  shows the gate's reason.

### Card panel

Opens inside the tab on the right. Editable prompt (the composer editor),
images, labels, priority, pinning and relations; the journal rendered as
Markdown. Actions: **Send to session ▾** (sessions of the project, with
idle/working hints), **Send for review ▾**, **Send rework**, **Answer** (Your
Call), **Move to…**, **Done**, **Delete** (with confirmation).

### Composer and elsewhere

- A split button **Add as task** next to Send, Alt+Enter. Default: Queue of this
  session, pinned to it. Arrow menu: project pool, Backlog. Success clears the
  composer and flashes the chip; failure keeps the draft.
- A composer toolbar chip **Tasks N** (this session's queue; Danger when a Your
  Call exists). Clicking it is the one action that switches the board: it opens
  the Tasks tab on that session's project with its tasks highlighted.
- Session rows show a queue badge.
- Your Call raises the usual attention notification.
- The inspector tab "Tasks & agents" (provider todo lists) becomes
  "Plan & agents".

### Data flow

The GUI reads the local replica with `hgs overmind board --json --project P
--since <cursor>` and watches the `rev` file, polling as a fallback. Writes run
locally; `send` routes as in Part 5. A session host without Overmind support
disables sending with "Update HGS on this machine".

## Part 7. Mobile client

Full parity with the desktop. The phone acts as `human` with its device label.

### Reading

- The gateway snapshot gains a small `mobile_overmind` digest: `rev`, per
  project counts by role, and `decisions` (at most 50 entries of `{task, project,
  title ≤120, signature}`) for Your Call. Only the gateway publishes it. The full
  board is never put into snapshots: they are resent every 5 s and share a 1 MiB
  catalog.
- A new relay operation **`overmind_read`** (a read, gateway-scoped, result at
  most 1 MiB, paginated) with actions `projects`, `board` (delta since a cursor),
  `task`, `attachment` (by SHA-256, ranges of at most 512 KiB, like
  `attachment_read`) and `sessions` (send targets with their state).
- The phone caches boards and cards in its encrypted private store with the
  existing cache bounds and purges them on disconnect.

### Writing

- A new relay operation **`overmind_write`** (a mutation, gateway-scoped, with a
  durable `request_id`) with the actions of every human desktop command: create,
  edit, move, reorder, pin, label, link, comment, send, review-send,
  rework-send, answer, resolve-delivery, done, reopen, delete.
- Task creation uses the request UUID as the task `uuid`, so a retry never
  duplicates a task. Writes made without a connection wait in the phone's
  encrypted outbox with an explicit retry, like drafts: the relay keeps queued
  requests only 120 s.
- `send` runs on the gateway and reaches other machines through Part 5's bundle
  delivery. The `claim` is still written by the session's host.

### Validation split

The relay validates the envelope of the two operations: the `action` allowlist,
sizes and identity. `hgs` validates fields and gates and remains the single
source of rules, so most new Overmind actions need no relay deployment. This is
a deliberate exception to the per-operation strict schemas used elsewhere.
Every new `hgs overmind` command used by the connector is added to the
`mobile_peers` argv allowlist and to the `--help` markers the connector probes.

### Your Call alerts

The relay derives a new alert kind `decision` from `mobile_overmind.decisions`,
with the slot `overmind:<task uuid>`, under the existing presence rules. It
resolves when the task leaves the list or its signature changes. Android gains a
Decision card kind and a preference toggle; tapping opens the task. Older apps
treat the unknown kind as a refresh.

### Android UI

- Projects → project → **Tasks**: stages as horizontally swiped columns, one per
  screen; drag inside a column (long press) to reorder; "Move to…" between
  stages.
- Full-screen task card with prompt, images, journal and all actions.
- **Add as task** in the session composer; **Send a task here** in the session
  long-press sheet; queue and Your Call badges on projects and sessions.

### Privacy

The relay operator can read task text in transit and in its bounded request
store, exactly as it can read session messages today. End-to-end encryption
remains a general mobile launch prerequisite, not part of this stage. Push
payloads never contain task content.

## Part 8. Errors and edge cases

| Situation | Behavior |
|---|---|
| Gate refusal | `{code, message, field, state}`; nothing written. |
| Session stopped, paused or showing a dialog | `send` refuses with the reason; the task stays in Queue. |
| Delivery failed without a receipt | `[unclaim]`, back to Queue, reason in the journal. |
| Delivery uncertain | Stays in Design with `delivery_uncertain`; human resolves. |
| Assignee session terminated or archived | `orphaned` flag; human returns the task to Queue. |
| Session renamed | `session_renamed` event inside the rename; pins and assignee follow. |
| Peer unreachable | Everything works locally; the GUI shows sync state per machine. |
| Cache corrupted or schema changed | Rebuilt from `events`. |
| Unknown event kind or version | Task read-only on that machine. |
| Concurrent conflicting transitions | `[rejected]` for the loser; `conflict` if an external effect is overturned. |

## Part 9. Compatibility and rollout

- New namespace `hgs overmind`; no change to `hgs send`, `HGS_*` variables or
  existing state paths. Old peers simply do not take part.
- New relay operations, feature flag `overmind`, snapshot field and alert kind
  are additive within API v1. Rollout order: hgs, desktop and connector; then
  Android; then the relay with all PostgreSQL workers replaced together. Relay
  memory qualification is re-run because new payload paths are added.
- The inspector tab rename is UI-only.
- `.beads/` is not touched by stage 1.

## Part 10. Testing

- **Rust unit tests**: convergence under any permutation of event delivery
  (property tests); every default transition and refusal; Done chokepoint over
  all commands and MCP tools; workflow validation; identity detection; `ref`
  prefix growth; template rendering and `hgs send` limits; English agent-facing
  text and ASCII markers.
- **Integration**: two isolated state directories as two machines with a local
  transport instead of SSH; bundle delivery; sending into a fake agent session
  on an isolated tmux socket (`tmux -S`, never the live server); receipts,
  uncertain delivery, rename, orphaning; MCP over stdio.
- **Relay**: envelope validation, idempotent writes, `decision` alerts and
  resolution, memory qualification.
- **Connector**: both operations, gateway scoping, routing `send` to peers.
- **Qt**: board per workflow, drag gating, card actions, Add as task and
  Alt+Enter, chip, sticky project selection, highlighting.
- **Android**: board paging, card actions, outbox and retry, Decision cards.

## Delivery order inside stage 1

Stage 1 is too large for one implementation plan. Each group below gets its own
plan and lands in order: steps 1–3 (backend), steps 4–5 (agents and desktop),
step 6 (mobile), step 7 with each group where it applies.

1. Event store, folding, workflow engine and CLI on one machine.
2. Replication between machines and bundle delivery.
3. Sending into sessions, agent identity and agent commands.
4. MCP server and launch-time attachment.
5. Desktop GUI.
6. Mobile: connector and relay operations, digest and alerts, Android UI.
7. Documentation in `docs/reference.md` and `docs/mobile-architecture.md`, packaging.

## Non-goals for stage 1

Automatic sending, service prompts, readiness detection, beads, the manager,
voice, iOS, end-to-end encryption, time-based scheduling, per-task worktrees
(Zerus never creates worktrees automatically), purging old events.

## Later stages: decisions already taken

### Stage 2 — Autopilot

- Per-session switch. Sessions take tasks pinned to them first, then pool tasks
  of their project whose requirement labels they satisfy.
- Each machine dispatches only into its own sessions. Pool claims get a guard:
  a claim is confirmed by an exchange round with the reachable peers before the
  prompt is sent.
- Profiles in the shared settings (built-in → profile → project → session):
  prompts for start of work, between tasks, status check (silence or background
  work longer than a threshold), done check (idle while the task is still
  active), plus the stage 1 templates; context policy `keep`/`compact`/`clear`,
  default `clear`; limits after which the task goes to Your Call.
- A readiness function decides when a session has really finished. It must
  learn from the open recovery bug `zerus-a1m2` (continuations sent while the
  agent works) and from background tasks, subagents and the turn Claude starts
  right after Stop.
- Distinguishable "no work" outcomes (empty, starving, cycle, WIP saturated) as
  in vikunja-mcp.
- An autopilot with an empty profile is the plain automatic queue.

### Stage 3 — Beads and similar trackers

- Adapter interface; beads first.
- Import by filter (all open, a label, or `bd ready`) as linked tasks; status
  and results flow both ways for linked tasks; a local task reaches beads only
  through an explicit "Publish to beads", because beads data may become public.

### Stage 4 — Manager

- Named managers with a configurable scope (projects or sessions), each a session
  with a chosen agent.
- The manager talks to the user, creates tasks from the user's words, keeps track
  of the autopilot, intervenes and dives into a task's context on request, and
  asks session agents or its own subagents for test environments and
  screenshots.
- It stays free: work goes to subagents or to the session whose task it concerns.
- Dispatch stays algorithmic (priority, order, status). The user may ask the
  manager to change the rules; the manager may propose reordering, for example
  when a task fits a session's current context, as an accept/reject proposal.
- Voice from the mobile app comes later; nothing exists for it yet, and phone-side
  speech recognition and synthesis over ordinary requests is the likely path.
- Open: how events reach the manager without keeping it busy, delegation tools,
  the proposal format.
