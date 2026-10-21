# Retiring old Claude projections from indexed history

Beads: zerus-vxx3 (follow-up of zerus-e9qx). Status: design and source for
review; not compiled or tested.

## Problem

zerus-e9qx changed how a Claude transcript line becomes a public history row:
compaction summaries and Claude's own local command records are no longer
messages, and a prompt that is only pasted text loses Claude 2.1.296's blank
line framing. The history index is incremental. Rows it stored before that
change keep the old projection, so a conversation with an earlier compaction
still shows the summary and command output as sent messages, and a framed
phone message still appears beside its attachment receipt.

## Constraints

- No epoch reset. The phone keys read watermarks, cursors and cached pages to
  the epoch; a reset recounts and refetches every conversation.
- Incoming rows and their sequences never change; only outgoing provider rows
  can be affected.
- Decisions rest on native provenance, not on the stored public text.
- Bounded, resumable work inside the existing locked history transaction.
- Other agents and other streams are unchanged.

## Design

Every provider row records `source_offset` (`file:byte`), the start of the
native transcript line it was projected from, and `message_id`, Claude's line
`uuid`. A projection upgrade re-reads exactly those lines and applies the
current projection to them.

1. A new meta key `projection_version` (`"1"`) marks an index whose stored
   provider rows follow the current projection. A new index starts with it set,
   as `reconcile_version` does.
2. While it is unset, each `hgs history` call for a Claude conversation, after
   indexing its sources, scans outgoing rows whose body source is
   `claude_transcript` in `id` order from the cursor `projection_upgrade_after`.
   That includes provider rows and hook (journal) rows a provider row was merged
   into: the existing reconciliation keeps the hook row's stream but gives it the
   provider's body, uuid, offset and aliases. A call takes at most 64 rows and
   stops before another row once 4 MiB of transcript were read; each line is read
   whole, up to 16 MiB, so one call reads less than 20 MiB.
3. A row qualifies only when its own provider origin alias names that row. It
   then opens the indexed transcript file named by the offset, reads one line from
   that byte, and requires a complete JSON line whose `uuid` is exactly the row's
   `message_id` and whose `sessionId` is absent or exactly the conversation's ID
   (a present `null`, number, boolean, array, object, empty or other string does
   not qualify). Anything else leaves the row unchanged and counts it as
   unverified. A conversation whose transcript path is unknown is left for a
   later call.
4. The verified line goes through the current `search::provider_event`:
   - A public row whose digest differs: it is added through the ordinary `add`
     path, which updates the existing row in place through its alias, keeps its
     stream and every alias, recomputes `native_match_hash` and queues
     reconciliation. A framed paste therefore merges with its attachment receipt
     exactly as a new one does, also when it is a merged hook row.
   - Nothing: the row came only from the old projection. It is removed with its
     aliases and pending work when its only alias is its own provider origin.
     A row that has merged with any other evidence stays, counted as retained.
5. When a scan reaches the end, `projection_version` is set and the cursor
   removed. Progress appears as `source_status.projection_upgrade`
   (`indexing`/`complete`/`ledger_full`, `generation`, and the refreshed,
   retired, retained, declined and unverified counts); it does not change
   `complete`, because no incoming row or sequence is touched.

### Change ledger

Every row change the upgrade makes is recorded in the same transaction in
`projection_changes(generation INTEGER PRIMARY KEY AUTOINCREMENT, epoch, id,
change)`, `change` being `retired` or `refreshed`, so a cached phone window can
later learn exactly what changed (client design:
`2026-10-19-android-cached-history-retirement-design.md`). The upgrade must not
be deployed without it: changes made unrecorded cannot be reported later.

- A refresh is recorded only when `add` actually rewrote the row; a rewrite it
  declines past the index budget is counted as `declined` and changes nothing.
- Before changing a row the upgrade checks that the ledger holds fewer than
  100 000 entries and that SQLite's sequence is below 2^63-1. Otherwise it
  stops with state `ledger_full`, changes nothing and keeps its cursor.
- Entries of an earlier epoch are deleted before each upgrade call; the three
  full index resets (schema migration, source descriptor change, source
  truncation) delete all entries. Entries are not collected within an epoch.
