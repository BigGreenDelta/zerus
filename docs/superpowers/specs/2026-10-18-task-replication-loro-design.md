# Task replication on Loro

Status: draft, 2026-10-18. This spec covers how tasks are stored and
replicated across the user's computers and phone. The task model, the
interface and the agent commands are in
`docs/superpowers/specs/2026-10-13-project-task-queue-design.md`. Loro
(loro-dev/loro, MIT) becomes Zerus's replication foundation: tasks are its
first user, and the swarm catalog moves to it later.

## Goal

Every computer and the phone hold the tasks they need and work with them
offline. Changes made anywhere merge without a central server and without
asking the person to resolve conflicts. Upgrading Loro or changing the task
schema touches one place in the source tree.

## Decisions

| Topic | Decision |
|---|---|
| Rust | Zerus tracks the newest stable Rust (commit `1eedb997`: `rust-version` 1.99, CI 1.99.1) |
| Where Loro lives | One Rust crate in this repository is the only code that imports Loro |
| Replicas | Every computer holds every document; the phone holds the live tasks and the project settings |
| Documents | One document per task; one small document per project for its settings; no index document |
| Computers | Exchange through the existing swarm channel and `hgs-swarm` worker |
| Phone | Syncs with any computer through relay → Rust connector → hgs; the relay keeps an exchange two minutes at most |
| Done tasks | Compacted on acceptance; the phone lists them from a computer and fetches one when it is opened |
| Sends | Only the session's computer pastes; it never sends a task its copy shows In progress |
| Rollout | Tasks first; the swarm catalog moves to Loro later |

## Out of scope

- Moving the swarm catalog to Loro. It keeps its own CRDT until a separate plan
  migrates it with its existing data and mixed-version members.
- iOS here. The crate is built so that the same API can be generated for
  Swift; `2026-10-20-task-tracker-ios-design.md` does it (owner decision,
  2026-10-20).
- Automatic dispatch and any coordination that would prevent two computers
  from sending the same task before their copies meet; both sends run (see
  Sending).
- End-to-end encryption through the relay. The relay already sees session
  data; task updates follow the same trust model.

## The replica crate

`crates/replica`, package `zerus-replica`, is a library that hgs uses as a path
dependency and that the Android app links through UniFFI.

- **It is the only code that imports `loro`.** The version is pinned exactly
  (`loro = "=X.Y.Z"`). Raising it is a reviewed change to this crate alone;
  hgs, the connector and the apps get it together.
- **It owns the schema.** Document layout, field names, validation, the task
  rules of the task spec (stages, who may change what, the In progress check
  before a send), ID generation and the schema version all live here, once.
- **It exposes our operations, not Loro's.** Callers use typed operations:
  `create_task`, `edit_text`, `set_stage`, `set_target`, `move_before`,
  `record_send`, `record_report`, `record_cancel`, `comment`, `set_settings`;
  reads return plain structures. Sync uses `inventory`, `export_since`,
  `import`, `compact_done`. Nothing outside the crate sees a Loro type.
- **UniFFI bindings** are generated from the same API for Kotlin (and later
  Swift). The Android app never calls Loro directly.
- **Peer identity.** Every process that writes uses a fresh random Loro peer
  ID for its session, as Loro requires; two writers never share one.

## Documents

### Task document

One Loro document per task, identified by a UUID. It holds everything about
the task:

| Container | Contents |
|---|---|
| `meta` (map) | `schema`, `id` (the short ID), `project` (project ID), `created_at`, `author` (the person, or an agent with its session) |
| `fields` (map) | `stage`, `target` (`any` or `{machine, session}`), `rank`, `session` (last sent to), `summary` (latest report) |
| `text` (text) | The prompt |
| `log` (list) | History entries: comments, sends, reports, cancellations, stage and target changes, each with its actor and time |

The document grows only with the edits of its own task, so its size is bounded
by that task's life.

**Short ID.** A task is shown and addressed as `T-` plus six lowercase base-36
characters, chosen at random when the task is created, as Beads does; a local
collision is redrawn. The UUID is the document's identity; if two computers
ever create the same short ID before they meet, both tasks stay, and the
interface adds the first characters of the UUID to tell them apart.

