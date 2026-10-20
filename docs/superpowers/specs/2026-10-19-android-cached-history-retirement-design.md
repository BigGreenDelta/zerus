# Retiring stale rows from a phone's cached history window

Beads: zerus-vxx3 and zerus-vxx3.1. This D4 contract is implemented by the
projection backend, transports and both phone display caches. Exact-source checks
and platform delivery are tracked separately; implementation does not establish
an owner installation or native UI acceptance.

## Problem

The projection upgrade (zerus-vxx3) corrects the computer's history index in the
same epoch: it removes rows the current projection no longer publishes and
refreshes rows whose projection changed, after which ordinary reconciliation can
merge a refreshed row with a hook row or a receipt. An Android window cached
before that keeps the old rows. `HistoryWindow.merge` keeps every cached row of
the epoch and drops one only when a merged canonical row names it as an alias
(`history:<id>` or an original ID; Android's canonical event ID is
`history:<history_id>`). A removed row has no such successor, and a refreshed row
is reloaded only if its range is requested again, which a window at its head
never does. Cached page slices (`HistoryPage.retained`/`neighbors`) can also bring
an old row back into a window later.

## Invariants

- No epoch reset, no app-data clearing, no inference from text or from a page
  that merely omits a row.
- No draft, outbox, receipt, attachment, voice, read mark, read dwell, delivery
  settlement, request UUID or send/replay authority is created or changed; only
  the derived display cache of canonical rows changes.
- Old phones, connectors, relays and `hgs` make no projection requests and
  change nothing.
- A change is acknowledged only after it is applied; nothing unresolved is ever
  acknowledged.

## Computer (`hgs`)

### Ledger

`projection_changes(generation INTEGER PRIMARY KEY AUTOINCREMENT, epoch TEXT NOT
NULL, id TEXT NOT NULL, change TEXT NOT NULL CHECK(change IN
('retired','refreshed')))` in the same index database.

- The projection upgrade appends one entry in the same transaction as the row
  change it describes: `retired` when it deletes a row, `refreshed` when `add`
  rewrites one. Recording ships in the same change as the upgrade; an index that
  completed the upgrade without the ledger cannot be reported later.
- Bound: the upgrade visits each outgoing row ID once per projection version and
  records at most one entry per visited row; rows that ingestion adds meanwhile
  are already projected currently and record nothing. Entries are therefore at
  most the index's row count at the scan (the existing 100 000 row quota), each
  under 256 bytes. As a hard guard, the upgrade refuses to change a row when the
  ledger already holds 100 000 entries or the next generation would exceed
  2^63-1: it stops with `projection_upgrade.state = "ledger_full"` and leaves the
  rows as they are. Nothing is ever changed unrecorded.
- No garbage collection within an epoch, so a same-epoch position never expires.
  Entries of another epoch are deleted before the upgrade or a projection read
  runs (`DELETE FROM projection_changes WHERE epoch<>?`), which keeps the
  invariant that the table holds only current-epoch entries; the generation
  primary key alone then serves keyset reads.
- Every epoch replacement is covered: the three full resets (schema migration in
  `index`, source descriptor change and source truncation or replacement in
  `sources`) add `DELETE FROM projection_changes` to their delete batches; the two
  epoch changes that keep rows (`incoming` historical backfill and the
  `merge_row` canonical-order reset) are covered by the deletion of other-epoch
  entries above. A client of an old epoch is refused by the epoch check before
  any entry is read.

### Successor resolution

For an entry's `id` the computer resolves the current canonical row from an
already settled index, exactly. The projection reader does not index or reconcile
sources; an absent, legacy or unsettled index is refused without migration:

1. a row whose ID is `id`;
2. otherwise the row owning the alias `history:<id>` (merges re-point this alias
   to every later survivor, so it names the final owner);
3. otherwise none.

`refreshed` requires a successor and `retired` requires none. Any other outcome
is returned as `unresolved`. A successor is serialized exactly as a history page
row (aliases as `original_ids`, `history_id`, `history_cursor`,
`history_stream`, no `native_match_hash`), through the same code, including that
code's only reduction: an oversize row's `detail` clipped to 500 characters with
`detail_truncated`. Identities, aliases, attachments, voice and receipt metadata
are never reduced.

### Entry grammar (review D4)

Each entry is exactly one of three disjoint shapes; the server emits only these
and the client accepts only these:

1. `resolution: "resolved", change: "retired", successor: null`: permits the
   guarded display retirement below.
2. `resolution: "resolved", change: "refreshed", successor: <page row>`: permits
   the guarded display replacement below.
