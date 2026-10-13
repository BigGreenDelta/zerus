# Rust relay runtime

`zerus-relay` is the standalone server for Zerus mobile API v1. It runs on Rust
1.85 or newer, Tokio, Axum/Hyper and SQLx. PostgreSQL serves multiple workers;
SQLite is an exclusive local backend. The production image contains the Rust
binary and its system libraries, without Python. The Python outbound connector
and installed Android clients use their existing protocol and credentials.

## Build and verify

From this directory:

```sh
cargo +1.85.0 build --release --locked
cargo +1.85.0 fmt --check
cargo +1.85.0 clippy --locked --all-targets -- -D warnings
cargo +1.85.0 test --locked
```

The PostgreSQL contract is explicit, never silently treated as passed by a local
SQLite run. Point `ZERUS_RELAY_TEST_DATABASE_URL` at a **disposable** PostgreSQL
instance whose fixture role may create databases. The test creates and removes
a uniquely named database; credentials are not logged:

```sh
cargo +1.85.0 test --locked --test contracts postgres_cross_worker -- --ignored
```

From the repository root, with the `services/mobile` test dependencies installed:

```sh
export ZERUS_RELAY_BINARY="$PWD/services/mobile/relay/target/debug/zerus-relay"
export PATH="$PWD/services/mobile/relay/target/debug:$PATH"
python -m unittest discover -s services/mobile/tests -p test_rust_relay.py -v
python -m unittest discover -s services/mobile/tests -p 'test*end_to_end.py' -v
python -m unittest discover -s services/mobile/tests -p test_peer_migration.py -v
```

These replay the released Python HTTP assertions against a Rust process and
exercise the actual Python connector with synthetic native subprocesses. They
include persisted Python receipts, exact canonical hashes, Unicode, attachments,
one-hop routing and cancellation. The full reference/connector suite remains
`python -m unittest discover -s services/mobile/tests -v`. The reference Python
HTTP module is retained as an independent compatibility oracle, not selected by
the supported server CLI. `zerus-mobile` delegates administration/serving to
`zerus-relay`; the offline `migrate-sqlite` utility remains Python.

## Boundaries

The additive `voice_profiles`, `voice_submit`, `voice_status` and `voice_cancel`
operations target an exact conversation computer and session. Its native worker
owns retained audio, recognition and delivery; the relay acknowledges acceptance
and immediately clears completed upload bodies, retaining only the request
identity receipt. Voice history and playback are shared by authorized devices
through existing conversation and `attachment_read` access, independent of which
device submitted the voice message. See [the contract](../../../docs/voice-messages.md).

- `http.rs` owns authentication before ingestion, separate anonymous,
  authenticated and long-poll admission, trusted proxy parsing, request/body
  deadlines, disconnect cleanup and bounded response spools.