- `source_status.projection_upgrade` reports `generation`, the newest entry of
  the epoch the response returns (0 when none). Reconciliation and incoming
  numbering run after the upgrade and can replace the epoch, so the generation
  is computed again, after other-epoch entries are deleted, once incoming
  numbers are assigned and before the transaction commits; the upgrade's other
  status fields describe that call's scan.

### Reading the ledger: `hgs history-projection`

`hgs [@host] history-projection SESSION --json` reads the ledger for a phone's
cached window (client design: wire grammar D4). Its request is
`{request_id, expected_run_id, expected_conversation_id, archive_id?,
agent_id?, history_epoch, projection_after, projection_until}`; unknown fields,
and generations that are not JSON integers, are refused. Identity, archive and
child admission is `history`'s own; the epoch must be non-empty, at most 128
characters and free of control characters, and `0 <= after <= until`.

- The read opens only an index that already exists: the private directory,
  lock file and database must be there, and the database must hold a schema-2
  index with its epoch, cursor key and ledger table. The lock file is opened
  without following a link or waiting on a FIFO and must be a private regular
  file before it is locked. It opens the database without the create flag and
  takes the same lock as ordinary history calls,
  but never creates, migrates, resets or repairs anything; a missing,
  uninitialized or other-schema index is refused unchanged ("not ready"), and
  only an ordinary history call builds or upgrades it.
- One `BEGIN IMMEDIATE` transaction holds admission, the generation, successor
  resolution and the encoded response, so they describe one snapshot. Any
  refusal rolls back: it changes nothing and produces no response to
  acknowledge.
- The read never indexes sources, reconciles or assigns incoming numbers; doing
  so here could merge before sources are complete. It resolves against the index
  that ordinary `history` calls settled and refuses ("still indexing") while
  `reconcile_work` holds anything or `reconciled` is not `1`. A later history
  call that reconciles changes what a later read serves.
- Admission checks, in order: the epoch is the index's current one; the index
  is settled; other-epoch entries are deleted; `until` is no later than the
  current epoch's newest generation.
- Entries `after < generation <= until`, at most 256, in generation order. An
  entry's successor is the row with its ID, else the row owning the alias
  `history:<id>` (merges repoint it to each survivor), else none. `retired`
  without a successor and `refreshed` with one are `resolved`; anything else is
  `unresolved` with `reason` `unexpected_successor` or `no_successor` and no
  successor.
- A successor is encoded by `public_row`, the code a history page uses for its
  rows: aliases as `original_ids` with the page's existing limit of 8 (inherited
  page behaviour, not a projection-specific cut), `history_id`, cursor, stream,
  incoming number, and no `native_match_hash`. Attachments, voice and receipt
  fields are never reduced.
- The complete encoded response, envelope included, is charged against
  880 KiB. Only the first entry may be reduced, as a page reduces its first
  row: its successor's `detail` is clipped to 500 characters with
  `detail_truncated`; if it still does not fit it is returned as `unresolved`
  with `reason: "oversize"`. A later entry that does not fit ends the response.
  `complete` is true only when every entry up to `until` is in the response;
  `next_projection_after` is then `until`, otherwise the last returned
  generation. No entry is dropped and an incomplete response is never empty.
- The help line is a new, distinct marker; connectors advertise the operation
  only once they implement it.

History cursors are positions (`at`, `ordinal`, `id`), so a removed row cannot
invalidate a cursor; canonical merges already remove rows within an epoch.

## Limits

- A phone keeps rows it already cached in the same epoch: Android merges pages
  into its window and drops a cached row only when a canonical row names it as
  an alias. Freshly loaded windows are clean. Removing stale cached rows needs a
  separate Android change (a fetched contiguous page is authoritative for the
  rows between its ends) and is not part of this design.
- A hook (journal) row that recorded a framed paste before zerus-e9qx and never
  merged with its provider row keeps that text: the journal stores the projected
  prompt, not Claude's wrapper. One that merged takes the corrected body.
- Unverified rows advance the scan; a completed scan does not retry them.
- A transcript line that cannot be verified keeps its old row.