**Order of To do.** Each task carries its own `rank`, a fractional key between
its neighbours (as Linear and Figma order lists). Moving a task rewrites one
field of one document. Concurrent moves of different tasks do not interfere;
concurrent moves of the same task keep the later one. Equal ranks fall back to
the document UUID.

**Concurrent edits.** Fields are last-writer-wins maps; the prompt text merges
character by character; the log only grows. Two conflicting stage changes made
at once (Accept on one computer, Back to To do on another) both appear in the
log, and the merged stage is Loro's winner. The crate never rewrites a merged
state behind the person's back.

### Project document

One small document per project, holding only its task settings: the task and
cancel message templates, the switch for agents acting on other sessions, and
the limit on tasks started by agents. It changes rarely.

### No index document

Which task documents exist is not replicated as a document. Each replica keeps
an inventory, document UUID → version vector, and replicas compare inventories
when they sync (see Synchronization). The inventory is sync bookkeeping, not a
log, so nothing in it grows without bound.

## Storage

On a computer, under the hgs state directory (`HGS_STATE_DIR`, by default
`$XDG_STATE_HOME/hgs/sessions`), so isolated test hosts stay isolated:

```
<state dir>/replica/<sha256(project ID)>/project-id
<state dir>/replica/<sha256(project ID)>/project.loro
<state dir>/replica/<sha256(project ID)>/tasks/<uuid>.loro
<state dir>/replica/<sha256(project ID)>/tasks/<uuid>.updates
```

The directory is named by a hash, since a catalog project ID may hold any
character; `project-id` names the project for people.

- `.loro` is a Loro snapshot; `.updates` collects updates appended since,
  each block with its length and a checksum, folded into the snapshot when it
  reaches a size bound. A block cut short by a crash ends the list, and the
  next save rewrites the snapshot without it. Writes are atomic and
  happen under a per-project lock, so the GUI's commands, agents' commands and
  the worker never interleave.