- `protocol.rs` validates exact operation envelopes and native identities.
  Attachment validation decodes into one reusable 48 KiB buffer. The relay never
  executes native commands or follows paths supplied by a phone.
  `attachment_read` is an additive read: exact `expected_run_id` and nonempty
  `expected_conversation_id`, canonical receipt `message_id`, `index` 0–7,
  `offset` below 10 MiB and `length` 1–524,288 (`offset + length` ≤ 10 MiB),
  optional nonzero `archive_id` and child `agent_id`; any other field is
  rejected. Its base64 result fits the ordinary 1 MiB result bound. It is a read
  for quotas, outside the persisted `READ_SQL`/`read_history_v3` text, with its
  own retention: the partial `attachment_history` index lets maintenance delete
  a completed row 5 seconds after the phone has read its terminal receipt
  (`result_read`) or 60 seconds after completion, so file bytes are carried,
  not kept. Submission requires the computer to advertise the operation.
  `overmind_read` and `overmind_write` are gateway-scoped (`GATEWAY`): the
  session is empty and the payload is exactly `{action, request, device?}`
  for a read or `{request_id, action, request, device?}` for a write, whose
  `request_id` repeats the envelope's (it is the Overmind request identity and
  the created task's UUID). Read actions are `projects`, `board`, `task`,
  `decisions` and `attachment`; write actions are `create`, `edit`, `move`,
  `reorder`, `pin`, `unpin`, `label`, `link`, `comment`, `send`,
  `review-send`, `rework-send`, `answer`, `resolve-delivery`, `done`, `reopen`
  and `delete`. `request` is an object whose fields and gates `hgs overmind`
  on the gateway validates, so new request fields need no relay deployment;
  an `attachment` read's `offset` and `length` follow the `attachment_read`
  bounds. `device`, an informational label, follows `hgs overmind --device`:
  1–64 characters, no control character (C0, DEL or C1), not starting with
  `-` or `@`. A canonical read payload is at most 64 KiB and a write
  payload at most the 29 MiB request maximum (inline images), else 413; other
  envelope violations are 400. Submission requires the gateway snapshot to
  advertise the operation and an available local route of the named computer:
  a computer reachable only through a peer route is refused with 409, so a
  gateway operation never runs on another machine's connector, and its claim
  carries no peer route. `overmind_read` is a read with the attachment
  retention class and its own partial `overmind_history` index;
  `overmind_write` is a mutation with a permanent identity tombstone, so a
  retry returns the first receipt instead of applying twice.
- `json.rs` bounds depth and lexical structure before object materialization.
  Canonical ASCII escaping, key ordering, arbitrary integer precision and Python
  float formatting preserve API v1 request identities. A deterministic 20,000
  float differential test compares the independent Python encoder. Containers
  are decoded from borrowed raw JSON so the legal object key
  `$serde_json::private::Number` cannot be interpreted as serializer metadata.
  Legacy Python receipt tests cover this key as well as Unicode and large numbers.
- `store/requests.rs` owns queue admission, result reservations, committed claims,
  receipts, expiry and permanent mutation identity tombstones. A prepared
  submission owns one canonical payload plus small typed metadata; SQL work does
  not retain its decoded attachment tree. Claim payloads leave storage only
  after commit. No error path changes `claimed` back to `queued`.
- `db.rs` supplies bounded pool acquisition and one transaction implementation
  for both SQL dialects. PostgreSQL workers share the existing schema and quota
  allocation. PostgreSQL network buffers are shrunk before connections return to
  the pool, preventing the largest transferred payload from remaining allocated
  on every pooled socket. SQLite uses a private file, an exclusive process lock, one
  connection and SQLx's database worker thread. Dropping a transaction rolls it
  back; cancelling a response never retries an external operation.
- `registry.rs` binds physical computers, aliases and immutable one-hop routes.
  Catalog size is checked before fetching selected payloads, then fetched in a
  batch. A withdrawn or revoked route fails queued requests and makes claimed
  requests uncertain. It never reroutes a claimed request. A peer the gateway
  manifest marks offline stays listed on its computer with `online: false` and
  no snapshot; it is never selected, claimed or updated by a peer heartbeat, and
  needs a fresh peer heartbeat after it is marked online again.
- `store/snapshots.rs` separates liveness and event-free snapshot changes from
  shared payload accounting. Event publication and wake jobs have transactional
  caps. Lock order is global event accounting, payload shard when needed,
  workspace, credential, then route/request; multi-workspace locks are sorted.
  Heartbeats may carry `presence` beside the snapshot. The relay remembers each
  machine's latest presence with its receipt time without rewriting an
  unchanged snapshot; a heartbeat without presence keeps the remembered value.
  The gateway snapshot's `preferences.mobile_delivery` is stored with it.
