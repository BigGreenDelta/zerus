# Rust relay runtime

`zerus-relay` is the standalone server for Zerus mobile API v1. It runs on the newest
stable Rust, Tokio, Axum/Hyper and SQLx. PostgreSQL serves multiple workers;
SQLite is an exclusive local backend. The production image contains the Rust
binary and its system libraries, without Python. The Python outbound connector
and installed Android clients use their existing protocol and credentials.

## Build and verify

From this directory:

```sh
cargo +1.99.1 build --release --locked
cargo +1.99.1 fmt --check
cargo +1.99.1 clippy --locked --all-targets -- -D warnings
cargo +1.99.1 test --locked
```

[`tests/ORACLE.md`](tests/ORACLE.md) maps every test of the Python reference
relay to the Rust contract or other test that replaces it.

The server is the default `server` feature. Without it the library is only the
API v1 codec (`json`), envelope validation (`protocol`) and `error`, with none
of the HTTP, database or TLS crates, for the outbound connector:

```sh
cargo +1.99.1 clippy --locked --no-default-features --lib --test codec --test vectors -- -D warnings
cargo +1.99.1 test --locked --no-default-features --test codec --test vectors
```

`tests/vectors.rs` checks the library against the golden vectors in
[`../vectors`](../vectors/README.md), read at run time: `request_hash` through
the codec at a claim batch's limits, the operation tables, and the `relay`
column of `validation/*.jsonl`, which `protocol::request` decides. It names every
case where that column differs from the frozen decision of the Python reference
relay. After a reviewed change to the validation or to a vector submission,
`ZERUS_BLESS=1 cargo +1.99.1 test --locked --test vectors` rewrites only that
column; then run `capture.py --check`.

The image and the infra packager copy only `Cargo.toml`, `Cargo.lock`, `.cargo`
and `src/**/*.rs|*.sql`, so the crate has no build script, declared test target,
path dependency or include from outside `src/`
([the source check](../../../tests/test_source_relay_inputs.py)).

### PostgreSQL tests

The storage and HTTP contracts (every `tests/*.rs` but the database-free
`codec.rs`, `vectors.rs`, `alerts.rs` and `private_files.rs`) and the connector
HTTP suites run on PostgreSQL. Point `ZERUS_RELAY_TEST_DATABASE_URL` at a
**disposable** server whose role may create databases: each test creates its
own `relay_test_<uuid>` database and drops it afterwards, also when it fails,
and credentials are not logged. The Python
fixtures also drop theirs after Ctrl+C; a killed or interrupted `cargo test` can
leave some behind. Without the variable the Rust contracts fall back to a
private SQLite file (`tests/python_receipts.rs`, which has only a PostgreSQL
database, passes without running) and the connector HTTP suites skip; a local
SQLite run never stands in for PostgreSQL. CI sets `ZERUS_REQUIRE_RELAY_E2E=1`,
which turns that fallback and those skips into failures. The end-to-end suites
(`test_end_to_end`, `test_presence_end_to_end` and `test_peer_end_to_end`) run
each flow against this tree's Python connector in-process and, with
`ZERUS_CONNECTOR_E2E_COMMAND` naming a built Rust connector
(`services/mobile/connector`), against that one as its own process with a
private configuration file; with the requirement set, both must run. A one-line server on
a random loopback port, whose data, leftovers included, leaves with the
container:

```sh
docker run --rm -d --name zerus-relay-test-postgres -e POSTGRES_HOST_AUTH_METHOD=trust -p 127.0.0.1::5432 postgres:17.11-alpine -c max_connections=300
until docker exec zerus-relay-test-postgres pg_isready -qh 127.0.0.1; do sleep 1; done
export ZERUS_RELAY_TEST_DATABASE_URL="postgresql://postgres@$(docker port zerus-relay-test-postgres 5432)/postgres"
```

With 32 test threads the parallel contracts peak near PostgreSQL's default of
100 connections, hence the higher limit. Then, in this directory (the
cross-worker contracts exist only on PostgreSQL and run explicitly):

```sh
cargo +1.99.1 test --locked
cargo +1.99.1 test --locked --test contracts postgres_cross_worker -- --ignored
```

From the repository root, with `services/mobile` and
`services/mobile/tests/requirements.txt` (the test-only database driver)
installed in a Python 3.11+ virtual environment:

```sh
export ZERUS_RELAY_BINARY="$PWD/services/mobile/relay/target/debug/zerus-relay"
export ZERUS_DIRECTORY_TEST_HGS="$PWD/target/debug/hgs"
export PATH="$PWD/services/mobile/relay/target/debug:$PATH"
python -m unittest discover -s services/mobile/tests -v
docker stop zerus-relay-test-postgres
```