- **Done compaction.** When a task is accepted, its document is rewritten as
  one snapshot that keeps its full history (Loro's compressed snapshot). Later
  comments on it are appended and folded in as for any document. Back to To
  do reopens it in place.

On the phone, the same layout lives in the app's private files: the project
documents, the live tasks, and Done tasks it has opened, the latter as shallow
snapshots without their edit history.

## Synchronization

### Inventory and digest

A replica's project inventory lists, for every task document it holds, the
UUID, the version vector and the stage. A digest of the inventory lets two
replicas skip a project whose documents all match. When digests differ, the
replicas exchange inventories and, for each document whose versions differ,
the updates the other side lacks (`export_since` its version vector). A
document one side does not have is sent whole.

### Between computers

The `hgs-swarm` worker already exchanges the catalog over SSH with every
enrolled computer, in both directions over one configured connection. After
the catalog, it runs a replica exchange for every project: `hello` advertises
the replica capability and per-project digests, and differing projects
exchange inventories and updates in bounded pages. A local change triggers an
exchange within the same few seconds as a catalog change. A member whose hgs
does not advertise the capability is skipped and named in Settings as needing
an update, as for newer catalog kinds.

### Phone

The phone syncs with whichever computer its relay connection reaches: relay →
that computer's Rust connector → `hgs task exchange --json` with the payload on
stdin. The legacy Python connector does not offer it; a phone behind one works
as before, without tasks. The exchange is the mobile operation `task_exchange`:
additive to mobile API v1, scoped to a computer, gated by a capability that the
connector advertises only when its hgs supports it, and admitted on the mobile
peer allowlist. The relay and the connector keep it as a short read, two
minutes at most, never as a mutation: a merge is idempotent, so a repeated
exchange is safe and needs no durable request identity (owner decision,
2026-10-19).

- **What the phone holds.** The project document, every task that is not Done,
  and any task it already holds. It sends its inventory and the updates the
  computer lacks; the computer imports them and answers with the updates the
  phone lacks of those documents and of live tasks it does not have. Once it
  has nothing more for the phone, its answer names its own inventory of that
  view, which tells the phone what to send.
- **When it syncs.** Each computer's heartbeat carries, for every project, the
  digest of that phone view (`hgs task digests --json`, read with the catalog).
  While the app is open it exchanges a project whose digest differs from its
  own, and right after its own edits; nothing runs in the background (owner
  decision, 2026-10-19). Edits made offline wait on the phone until a computer
  is reachable.
- **Pages.** A request and an answer each stay within the relay's 1 MiB; Loro
  bytes travel as base64. An answer carries at most 512 KiB of updates, and a
  document whose updates are larger travels in parts the phone asks for one by
  one, each naming the whole update's SHA-256 and its offset; a part of an update
  that changed meanwhile starts it again. A request carries at most 256 updates.
  An inventory too large for one request goes in ranges of task UUIDs, each
  answered for its own tasks; the project document belongs to the first.
- **Storage.** The documents live in the app's `noBackupFilesDir`, which backups
  and device transfer never include, without further encryption (owner
  decision, 2026-10-19).
- **The phone's side.** The exchange itself lives in `zerus-replica`: the
  computer's answer, which hgs serves, and the phone's exchange, which walks
  the ranges, assembles parts and sends what the answer's inventory lacks; the
  app only carries its JSON through the relay. The phone keeps one replica per
  relay connection and swarm, so two relays never share documents, and drops it
  when the connection is removed. While the app is visible it checks every
  15 seconds, with the digests of the latest catalog or, when the catalog has
  not been read for 20 seconds, of its own read of the computers; it exchanges
  with every online computer of the project that advertises the exchange and
  names a digest other than its own, one at a time, and waits a minute before
  it tries a computer whose exchange failed again. An update of its own larger
  than 512 KiB, more than one request carries, is not sent; the exchange names
  it.
- **Send now and Cancel from the phone.** The phone first exchanges the
  project with the session's computer, so its own edits of the prompt and the
  session reach it, and refuses with the reason when that exchange fails; then
  it asks that computer to send (`task_send`) or cancel (`task_cancel`), and
  exchanges again to show the result (owner decision, 2026-10-20). Both are
  computer-scoped mutations of mobile API v1 with the relay's durable request
  identity: the connector runs each identity once (`hgs task mobile-send` and
  `mobile-cancel`), a refusal before anything happened is a failed receipt with
  hgs's reason, and an outcome that cannot be confirmed is uncertain. While the
  session's computer is out of reach, a Cancel goes to another online computer
  of the project, which cancels it there without the cancel message (see
  "Actions with side effects"); the cancel dialog says so. A Send now that may
  have happened keeps Send now off for that task until an exchange with its
  computer completes.
- **Done on the phone.** As on the desktop: the last 7 days, then "Show older
  (N)", in pages of 50 that the computer derives from its documents
  (`task_done`, a short read, `hgs task mobile-done`), newest first (owner
  decision, 2026-10-20). With Only this session the computer lists and counts
  only the tasks sent to that session. Opening a Done task fetches its document
  through the exchange, which then keeps the copy the phone holds.

### Done tasks

A computer derives the Done list of a project (short ID, the start of the
text, the session, the acceptance date) from its documents and serves it to
the phone on request; it is not replicated. The phone fetches a Done task's
document, as a shallow snapshot, when the person opens it, and keeps it
cached. A Done task reopened anywhere becomes a live task again and reaches
the phone through the normal exchange.

## Actions with side effects

Pasting a prompt or the cancel message into a session can happen only on the
computer that runs the session. So:

- **Send now, `hgs task send` and Cancel** run on the session's computer. The
  desktop calls that computer's hgs (`hgs @alias task send …` over SSH, as
  other remote session actions do); the phone goes through the relay to that
  computer's connector; an agent's `hgs task send` or `cancel` is forwarded the
  same way. If the computer cannot be reached, Send now and `hgs task send` are
  refused with the reason. A Cancel goes through all the same: the task moves to In review on
  this computer, its session gets no cancel message, and the session's
  computer receives the change when it syncs again.
- **That computer checks its own copy first.** If the task is already In
  progress there, it refuses: "T-a1b2c3 is already In progress in api". Two
  computers that send before their copies meet both send; each agent works and
  reports, as the task spec says.
- The send, the report and the cancel are recorded in the task document on the
  session's computer and replicate from there.