- `alerts.rs` is the pure presence rule: preference parsing, owner away time
  over every remembered presence, and candidate resolution from
  `attention_signature`, `reply_id`, shared read marks and native need. A
  changed signature drops an attention or error candidate only once the native
  need is gone; a read mark of the captured or current signature drops it.
  Missing or malformed information means away. `decision` candidates come from
  the gateway's own snapshot only, from `mobile_overmind.decisions` (Your
  Call): of its first 50 entries, those with a canonical UUID `task` and a
  1–128 character `signature` without control characters, first entry per
  task. A new or re-signed entry is an ordinary `decision` event and a
  candidate in the slot `overmind:<task>`, which captures the signature;
  leaving the list or a changed signature resolves it. Only the gateway's own
  snapshot (its local route) raises or resolves decisions: another gateway's
  peer heartbeat for the same computer never does, and delivery reads the
  local route's snapshot for them. A snapshot without the digest is missing
  data: it raises nothing against a digest, and keeps held decisions, unless
  its valid capabilities (protocol version 1 with an `operations` array) no
  longer list `overmind_read`, which drops them. This relies on the connector
  contract: the gateway connector carries its latest digest in every
  snapshot; it omits the digest only while none is known and never
  substitutes an empty list.
- `store/candidates.rs` holds at most one alert candidate per computer, session
  and kind (1,000 per workspace, 24-hour expiry). Ordinary events refresh phone
  state only. Changed snapshots and new presence evaluate candidates on the
  worker that received the heartbeat. An elected maintenance pass evaluates the
  due candidates of at most 32 workspaces, most overdue first, and drops the
  oldest expired candidates of any workspace. Waiting candidates are rechecked
  within 15 seconds while at most 32 × 15 / `maintenance_interval` workspaces
  wait (480 at the 1-second default); beyond that, rechecks slow down
  proportionally, still most overdue first. Plans come from unlocked reads, so
  several workers may evaluate the same candidate. Deleting the candidate row
  in the transaction that appends the `alert` event and coalesces device wakes
  is the cross-worker single-delivery claim. Candidate rows are locked last,
  after any event, credential and route/node locks, in ascending id;
  maintenance visits workspaces in sorted order and folds expired rows into
  that order.
- `store/background.rs` owns bounded indexed maintenance batches and durable
  poll/push leases. PostgreSQL advisory locks elect maintenance transactions;
  PostgreSQL startup never retires another worker's claims. A failing alert
  pass at startup is logged and left to the background loop. A wake job that
  receives a newer alert while leased is sent again instead of deleted.
- `push.rs` sends generic wake hints through FCM HTTP v1. Provider requests run
  outside transactions and never target client-supplied URLs. FCM uses
  short-lived service-account OAuth tokens and an RS256 signature from `ring`;
  tokens and provider errors are never logged. Registrations from the removed
  UnifiedPush provider are deleted on their next delivery attempt.

Idle polls hold no query-pool connection. Each PostgreSQL worker has one dedicated
LISTEN connection and a bounded local broadcast channel; notifications contain
IDs only. Polls subscribe before querying, recheck authorization on each wake and
fall back to durable reads every five seconds. Listener reconnect wakes waiters.
A notification is a hint, never proof of delivery.

## Resource ownership

Defaults retain the existing 29 MiB inline-request and 1 MiB ordinary-response
limits, one large payload lane, 8 MiB of aggregate small request bodies and eight
small response slots. Parsing/encoding work runs on a bounded blocking executor;
its ownership guard retains admission even if the HTTP future is cancelled.
The canonical encoder preallocates its exact output size rather than doubling
large buffers for final delimiters. Responses use immediately unlinked files
with 64 MiB total reservation per worker and a 32 MiB maximum claim reservation.
Keep `TMPDIR` on a private disk-backed spool, as in Compose. The Debian image
also fixes glibc's arena count and mmap/trim thresholds (`MALLOC_ARENA_MAX=2`,
`MALLOC_MMAP_THRESHOLD_=131072`, `MALLOC_TRIM_THRESHOLD_=131072`): deallocated
maximum payloads must return to the OS instead of accumulating in allocator
arenas between tasks. Apply the same environment before starting a glibc binary
outside Docker when reproducing its memory measurements.