`relay_fixture.start_relay` gives every end-to-end, peer, presence, directory,
speech and speech-configuration HTTP test its own database. It seeds it only
through the `zerus-relay` command line (`provision`, `node`, `invite`,
`revoke-*`) and `/v1/pair`, waits for `/readyz`, and the suites read no database
directly: the storage effects they used to inspect are Rust contracts. These
suites exercise the actual Python connector with synthetic native subprocesses,
exact canonical hashes, Unicode, attachments, one-hop routing and cancellation.
`test_rust_relay.py` runs the released HTTP contracts on the same fixture. It
observes expiry and retention over HTTP with second-scale settings
(`queue_ttl`, `claim_ttl` and `retention` of 1 second, background maintenance
every 0.1 seconds) and restarts the relay process on its database.
`tests/maintenance_contracts.rs` covers what HTTP cannot show, and
`tests/python_receipts.rs` loads `tests/fixtures/python_relay.sql`, a frozen
database the removed Python PostgreSQL relay wrote: its receipts keep their
identity and its schema upgrades in place. Without `ZERUS_RELAY_BINARY`, the
fixture serves the retained Python reference relay on a private SQLite store
and the Rust-only suites skip.
Hosted CI runs the discover once with both variables, `ZERUS_REQUIRE_RELAY_E2E=1`
and `ZERUS_DIRECTORY_TEST_HGS` naming the built `hgs`, then repeats
`-p 'test_mobile_speech*.py'` without `ZERUS_RELAY_BINARY`. The reference Python
HTTP module is retained as an independent compatibility oracle, not selected by
the supported server CLI. `zerus-mobile` delegates administration and serving to
`zerus-relay`.

`test_rust_relay.py` used to borrow its cases from the reference relay's
`test_relay.py` and to seed a Python-written SQLite store. Where each one went
(Python names drop the `test_` prefix; Rust contracts are in `tests/`):

| Former case | HTTP test in `test_rust_relay.py` | Rust contract |
| --- | --- | --- |
| `health_capabilities_and_local_only_administration` | same name | |
| `pair_once_and_expiry_and_hash_storage` | `pair_once_and_opaque_token` | `maintenance_contracts.rs` `invitations_expire_and_credentials_are_stored_only_as_hashes` |
| `pair_rate_limit` | same name; the fixture's pairing is the first attempt | `contracts.rs` `scoped_pairing_and_rates` |
| `failed_authentication_rate_limit` | same name | |
| `workspace_and_role_isolation` | same name, the foreign computer from `provision` | |
| `durable_claim_duplicate_result_and_owner` | same name, the other phone from `invite` and `/v1/pair`, the other node from `node` | `contracts.rs` `claims_and_receipts_never_replay` |
| `single_claim_and_request_validation` | same name | |
| `expiry_restart_and_late_result` | `expiry_and_restart_keep_claims` (a real restart; PostgreSQL keeps the claim) and `claim_timeout_late_result_and_revoke` | `contracts.rs` `budgets_reserve_results_and_release_on_expiry`, `restart_retires_claims_only_on_sqlite`, `expired_claims_and_revocation_are_terminal` |
| `queue_limit_retention_tombstones` | same name | `contracts.rs` `claims_and_receipts_never_replay`, `budgets_reserve_results_and_release_on_expiry` |
| `claim_timeout_and_revoke` | `claim_timeout_late_result_and_revoke`, revoking with `revoke-node` and `revoke-device` | `contracts.rs` `expired_claims_and_revocation_are_terminal` |
| `device_logout_revokes_token_and_cancels_queue` | same name; the computer never receives the command | `maintenance_contracts.rs` `device_logout_fails_its_queued_requests` |
| `old_read_history_does_not_consume_mutation_tombstones` | same name | `maintenance_contracts.rs` `read_history_expires_without_consuming_mutation_tombstones` |
| `long_poll_rechecks_revocation` | same name, revoked by another process before the poll's deadline | |
| `heartbeat_events_baseline_transition_and_privacy` | same name, the other workspace from `provision` | |
| `body_cap_nonfinite_and_poll_bounds` | same name | `contracts.rs` `http_oversized_declared_body_is_answered_after_bounded_drain` |
| `async_question_fingerprints_add_change_remove` | same name | |
| `push_retired_unifiedpush_and_unavailable_fcm` | same name | |
| `mark_read_is_exact_and_capability_gated` | same name; the operation set from `vectors/operations.json` | `contracts.rs` `mark_read_payload_is_exact_and_capability_gated` |
| `set_mobile_delivery_request_validation` | same name | `contracts.rs` `mobile_delivery_is_computer_scoped_and_capability_gated` |
| `mark_read_is_read_class_and_pruned_like_reads` | same name | `contracts.rs` `mark_read_is_read_class_and_pruned_like_reads` |
| `attachment_read_is_exact_gated_and_short_lived` | same name, with the 5-second grace after reading | `contracts.rs` `attachment_read_is_exact_gated_and_short_lived` |
| `existing_python_receipt_identity` | | `python_receipts.rs` `python_receipt_identity_and_presence_alerts_survive_the_upgrade` |
| `presence_alerts_http_contract` | same name, on a database the Rust relay created | `python_receipts.rs` (a Python-written database upgraded in place) |

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
  `remove_queued` (OpenCode) is a session mutation with a durable request
  identity, like `forget`: exact `expected_run_id`, nonempty
  `expected_conversation_id` and one `message_id`, `msg_` followed by 1–124
  ASCII letters, digits, `_` or `-`
  ([shared with hgs, the desktop and the connector](../../../tests/fixtures/opencode-queued-message-ids.json));
  any other field is rejected. Submission requires the computer to advertise
  it, which only the Rust connector does, with the scoped session-action ABI.
  An older hgs prints that marker too and refuses the request (an uncertain
  receipt, nothing removed), so the session's `allowed_actions` listing
  `remove_queued` is what shows that its hgs can remove a message; phones
  require both. The golden vectors freeze the Python connector's tables, so
  `tests/vectors.rs` names the operations they lack from
  [`rust-only-operations.json`](../tests/rust-only-operations.json).