3. `resolution: "unresolved"`, either change, `successor: null`, optional
   `reason` (`"no_successor"`, `"unexpected_successor"`, `"oversize"`): grants no
   mutation for that entry and stops the prefix there; resolved entries before it
   may still commit.

Any other combination (a resolved refreshed entry without a valid row, a resolved
retired entry with a row, an unresolved entry with a row, an unknown value) makes
the whole response malformed: it is discarded and nothing is acknowledged.

### Request and response

A separate command, `hgs history-projection SESSION --json`, with its own help
line (the exact marker the connectors probe) and a strict request:
`{request_id, expected_run_id, expected_conversation_id, archive_id?, agent_id?,
history_epoch, projection_after, projection_until}`. Identity, archive and child
checks are those of `history`. `history_epoch` must equal the current epoch;
`projection_after` and `projection_until` are JSON integers (no float, string or
bool), `0 <= after <= until <= current maximum generation`; anything else is
refused.

The response echoes the identity fields and `history_epoch`, and returns
`projection_changes`: entries with `after < generation <= until`, ascending, at
most 256, in the grammar above; `next_projection_after` is the last returned
generation, or `until` when `complete` is true (every entry up to `until`
returned).

Size: the server charges the complete response, envelope, identity echo, every
entry and its successor as encoded on the wire, against 880 KiB. It stops before
an entry that would not fit and returns `complete: false` with the entries so far.
When the first entry does not fit even alone, it returns that entry as
`unresolved` with `reason: "oversize"` and no successor, so the response always
makes a bounded statement about its first generation; it never returns
`complete: true` without every entry, never drops an entry, and never returns an
empty `complete: false` response.

Every `history` response adds `source_status.projection_upgrade.generation`, the
current maximum generation of this epoch (0 when none).

## Client algorithm

### Markers (review D3)

- `HistoryPage` and `HistoryWindow` carry `projectionApplied`: every ledger entry
  up to it is reflected in their rows.
- A network page gets the response's reported generation when the response is
  canonically complete (the change and its reconciliation ran before the page
  was read in the same call), and 0 otherwise.
- A cached slice stores its marker in the same `zerus_page_window` JSON as its
  rows (`projection_applied`, absent means 0); each slice is one atomic encrypted
  write, so rows and marker never separate in a crash.
- Merging any page into a window sets the window's marker to the minimum of both.
  An old slice therefore lowers the marker, and its rows are covered again by the
  next run (applying a change twice is idempotent). A marker is never copied onto
  rows that were not covered.

### Runs and progress (review D1)

- A run starts when a page for a cached window of the same target and epoch
  reports a generation above the window's marker, and both the relay and the
  selected machine advertise `history_projection`. It snapshots `until` = that
  generation, so entries appended later wait for the next run.
- Each response is validated in full before use: identity and epoch echo, entry
  count and byte bounds, strictly increasing generations above `after` and at
  most `until`, `next_projection_after` as defined, a non-empty entry list unless
  `complete`, 64-character lowercase hexadecimal IDs, and every entry in one of
  the three grammar shapes. A response that fails any check is discarded;
  nothing is acknowledged.
- Entries are applied in generation order up to the first `unresolved` entry. The
  applied prefix and the resulting rows are written together; the marker becomes
  the last applied generation (or `next_projection_after` when the page was
  complete and fully resolved). A later run resumes there without repeating
  applied entries. An unresolved entry and everything after it stay pending.
- At most four projection pages per run; a valid page with `complete: false` or a
  spent budget is resumable progress, not a failure. Errors, timeouts, a target
  or epoch change, or an incomplete response end the run without acknowledging.
- Commit goes through the existing object-revision `rebase`: the validated
  display delta (retire these IDs, replace these with their successors) is
  applied as a pure function to the latest window of the same target and epoch,
  never by writing back the window the run started from. The new marker is then
  computed from that latest window: when its marker is still at least the
  run's starting marker, every row in it was covered up to that point, so it
  becomes the larger of its marker and the run's last applied generation;
  otherwise an intervening merge brought in rows with less coverage, the delta is
  still applied and the marker keeps the latest window's lower value. A target or
  epoch change in between discards the delta. Entries already covered by the
  latest window's marker are skipped, so an older response cannot overwrite a
  newer ordinary page's corrections. Asynchronous display preparation also
  rechecks the current frame, presentation revision, outgoing messages and
  questions before publishing; new UI state is never overwritten by older work.

### Applying an entry (review D2)

- `retired`: remove the cached event `history:<id>` when it is a canonical
  outgoing row of this epoch with no request, receipt, outbox or voice identity.