Long polls have separate local and durable quotas (256 per worker, two per
credential, 32 per workspace). Cancellation transfers the local permit to a
tracked lease-cleanup task until SQL cleanup finishes. Failed cleanup also has a
durable expiry. Ordinary requests cannot consume that reserved cleanup capacity.

The server bounds accepted TCP connections (default 1024), headers (100 and a
32 KiB buffer), header ingestion (10 seconds) and connection lifetime (120 seconds).
Connections closing at their lifetime ceiling are normal: clients retry reads,
while mutation receipt recovery uses the original UUID. There are two async
runtime threads per process. Worker overload returns 429 and `Retry-After: 1`.
Health is independent of SQL/admission; readiness coalesces a two-second probe
and caches its result for one second. Errors omit SQL, bodies, tokens and URLs.

SIGINT/SIGTERM stops accepting connections, wakes idle polls, drains tracked
work and closes pools. Final database cleanup has a five-second deadline so an
unresponsive database cannot prevent process exit; durable leases then expire.
Compose allows 45 seconds. Command claims committed before
a shutdown retain their normal deadlines and can become uncertain; restarting a
worker never makes those commands executable again.

## Upgrade and operation

The listener, API, private JSON configuration, database tables, IDs, token hashes,
pairing invitations, event cursors and connector journal bindings are preserved.
The default state path remains `~/.local/share/zerus-mobile/relay.sqlite3`.
PostgreSQL workers must use identical pool/quota settings, including allocation
shard count; mismatched allocation is refused. Local administration stays CLI
only. The image also supplies `zerus-mobile` as a binary alias.

An existing PostgreSQL installation needs a runtime replacement, not a data
export or identity reprovisioning. Before an owner-authorized cutover, verify a
backup restore, drain ingress and stop only the old relay workers. Start the Rust
workers against the same authoritative database and existing configuration;
verify readiness, a synthetic request/receipt and revocation. Keep native agents,
connectors, tmux and DeepSeek hosts running. A Rust-to-Python rollback must use the
same current database, never a snapshot predating newly accepted receipts.

Presence alerts add the `alert_candidates` table and nullable columns
(`nodes.presence_at`, `presence_until`, `mobile_delivery`,
`computer_routes.presence_at`, `presence_until`, `events.alert_kind`); the
schema version stays 1 and the Python reference ignores them. Deploy the relay
after `hgs`, the desktop, the connector and Android. Replace all PostgreSQL
workers together: an older worker would wake phones for every event, reject the
`presence` field and serve `alert` rows without `alert_kind`. The offline
SQLite-to-PostgreSQL migration does not carry candidates or `alert_kind`;
phones ignore such migrated alert rows.

Attachment reads add only the `attachment_history` partial index (schema version
1; the Python reference creates it too). Its first `CREATE INDEX` scans the
requests table once, briefly blocking writes on PostgreSQL. Phones use the
operation only when both relay capabilities and the computer snapshot list it,
so the relay, connector and app can be deployed in any order; replace all
PostgreSQL workers together so every worker accepts and expires the operation.

Overmind operations add only the `overmind_history` partial index (schema
version 1; the Python reference creates it too), with the same one-time scan
as `attachment_history`. On a large PostgreSQL deployment, pre-create it with
`CREATE INDEX CONCURRENTLY IF NOT EXISTS overmind_history ON requests(updated,id)
WHERE operation='overmind_read' AND state IN ('completed','failed','uncertain')`
before the cutover, so worker startup does not block writes; confirm
`pg_index.indisvalid`, since startup keeps an invalid index of that name.
Deploy `hgs` and the connector first, then Android, then the relay, replacing
all PostgreSQL workers together: an older worker refuses the operations as
invalid and derives no `decision` alerts. There is no separate feature flag.
Phones use the operations only when both relay capabilities and the gateway
snapshot list them, so an older relay or connector hides Tasks on the phone.
The connector lists them only on the gateway, while its `hgs --help` carries
the mobile Overmind ABI line. It always pages task and board reads, so the
gateway's `hgs` must accept the paging fields; an older `hgs` refuses them, and
on the gateway or the session's machine it refuses an answer's `repeat`.
Android builds that predate the `decision` kind treat its alert rows and
ordinary events as plain refreshes. Snapshots are stored verbatim, so an
upgraded relay compares against the digest stored before it; tasks already
waiting in Your Call before either upgrade raise nothing, only later entries
and signature changes do. Before the deployment, run the PostgreSQL contract
test above and the `tests/load.py` memory qualification below for the new
write path (Beads `zerus-89h.4.1`).

