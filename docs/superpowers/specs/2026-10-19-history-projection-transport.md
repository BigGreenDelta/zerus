# History projection transport

This implementation connects the native ledger and existing-only projection reader
to API v1, then applies a bounded resolved prefix to Android and iOS display caches.
The cached-history retirement design at `f7453708` supplies the D4 contract. Native
backend component qualification does not substitute for transport/client checks.

## Admission and routing

`history_projection` is an additive read operation in both the relay operation
list and each capable selected machine's operation list. It has no new feature
bit. Each client requires both lists before requesting it. A retained
relay receives no projection request; a retained connector does not advertise the
operation. A new connector whose hgs help lacks the independent exact marker
`history-projection <session> --json` does not advertise it and refuses it before
an archive lookup or projection call. The ordinary history marker does not admit
projection, and the projection marker does not admit ordinary history.
On a due Rust help probe only projection is revoked before the call; failed or
non-text help leaves it unavailable while all existing legacy flags retain their
previous failure semantics. A successful help restores the own marker normally.

The native command is `hgs [@host] history-projection <session> --json` with the
unchanged scoped JSON payload on stdin. Both native peer paths admit only the
fixed `SESSION --json` argument shape and retain machine verification. The native
one-hop transport caps its capture at 1 MiB; the connector's same capture limit
also covers local helper calls. The connector makes one projection call with a
2-second deadline and 1 MiB output cap; it never runs the ordinary history head
indexing loop. Existing archive admission and lookup rules still apply when a
new hgs supports archives. Those checks can precede the single projection call.

## Request and response

Required payload fields are `request_id`, `expected_run_id`,
`expected_conversation_id`, `history_epoch`, `projection_after` and
`projection_until`. Only `archive_id` and `agent_id` are optional. UUIDs are
canonical and nonnil. Native identity bounds count UTF-8 bytes: run 128,
conversation 256, epoch 128 and child 160. Identities are nonempty and contain no
control characters; child identities contain neither slash. Positions are actual
JSON integer literals within `0 <= after <= until <= Int64.max`. Booleans,
floating-point/exponent numbers, numeric strings, unknown fields and optional
nulls are refused. The native reader remains authoritative for current identity,
epoch, ready existing index and `until <= current generation`. Native serde's
optional-null compatibility does not broaden the transport's omission-or-string
contract.

Each connector checks the entire response before returning it intact: exact
request/session/run/archive/child/parent/epoch echo, literal generation and next
positions, actual Boolean completion, at most 256 entries and at most 880 KiB of
compact ASCII JSON including all envelope and successor metadata. Returned
generations increase strictly above `after` and are at most the captured `until`;
gaps are allowed. Complete responses set next to `until`; incomplete responses
are nonempty and set next to their last returned generation, strictly before `until`. History IDs are
64-character lowercase hexadecimal strings. Each entry has exactly generation,
history ID, change, resolution and successor, with an optional unresolved reason:

- resolved retirement has a null successor and no reason;
- resolved refresh carries one canonical public history page row and no reason;
- unresolved retirement or refresh has a null successor and may carry
  `no_successor`, `unexpected_successor` or `oversize`.

Canonical successors retain aliases, opaque cursor, stream, incoming sequence,
receipt/media fields and additive public metadata. The connector checks their
public type, identity, child scope and displayable body without reconstructing,
clipping, dropping or inventing a row. The qualified native backend alone may
represent an oversized first successor as explicit unresolved/oversize. The
existing default-JSON 1 MiB completed-receipt limit is a second independent bound;
a result that fails either bound is a failed read with no successful prefix.

Completion describes the requested ledger range, including unresolved entries.
It grants no display acknowledgement. This transport does not apply a projection,
advance a client marker, mark messages read, reconcile native sources or change
voice, receipt, delivery or outbox state. Each client applies and persists
only a resolved prefix against the latest same-target, same-epoch cached window,
with the D4 snapshot, four-page, rebase and minimum-slice-marker rules.

## Phone display caches

Network pages with a canonically complete head carry the current projection
generation; incomplete or legacy pages carry zero. Each encrypted retained slice
stores its rows and applied generation in one atomic value. Merging slices takes
the minimum marker, so an older cached slice never inherits coverage it lacks.

A run captures the reported generation, requests at most four pages and validates
each complete response before applying any entry. The first unresolved entry stops
the prefix. Each prefix rebases onto the latest exact-target, same-epoch window;
an intervening older slice keeps its lower coverage. A refresh replaces only a
previously present old or successor row, through the ordinary pure display alias
merge. A retirement removes only an unprotected canonical outgoing row. New
ranges are never imported by this feed. Drafts, outbox, receipt UUIDs, voice state,
read marks, divider identity and delivery settlement are outside this path.
An entry covered by the latest window's marker is skipped even if it belongs to
an older in-flight run. Visible-row preparation rechecks the captured presentation
revision, outgoing messages and questions after background work before publishing.

The Windows gateway keeps this operation outside its advertised/dispatch allowlist
until a separate native Windows existing-index, private lock, epoch and projection
contract gate qualifies it. Ordinary Windows history remains available. A portable
policy control checks that even a new help marker cannot advertise the operation
and that refused requests retain their durable duplicate receipt without native calls.

## Retention and compatibility evidence

The new operation uses the shared read quota, read restart failure class and
one-hour terminal-read retention. The connector prunes only delivered terminal
projection receipts; undelivered and running rows remain. The relay uses a
separate additive partial index and bounded retention query. Existing `READ_SQL`,
`read_history_v3`, mutation capacity, mutation receipts and no-replay identity are
unchanged. The Python reference relay receives only the matching validator and
retention additions because it shares the operation tables; it remains retired.

The historical golden capture still uses its original help and 49-operation
request/envelope corpus. Its capability, heartbeat and unrelated rows remain
historical evidence. Only current operation/read sets and counts, read-class SQL
and the new literal refusal catalogue entry change. Dedicated fixtures under
`services/mobile/vectors/history_projection/` contain the new envelope, the
unchanged 28-request review corpus and 57 response grammar cases. Separate source
controls cover routing, one-call scope/budgets, exact wire and entry boundaries,
outer receipt refusal, read quotas, retention, restart and preserved mutation
identity. Scripted native and peer-wrapper controls are synthetic evidence, not
real hgs, SSH, provider, deployment or phone acceptance. Compiler and real CLI
qualification need separate reviewed source/artifact pins and execution grants.

Never deploy the projection upgrade alone: the first deployed upgrade must carry
the atomic ledger and the reviewed read/client feed together. A code rollback
must preserve the existing index, epoch, ledger and all owner state.