Everything else (creating, editing, moving, commenting, Accept, Back to To do,
settings) is a local edit on any replica, online or offline.

## Desktop and agents

- **Desktop.** Every computer holds every task document, so the GUI reads the
  local replica through hgs commands with JSON output, and refreshes when the
  project's local revision changes. Tasks of sessions on other computers are
  already local; only side-effect actions go to another computer (see above).
- **Agents.** `hgs task` in the agent's shell, on its computer, reads and edits
  the local replica; actions with side effects on another computer's session
  are forwarded as above.

## Android build

- The app's Gradle build compiles `zerus-replica` for arm64-v8a, armeabi-v7a
  and x86_64 with `cargo-ndk`, generates the Kotlin bindings with UniFFI, and
  packages the libraries.
- CI gets an Android job that builds the crate for those targets with the
  pinned Rust and runs the binding tests; the APK grows by the size of the
  library per ABI, measured and reported in the plan. The measured sizes are in
  docs/superpowers/plans/2026-10-19-task-replica-android-bindings.md.
- The app keeps its documents in private storage, never in shared storage or
  backups that leave the device unencrypted.

## Compatibility and upgrades

- **Golden vectors.** The repository keeps encoded snapshots and updates
  written by every released schema version and Loro version. Every build of
  the crate must decode and merge them; a Loro upgrade that cannot fails CI.
- **Schema version.** `meta.schema` names the layout. A reader ignores fields
  it does not know and Loro keeps them, so an older replica does not drop a
  newer one's data. A change that older readers would misinterpret raises the
  schema version; an older replica then shows such a task read-only with
  "Update Zerus to edit this task".
- **Mixed swarms.** Computers advertise the replica capability and the highest
  schema they read; exchanges with older members skip what they cannot read.
- **Mobile API v1** stays compatible: the new operations are additive and
  capability-gated, and the relay, connector and app can be deployed in any
  order.

## Limits

- A prompt is at most 64 KiB, the input limit of hgs; a comment at most 64 KiB.
- A task document that reaches 4 MiB accepts no further comments and says so;
  sends, reports and stage changes still work.
- Exchanges send at most 1 MiB per page, within the relay's request limits.

## Failure cases

| Case | Result |
|---|---|
| The session's computer is unreachable | Send now and `hgs task send` are refused with the reason; Cancel moves the task to In review here without the cancel message; local edits still work |
| Two computers send the same task before they meet | Both send; both reports are kept |
| A replica holds a schema newer than it reads | Those tasks are read-only there, with "Update Zerus" |
| A member without replica support | Skipped in exchanges and named in Settings |
| A document fails to decode | It is set aside, kept on disk, and reported; it never blocks other documents |
| The phone has no computer reachable | It works on its copy; edits wait |

## Testing

- Crate unit tests: every operation and rule; concurrent edits on forked
  documents (stage, rank, text, log) and their merges; short-ID collision
  display; Done compaction; schema-version handling; peer-ID freshness.
- Golden vectors decoded and merged on every build.
- Sync tests: inventory digests, paged exchanges, a missing document sent
  whole, a member without the capability skipped, the phone's exchange through
  a real connector and relay on isolated PostgreSQL.
- Side-effect routing: send and cancel on the session's computer, refused when
  it is unreachable, refused when its copy shows In progress.
- Android: binding tests on an emulator; offline edits merged after
  reconnection.

## Phases

1. **The crate and local tasks.** `zerus-replica` with the schema, the
   operations and golden vectors; hgs stores tasks in it and offers the
   `hgs task` commands on one computer.
2. **Computers.** Replica exchange in `hgs-swarm`; side-effect routing to the
   session's computer.
3. **Desktop.** The Tasks view over the local replica.
4. **Agents.** `hgs task` for agents: the proven session and its project,
   one-line answers and the instructions at session start.
5. **Android.** Rust in the Gradle build, UniFFI bindings, the phone replica,
   and the relay exchange.

Each phase gets its own implementation plan.

## Open questions

- None at present. Computers keep every Done task; bounded pages and Done
  compaction keep the lists and documents small (owner decision, 2026-10-20).
  Revisit when replicas grow large.