Stop the sole SQLite server before local administration or a runtime replacement;
Rust refuses a second owner of that file. Never run Python and Rust SQLite
writers simultaneously. SQLite-to-PostgreSQL is a distinct offline migration,
using the preserved utility and verification procedure in the parent README.
The Python historical service remains available to tests through `create_app`,
but normal CLI/server packaging always selects Rust.

Production deployment requires separate owner authorization. Repository tests
and a built image do not establish a live cutover. FCM delivery to a real device
requires operator credentials and a device; local tests do not claim that result.

The owner-authorized production rollout on 2026-10-10 deployed source
`770f00c7557527fcda0f641034c544e5ee7c82d5` to both PostgreSQL workers.
Pre-cutover and post-deployment backups restored successfully into disposable
databases. Quiescent Rust initialization preserved all 21 application tables;
credential hashes, permanent mutation receipts and private configuration remained
unchanged. Public pairing, cross-worker single claim, duplicate/conflicting
submissions, result recovery and revocation passed with synthetic identities.
The active connector resumed heartbeats, and FCM configuration loaded successfully.
PostgreSQL and unrelated services retained their container IDs and start times.

[Hosted qualification](https://github.com/ufna/zerus/actions/runs/38034012114)
passed the full Linux, Arch and macOS suite, including Rust relay contracts,
released HTTP/connector compatibility, PostgreSQL recovery and repeated 256 MiB
container workloads. An unchanged GUI clear-context fixture passed on rerun;
its timing instability is tracked separately in `zerus-wvl`.

## Speech transport

The optional speech host adds capability-gated `speech_profiles`, `speech_stats`,
`speech_submit`, `speech_status` and `speech_cancel`, all scoped to a computer
with an empty session. Both the relay and the selected computer must advertise
the operation. The connector returns a job receipt promptly; inference runs in
the separate host service and never occupies its native command queue.

Submit accepts at most 120 seconds of mono 16 kHz PCM16LE, inside a bounded
6 MiB JSON envelope. Its request UUID also identifies the host job. Status and
cancel require the original submit receipt, bound to the same device, workspace,
computer and native machine identity. A guessed job ID or a different phone
cannot retrieve its text. Submit and cancel receipts omit transcripts; only
status returns text. Uncertain jobs are recovered by status, never by uploading
audio again.

The relay removes submitted audio on the connector result acknowledgement and
scrubs terminal expired/revoked submissions during maintenance. It preserves
the fingerprint and mutation identity. Speech reads expire after 120 seconds,
independently of the existing persisted read-history predicate. Two additive
partial indexes support bounded cleanup; schema version remains 1. Replace
both PostgreSQL workers together so either accepts the new operations. Existing
phones and connectors remain compatible, and discovery does not load models.
The first index creation scans request metadata; use the normal backup, drain
and code-only update procedure above.

Host installation and adapters are documented in
[services/speech](../../speech/README.md). Local content-free counters and the
planned fleet analytics architecture are described in
[analytics](../../../docs/analytics.md). API v1 remains trusted TLS transport;
the relay can see audio and status responses in transit.

## Speech provider management

The additive computer-scoped operations `speech_config`, `speech_profile_upsert`,
`speech_profile_remove`, `speech_profile_test` and `speech_config_status` manage
profiles independently of conversation selection. They require both relay and
target capabilities; the connector probes the distinct native help marker
`Speech profile management v1`. Exact machine identity and configuration revision
are checked by the host. Status is additionally bound to the original device,
workspace and physical computer route.

Mobile replacement credentials must use the host's sealed P256/HKDF/AES-GCM
envelope; plaintext replacement keys are rejected before persistence. The
connector validates redacted responses and removes CLI/SSH stderr from management
errors. Completed mutation bodies are erased while their hashes and receipts
prevent replay; management reads expire after 120 seconds. Two additive indexes
cover management reads and body cleanup on both storage backends. Update all
PostgreSQL workers together through the existing backup/drain procedure.

The complete [profile and credential contract](../../../docs/speech-providers.md)
also defines explicit synthetic tests and optional voice/ASR `profile_revision`
pinning. Qualification runs `test_mobile_speech_config.py` against both released
HTTP implementations, plus local Rust and isolated PostgreSQL cross-worker
contracts. It covers real connector/CLI configuration without an inference
runtime, malformed and plaintext keys, identity conflicts, response redaction,
status ownership, short retention and exact payload accounting.

## Capacity evidence

Run `tests/load.py` through the repository's Python test environment after building
`services/mobile/Dockerfile`. It creates only disposable containers and volumes,
with a 256 MiB memory/swap ceiling, no capabilities, a read-only image and a
private disk-backed spool. Repeated maximum attachments and late syntax failures
exercise warm allocations, committed claims, results and cleanup. Inspect both
RSS and cgroup memory, since SQLite page cache and spool writes also count.

Use `--backend postgres` to create a private disposable PostgreSQL 17.11 container
and exercise the production SQL path. The harness suspends that database,
checks independent health and failed readiness, resumes it and verifies recovery
and delivery of the queued commands. It also checks graceful process exit while
the database is suspended again. PostgreSQL is measured separately from the
relay worker's cgroup. The SQL contract also terminates only its own LISTEN
sockets and checks reconnect wakeups and durable visibility across workers.

Local Linux x86_64 qualification on 2026-10-10, Rust 1.85 release image, default
limits, disk-backed spools, 256 MiB worker memory/swap limit:

| Backend and profile | Upload outcomes | Peak RSS MiB | Peak cgroup MiB | Resting RSS MiB | Health p99 ms |
| --- | --- | ---: | ---: | ---: | ---: |
| PostgreSQL, five maximum Unicode/attachment uploads, database outage/recovery | 3 accepted, 2 quota refusals | 139.30 | 137.18 | 9.79 | 29.28 |
| SQLite, five maximum Unicode/attachment uploads | 3 accepted, 2 quota refusals | 117.57 | 222.66 | 13.34 | 31.71 |
| SQLite, two simultaneous maximum uploads, twice | 2 accepted, 2 admission refusals | 117.51 | 195.82 | 13.52 | 46.31 |
| SQLite, five maximum bodies with late syntax errors | 5 rejected with 400 | 36.79 | 30.57 | 11.33 | 1.27 |
| SQLite, five dense JSON bodies | 5 rejected with 413 | 36.90 | 33.44 | 11.20 | 1.70 |

All profiles reported zero OOM kills. Every accepted request was claimed once,
completed and recovered by its receipt. The harness also requires resting RSS
below 64 MiB to catch retained payload/driver allocations. Health samples are
local observations during these short probes, not a latency SLO or a throughput
benchmark; RSS and cgroup accounting include different kinds of memory.

Historical Python measurements are in `docs/relay-architecture.md`. This Rust
migration does not certify the 10,000-computer/50,000-phone planning target.
Measure sustained metadata/heartbeat rates, notification fanout, PostgreSQL WAL,
autovacuum, storage and pool saturation before changing deployment limits.