- `json.rs` bounds depth and lexical structure before object materialization.
  Canonical ASCII escaping, key ordering, arbitrary integer precision and Python
  float formatting preserve API v1 request identities. A deterministic 20,000
  float differential test compares the independent Python encoder. Containers
  are decoded from borrowed raw JSON so the legal object key
  `$serde_json::private::Number` cannot be interpreted as serializer metadata.
  `tests/python_receipts.rs` replays a Python-written receipt with this key,
  Unicode and large numbers.
  The encoder sorts keys itself rather than trusting `Map` order, which
  serde_json's `preserve_order` feature would change; CI rejects that feature.
  `python_default_len` measures plain `json.dumps` output, as the connector's
  size bounds do. `tests/codec.rs` compares both forms with CPython on 20,000
  seeded values and documents the parse limits, including a claim batch's margin.
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
  needs a fresh peer heartbeat after it is marked online again. Its
  `last_seen_at` keeps the last accepted peer heartbeat (null if this native
  UUID was never observed on the route) for display only: availability counts a
  peer route's time only while it stores a snapshot accepted since it was last
  marked online, and gateway heartbeats never refresh it.
- Shared session notification revisions belong to the physical computer, provider
  and conversation, independently of gateway routes. The additive
  `computer_notification_preferences` table retains complete confirmed preferences
  with no expiry or LRU eviction. A computer may retain at most 4,096 scopes and
  1 MiB of canonical JSON (also bounded to 32 levels and 50,000 JSON values).
  Its exact encoded bytes and workspace/computer key bytes count toward the
  configured workspace and sharded global retained-payload quotas. Capacity or a
  conflicting/older revision rolls back the heartbeat and ledger together; it
  never removes an older highwater to make room. Unexposed computer identity
  consolidation merges the maps and returns the retired row's quota charge.
  Catalogs, command route selection and alert evaluation overlay these preferences
  on the selected route's same-provider/conversation rows. Native state, run IDs,
  read marks and route freshness remain the selected route's evidence. Projected
  snapshot and catalog bytes are checked before returning; one computer ledger
  is loaded at a time. A new higher revision does not require the old route to
  remain online or registered. This new table records observations accepted by
  this implementation; it cannot recover a preference never received by it.
- `store/snapshots.rs` separates liveness and event-free snapshot changes from
  shared payload accounting when no shared preference is present. Preference-bearing
  snapshots use global event locks when needed, then the payload shard, sorted
  workspaces, credential and route. Event publication and wake jobs have transactional
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
  Missing or malformed information means away.
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
- `push.rs` sends generic wake hints through FCM HTTP v1 and APNs. Provider
  requests run outside transactions and never target client-supplied URLs. FCM
  uses short-lived service-account OAuth tokens and an RS256 signature from
  `ring`; APNs uses an ES256 provider token from `ring`, refreshed every 40
  minutes, over HTTP/2. Tokens and provider errors are never logged. Registrations from the removed
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
A refused body is first discarded, unretained and for at most two seconds, so a
client still uploading reads the status instead of a connection reset. A body
refused for its declared or streamed size (413) or because the large payload
lane or small-body budget is full (429) is discarded up to the route's body
limit; an unknown-length stream over its limit releases its large payload
lane first. A refusal before the body is read (forwarding header,
Content-Encoding or Content-Type 415, authentication 401, rate-limit or
request-lane 429, unknown route, handler deadline) answers a client that is
unverified, misdirected or over a limit: it discards at most 64 KiB and holds
no admission permit meanwhile. Requests with `Expect: 100-continue` are refused
before any body is sent. A body that times out or fails while it is being read
is not discarded.
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