- `refreshed`: the successor replaces the cached event `history:<id>` in place;
  when the successor's ID is already cached, that event is updated and the old
  one removed. The successor then goes through the existing
  `RollingConversationHistory` alias rules, so rows it absorbed (a receipt row,
  `history:<removed>`) are superseded as today, receipt and voice cards keep
  their bodies and original request IDs, and no UUID is synthesized or outbox
  entry settled. When the successor's `at` differs from the replaced row's, it
  moves to the position its `at` gives among cached rows.
- No anchored positional read is used: `around` cannot follow an alias (for an
  equal-time provider row merged into a hook row, the hook row sorts before the
  old cursor).
- An entry whose old ID and successor ID are both absent from a window changes
  nothing in it and still counts as applied; it never imports rows the window
  did not have.
- Display merges use the pure `RollingConversationHistory` alias merge only, not
  view-model paths with read side effects (`readWriter.supersede`). Projection
  reads never feed read dwell, watermarks, unread dividers or delivery
  settlement.

## Propagation (all additive)

- `hgs`: `src/cli.rs` help line `hgs [@host] history-projection <session> --json
  …`; dispatch beside `history` in `src/state/mod.rs`; command, ledger, recording and resolution in
  `src/state/conversation_history.rs` (`projection_dispatch`, the upgrade's
  record calls, the three reset batches, a row serializer shared with `page`).
- Rust connector: `capabilities.rs` (`IMPLEMENTED`, a `history_projection` flag
  from `has("history-projection <session> --json")`, its `remove` and read
  classification lists), `validate.rs` (`validate_history_projection` with
  integer literals and bounded strings), `execute.rs` (read-only call to the new
  command, same timeout class as `history`).
- Python connector: `context.py` `OPERATIONS` and `READ_OPERATIONS`;
  `connector.py` `history_projection_supported` from the same help marker,
  admission, archive/child routing like `history`, native call.
- Relay: `protocol.rs` `READS`, the operation list, the allowed-field table and a
  `history_projection` validator.
- Android: `RelayApi.kt` (operation request; response parser; `HistoryPage.parse`
  reads the generation), `ConversationPaging.kt` (`projectionApplied` on
  `HistoryPage`/`HistoryWindow`, slice stamping in `slice`, `cached` parsing,
  minimum on `merge`, `retire`/`replaceWithSuccessor`), `HistoryReadPaths.kt`
  (cached marker), `ZerusViewModel.kt` (run trigger after page merges, capability
  check beside the existing `history` one, commit with `rebase`).
- iOS: `HistoryProjection`, `HistoryPage`, `HistoryWindow` and `CachedHistoryPage`
  in ZerusKit, plus `ConversationModel+Projection` and the ordinary history-page
  publication hooks, implement the same prefix, coverage and rebase contract.

Version loss is handled by capability: when the selected machine or the relay
stops advertising `history_projection`, runs stop and the marker stays as it is.
Requests reuse the existing read request identities and receipts; they are
read-only and never retried as mutations.

## Finite controls

Computer: one entry per actual retired and refreshed row change in the same
transaction, and none when `add` declined the rewrite;
`ledger_full` and generation exhaustion stop without changing rows; each of the
five epoch paths leaves no other-epoch entry served; successor resolution for an
unchanged ID, a `history:` alias chain after two merges, an equal-time provider
row merged into its hook row, and a receipt-owned survivor; retired entries
without successor; unresolved outcomes; request validation for float, string,
bool, negative, out-of-range, `after > until`, future `until` and wrong epoch;
page and byte caps with `complete` semantics.

Client: 33 refreshed entries in one page applied in one run and committed as one
prefix; a resolved retirement followed by an unresolved refreshed entry, and one
followed by an unresolved retired entry (both commit only the first); each
malformed resolved shape (refreshed without a valid row, retired with a row) and
an unresolved entry with a row, each discarding the response without
acknowledgement; a single successor over the byte cap (explicit oversize
unresolved) and an envelope-overhead boundary case; an intervening slice merge
and an intervening head page during a run, committed onto the latest window; more than four pages progressing over runs without repeats; an
unresolved entry stopping the prefix while earlier ones stay committed; an empty
final tail; entries appended after the snapshot left for the next run; an old
neighbor slice reopened into a window at a higher marker (row not resurrected); a
crash between two slice writes; equal-time ordering and alias supersession of an
absorbed receipt; malformed, wrong-target, wrong-epoch and truncated responses
retained without acknowledgement; old connector, relay or `hgs` making no
requests; and unchanged drafts, outbox, attachments, receipt UUIDs, voice states,
read marks and unread dividers. Original versus fixed controls on the same final
tests.