Offline peer routes keep `computer_routes.last_seen` instead of clearing it,
with no schema change or API field change. Replace all PostgreSQL workers
together: an older worker clears the time on its own offline transitions and,
for up to 45 seconds after the last peer heartbeat, could treat a peer that the
gateway marks online again as fresh before its next peer heartbeat. The Python
reference relay keeps the time with the same freshness rule, so a Rust-to-Python
rollback stays safe.

Attachment reads add only the `attachment_history` partial index (schema version
1; the Python reference creates it too). Its first `CREATE INDEX` scans the
requests table once, briefly blocking writes on PostgreSQL. Phones use the
operation only when both relay capabilities and the computer snapshot list it,
so the relay, connector and app can be deployed in any order; replace all
PostgreSQL workers together so every worker accepts and expires the operation.

Queue removal (`remove_queued`) adds no table, column or index: it is an
ordinary mutation with permanent request identity. Phones use it only when both
relay capabilities and the computer snapshot list it, so the relay, connector
and app can be deployed in any order; replace all PostgreSQL workers together so
every worker accepts it.

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

### Archive date pages

`archive_list` is an additive capability-gated, sessionless read. Payload fields
are `from`, `until` (exclusive Unix seconds, 0–253402300800), `limit` (1–50),
`cursor` (at most 1024 base64url characters), and literal `query` (256 printable
characters). The connector invokes only `hgs archives --json`; the native CLI
validates the cursor, range and catalog again. Native desktop/connector polls omit archived session rows; an upgraded phone
requests pages only inside Archive. To retain older Android clients, the
connector attaches a cached recent compatibility window (at most 50 rows and
128 KiB) plus explicit `mobile_archive` loaded/total/partial metadata. Native
`archive_revision` invalidates it on an atomic archive-directory change, and it
expires after five minutes. Upgraded clients ignore this window when the relay
and node advertise page support. Native identity checks remain authoritative.

The operation has a separate `archive_history` index and one-hour read retention;
the established `read_history_v3` predicate and durable mutation identities stay
unchanged. Apply the additive index to both PostgreSQL workers during the normal
coordinated runtime replacement (fresh backup and isolated restore qualification).
Update the native CLI and connector before enabling the relay capability, then
update clients; an older relay/CLI is reported as unavailable, not an empty page.
No native agent or phone read-mark migration is needed.

### Git checkout status reads

`git_status` is a sessionless computer read offered by the Rust connector only
when the native help contains `Mobile Git status ABI: git-status-v1`. A request
has an empty session and exactly `{request_id,path}` as its payload; no
run, conversation, agent or archive identity is accepted. The path must be an
absolute POSIX path with a leading slash, control-free and at most 4096 UTF-8
bytes, matching the existing mobile folder contract. Windows Git transport is
unsupported and unqualified in v1. Normal verified
local and one-hop routes apply; the read grants no forwarding or mutation
authority. Exact duplicate receipts retain their existing identities.

The operation shares the 5000-row workspace read quota and has a separate
`git_status_history_v1` partial index for bounded terminal-row maintenance,
with a retention ceiling of one hour. Existing `READ_SQL`, `read_history_v3`,
other index definitions and mutation retention predicates remain unchanged.
Deploy the additive index through the normal root-coordinated rollout; source
qualification does not switch any existing gateway to the Rust connector.

### Explicit prompt dismissal with context clear

`clear_context` accepts the optional strict Boolean `dismiss_prompt` when both
the relay and selected computer advertise `clear_context_dismiss`. Any presence,
including `false`, requires that feature after exact duplicate receipt lookup.
Omitting the field retains the ordinary request body/hash and validation;
`compact_context` rejects it. The connector separately verifies the native HELP
marker `Mobile clear-context ABI: dismiss-prompt-v1`. A `true` request requires
fresh exact native identity, an unarchived session and actual Boolean
`clear_context_dismiss: true`; the relay does not infer prompt eligibility.
An uncertain receipt retains its original identity and never permits replay.
