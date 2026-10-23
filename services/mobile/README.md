# Zerus mobile relay

A self-hosted Rust relay connects the Android client to computers running
`hgs`. Computers make outbound HTTPS connections; phones never receive SSH keys.
An enrolled gateway also exposes its enabled direct Zerus peers through its
existing SSH access. Requests to a peer go through that gateway; the peer's own
neighbors are not discovered or authorized recursively. See the
[gateway upgrade procedure](../../docs/mobile-deployment.md#upgrade-gateway-sharing)
before updating a multiple-worker installation.
For legacy API v1, the relay is trusted with snapshots, requests and conversation
results; TLS protects transport. Version 1 does **not** provide end-to-end
encryption. Opt-in API v2 encrypts private payloads between verified devices and
keeps routing, delivery and bounded service metadata at the relay. See
[encrypted workspaces](../../docs/encrypted-workspaces.md) for Create/Join/Change,
device invitations, trust limits and the
[public-mode resource requirements](relay/README.md#opt-in-public-encrypted-workspaces).

See [the architecture and wire contract](../../docs/mobile-architecture.md) and
[deployment instructions](../../docs/mobile-deployment.md). The legacy setup below
uses local CLI provisioning. Public API v2 adds explicit device/workspace
enrollment when enabled by the operator; service priority stays operator-owned.
A legacy phone can revoke its own credential with `DELETE /v1/device`. A
computer credential may invite, list and revoke the phones of its own workspace
(`/v1/node/invitations`, `/v1/node/devices`); Zerus desktop uses them for
**Machines → Phones**.

## Local installation

```sh
cd services/mobile/relay
cargo +1.99.1 build --release --locked
mkdir -p "$HOME/.local/bin"
install -m 0755 target/release/zerus-relay "$HOME/.local/bin/zerus-relay"
zerus-relay --database ./data/relay.sqlite3 provision \
  --name 'Example workspace' --computer-name 'Example computer'
zerus-relay --database ./data/relay.sqlite3 serve
```

The server is a standalone Rust executable built on Tokio, Axum and SQLx. The
Rust outbound connector implements API v1 and the encrypted API v2 workspace
flow. Existing Python 3.11+ API v1 gateways remain supported until an explicit
[per-computer cutover](../../docs/mobile-deployment.md#move-a-computer-to-the-rust-connector).
The relay image contains no Python interpreter. FCM HTTP v1 support is
built into the server; provider credentials remain private runtime configuration. See [the Rust runtime contract](relay/README.md)
for resource ownership, compatibility validation and the upgrade procedure.

Provisioning deliberately prints the node token and a single-use phone pairing
code. Transfer them privately; never paste them into logs or repository files.
The invitation expires after ten minutes. The state directory must be private
(mode 0700); the relay creates its database with mode 0600 and refuses database
symlinks. It never changes permissions on an existing parent directory.

Add another computer, invite another phone, or revoke a credential locally:

```sh
zerus-relay --database ./data/relay.sqlite3 node \
  --workspace WORKSPACE_UUID --name 'Another computer'
zerus-relay --database ./data/relay.sqlite3 invite --workspace WORKSPACE_UUID
zerus-relay --database ./data/relay.sqlite3 revoke-device --id DEVICE_UUID
zerus-relay --database ./data/relay.sqlite3 revoke-node --id NODE_UUID
zerus-relay --database ./data/relay.sqlite3 revoke-computer --id COMPUTER_UUID
```

`revoke-node` removes that gateway's credential and routes. Independently enrolled
routes to the same computer remain valid. `revoke-computer` blocks the physical
computer and its aliases through every gateway in the workspace.
Revoking a computer as a target leaves its gateway credential intact: it can
still provide access to other authorized peers. Revoke its node credential too
when the gateway itself must lose access.

The default listener is `127.0.0.1:8787`. Place it behind a TLS proxy before using
it on another machine. Keep access/body/header logging disabled at the proxy.
SQLite supports one relay process only, behind the serialized async adapter;
restarting that exclusive process marks existing claims uncertain. PostgreSQL is
the production backend for multiple workers; worker restart does not retire
another worker's claim. Back up the entire private state directory while stopped or
use SQLite's online backup API. A backup contains session data and push endpoints.

## Docker and optional Caddy TLS

```sh
cd services/mobile
docker compose build
docker compose run --rm relay provision \
  --name 'Example workspace' --computer-name 'Example computer'
docker compose up -d relay
```

Compose publishes the relay only on loopback. For Caddy TLS, edit `Caddyfile` to
replace the reserved `relay.example.com` domain, point that domain at the server,
and run `docker compose --profile tls up -d`. Caddy needs ports 80 and 443; do not
publish port 8787 on a public interface. If using an existing TLS proxy, route it
to the loopback port instead. Liveness: `GET /healthz` reports protocol version 1 independently of storage.
Readiness: `GET /readyz` returns 503 when the database cannot answer. Caddy
replaces inbound `X-Forwarded-For`; configure the relay to trust only its actual
peer address. The supplied network assigns Caddy `172.30.78.2`, so use
`"trusted_proxy_cidrs": ["172.30.78.2/32"]` in private relay configuration.
For SQLite, mount that private configuration with a Compose override and pass
`serve --host 0.0.0.0 --config /private/relay.json`. Without explicit proxy trust,
rate identity remains the socket peer. Review the subnet for local conflicts. The dynamic IPAM range
`172.30.78.128/25` deliberately excludes Caddy's static `.2` address; preserve
that separation in any private network override.

The relay uses a private disk-backed `/spool` volume for response encoding;
`/tmp` remains a small tmpfs. Each worker reserves at most 64 MiB of spool
capacity before encoding, with 32 MiB per claim response and 1 MiB per ordinary
response, and closes/unlinks files on completion or cancellation. Reserve
64 MiB of free disk per worker plus filesystem overhead. Named spool volumes
may be shared safely; this capacity multiplies by worker count. Keep their
permissions 0700 and include them in host disk monitoring. Do not move the
spool to tmpfs under the 256 MiB worker memory limit.

### Windows hosts: use Linux containers

Run the same Linux relay image on Windows; a native Windows relay port is not
provided. The relay is the HTTPS server shared by phones and computers. It is
separate from the [Windows outbound connector](../../docs/mobile-deployment.md#windows-gateway),
which runs beside `hgs.exe` and exposes that computer's sessions to a relay.
The relay may run on the same PC or on another host.

For Docker Desktop, use a Windows version covered by
[Docker's current requirements](https://docs.docker.com/desktop/setup/install/windows-install/),
select **Linux containers**, enable its WSL 2 backend and
[integration with your WSL distribution](https://docs.docker.com/desktop/features/wsl/).
Run the commands below in that distribution's Linux shell, from a checkout in
its Linux filesystem. Keep private configuration there too, rather than under
`/mnt/c` or another Windows-drive mount: the relay requires real Linux ownership,
private file modes and locking. Docker Desktop
[does not support Windows Server](https://docs.docker.com/desktop/troubleshoot-and-support/faqs/windowsfaqs/#can-i-run-docker-desktop-on-windows-server);
use a Linux VM or a separate Linux Docker host for that case.

For a **new** single-worker SQLite installation:

```sh
cd services/mobile
docker compose --project-name zerus-relay build relay
docker compose --project-name zerus-relay run --rm relay provision \
  --name 'Example workspace' --computer-name 'Example computer'
docker compose --project-name zerus-relay up -d relay
```

Use the same Compose project name for every later command. An existing deployment
must keep its original project name and volumes; choosing a different name starts
with different state. The supplied `relay-data` and `relay-spool` named volumes
keep SQLite and spool storage inside Docker's Linux filesystem. The image owns
their initial directories as UID/GID 10001 with mode 0700. Keep these volumes
when recreating containers; volume deletion, `down --volumes`, pruning or a
Docker Desktop data reset can destroy credentials, receipts and conversation
data. Follow the stopped/online SQLite backup rule above before changing storage.
See [Docker's volume lifecycle](https://docs.docker.com/engine/storage/volumes/).

Keep the `127.0.0.1:8787:8787` publication. After replacing `relay.example.com`
in `Caddyfile` with your domain and arranging DNS, host/router access to ports
80 and 443 and valid HTTPS, start the supplied proxy with:

```sh
docker compose --project-name zerus-relay --profile tls up -d relay caddy
```

An existing TLS proxy can use the published loopback port instead. Container
addresses and Compose names are internal; no host-network mode is required.
Use the relay's reachable HTTPS URL for the phone and the desktop connector,
rather than `localhost` or the Compose name `relay`. Enter the provisioned computer
credential in **Settings → Mobile connection**, then use **Phones** to make an
invitation for that workspace. Keep provision output private. Retain the proxy
trust and private/shared relay limits described above.

For multiple workers, use the PostgreSQL profile below, adding
`--project-name zerus-relay` to each Compose command too. Leave database storage
in its named volume. Private configuration and
Compose secret files must stay in the WSL Linux filesystem with the documented
0600 modes and container-readable ownership; a Windows bind mount or more
permissive mode is not a substitute. Never publish PostgreSQL's port. This uses
the existing API, pairing credentials and durable receipts; moving an existing
relay or gateway still requires its separate backup and cutover procedure.

## PostgreSQL production profile

Read the [relay implementation contract](../../docs/relay-architecture.md) before
sizing production. The supplied `postgres` profile runs a private PostgreSQL
service and two independent 256 MiB HTTP workers. PostgreSQL stays on major 17
and pins the verified 17.11 image manifest; review minor security updates against
[the official versioning policy](https://www.postgresql.org/support/versioning/). It preserves the v1 protocol;
this is a bounded inline-payload deployment, not a demonstrated capacity for
ten thousand computers or fifty thousand phones.

Create `services/mobile/private/` outside version control with mode 0700. The
bootstrap password file is `private/postgres-password`; the relay configuration
is `private/relay.json`. Compose's `POSTGRES_USER=zerus_relay` creates the
bootstrap superuser for initialization and local administration only. The relay
must use a distinct restricted login, `zerus_relay_app`, which owns its database
and schema. Generate independent random passwords privately; URL-encode only
the application password inside the DSN. Never pass passwords in command
arguments or record them in logs. Configuration example:

```json
{
  "database_url": "postgresql://zerus_relay_app:REPLACE_WITH_URL_ENCODED_PRIVATE_APP_PASSWORD@postgres:5432/zerus_relay",
  "trusted_proxy_cidrs": ["172.30.78.2/32"],
  "pool_min": 2,
  "pool_max": 10,
  "global_max_bytes": 34359738368,
  "quota_shards": 64,
  "max_polls_per_credential": 2,
  "max_polls_per_workspace": 32,
  "max_polls_global": 256,
  "large_payload_slots": 1
}
```

Both files must have mode 0600. Local Docker Compose secrets use bind mounts,
so host file ownership matters: make `relay.json` readable by UID 10001 and
`postgres-password` by the PostgreSQL image's UID 70 (Alpine). Verify those IDs
against the reviewed images before starting. Do not weaken file modes to make
mounts work. Keep private overrides, provider credentials and backups outside
version control. The database port is never published.

For a new, empty installation, build and start only the named production
services; a bare `up` also selects the default SQLite service:

```sh
docker compose --profile postgres build relay-postgres
docker compose --profile postgres up -d postgres
docker compose --profile postgres exec postgres psql -U zerus_relay -d postgres
```

Before provisioning, use that private administrator session to create the
application role and assign ownership. `\password` prompts without echo; enter
the application password used in `private/relay.json`. Keep the bootstrap secret
out of the relay configuration and do not grant application superuser privileges:

```sql
CREATE ROLE zerus_relay_app LOGIN NOSUPERUSER NOCREATEDB NOCREATEROLE
  NOREPLICATION NOBYPASSRLS;
\password zerus_relay_app
ALTER DATABASE zerus_relay OWNER TO zerus_relay_app;
\connect zerus_relay
ALTER SCHEMA public OWNER TO zerus_relay_app;
\quit
```

Then provision and start the relay with its application DSN:

```sh
docker compose --profile postgres run --rm --no-deps relay-postgres \
  provision --config /run/secrets/relay.json \
  --name 'Example workspace' --computer-name 'Example computer'
RELAY_HOST=relay-postgres docker compose --profile postgres --profile tls \
  up -d postgres relay-postgres caddy
```

Provisioning prints credentials deliberately; transfer them privately. The two
relay workers share durable rates, long-poll leases, queue claims and payload
accounting. Their idle waits hold no query-pool connection. Database connection
planning includes the configured query pool plus one dedicated LISTEN connection
per worker. Caddy's built-in dynamic A-record upstream refreshes the Compose service DNS
list every five seconds and round-robins across replica addresses, with passive
failure detection and no request replay retries. Verify both replica addresses
appear before load testing. A Caddy restart is necessary after changing its
upstream environment. See [Caddy dynamic upstream documentation](https://caddyserver.com/docs/caddyfile/directives/reverse_proxy#dynamic-upstreams).
Move an existing SQLite installation with the v0.42.2 migration instead of
provisioning new identities; see
[SQLite installations](../../docs/mobile-deployment.md#sqlite-installations).

## Isolated capacity probes

Build the Rust image, then exercise its complete upload, claim, result and
receipt lifecycle inside a disposable 256 MiB cgroup:

```sh
docker build -t zerus-relay-contract services/mobile
PYTHONPATH=services/mobile python services/mobile/relay/tests/load.py \
  --image zerus-relay-contract --kind attachments_unicode --repeats 5
PYTHONPATH=services/mobile python services/mobile/relay/tests/load.py \
  --image zerus-relay-contract --kind late-malformed --repeats 5
PYTHONPATH=services/mobile python services/mobile/relay/tests/load.py \
  --image zerus-relay-contract --backend postgres
```

Run from the repository root in a Python environment with the connector/test
dependencies installed. The harness accepts no live URL or credentials. It
creates and removes its own container and disk-backed database/spool volumes,
streams fixture files, samples health latency and reports process RSS, cgroup
peak memory and OOM events. `--concurrency 2` also exercises early backpressure.
Other profiles cover dense, deeply nested and mixed Unicode JSON.

The previous `tests/relay_load.py` remains a comparison probe for the Python
reference implementation. Its historical measurements in
`docs/relay-architecture.md` do not certify the Rust implementation. Neither
the functional contracts nor a small payload probe establishes fleet capacity;
measure database/WAL, query pools, bandwidth and notification fanout before
raising limits or sizing a ten-thousand-computer deployment.

## Push configuration

Push is disabled by default. `GET /v1/capabilities` requires phone authentication
and lists the configured `push_providers`. Create a private JSON file with mode
0600 and pass `serve --config /private/path/config.json`:

```json
{
  "fcm_credentials": "/private/path/firebase-service-account.json"
}
```

UnifiedPush support was removed. A legacy `push_hosts` key is still accepted so
existing private configurations load; it is ignored with a startup warning.
Registrations left from UnifiedPush are removed on their next delivery attempt.

iOS wakes use APNs token authentication. Add the private signing key Apple
issues (`.p8`, mode 0600, outside the repository), its key ID and the owning
team ID; `apns_topic` defaults to `app.zerus.mobile`:

```json
{
  "apns_key": "/private/path/AuthKey.p8",
  "apns_key_id": "ABCDEFGHIJ",
  "apns_team_id": "TEAM123456"
}
```

The relay then advertises `apns` (production) and `apns_sandbox` (development
builds) in `push_providers`. Each wake is a generic alert, "New session
activity.", with only `{event_id, kind}` beside it; the app decides any detailed
card after authenticated reads. Device tokens that Apple reports as unregistered
are removed.

FCM uses the optional pinned `firebase-admin` package and its HTTP v1 transport.
Keep the Google service account JSON outside the repository, readable only by the
relay user (mode 0600), and restrict its Firebase permissions to sending messages.
For Docker, mount private configuration and credentials at explicit paths and use
a Compose override to add `serve --host 0.0.0.0 --config /private/config.json`.
Credential paths and mounts belong in private deployment configuration.

Push payloads contain only `event_id` and `kind: "wake"`; they contain no session
names, messages or questions. The phone fetches details through its authenticated
API. Delivery runs outside heartbeat handling, retries at most five attempts and
expires after 24 hours. Unregistered provider targets are removed. Revoking a
phone deletes its push registration and pending delivery jobs.

## Delivery and resource limits

Requests require a UUID and an operation advertised by `/v1/capabilities`.
The existing inspection, history, message, question, session lifecycle, Terminal,
project and worktree operations retain their API v1 envelopes.
Mutation payloads carry the same UUID plus exact run/conversation expectations.
An identical repeated mutation gets the existing envelope. Changed content gets
409. The relay commits a durable claim before delivery and never requeues a
claimed command. An expired queue entry fails; a stale claim becomes uncertain.
Uncertain input needs explicit user review and a refreshed native identity.

Defaults: 1 MiB HTTP body for ordinary endpoints, 29 MiB for native send request
envelopes, 10-second ordinary body timeout, 60-second send body timeout,
25-second long poll, 40-second ordinary handler deadline and 75 seconds for
request submission, 120-second queued expiry, 90-second claim timeout, and
200 pending requests per workspace. Retained request and result/error bytes plus result reservations across all states
have a 100 MiB workspace payload budget plus a separately bounded 1 MiB
allowance for small text and control requests. Each new request reserves a
maximum 1 MiB canonical UTF-8 result/error data before admission. The HTTP layer
also checks the complete escaped receipt envelope against the installed phone's
1 MiB total response cap before accepting a result. Unicode
escaping counts toward that stored ceiling; oversized results receive 413. Identical UUID retries still return their receipt when
the budget is full; uncertain requests are never evicted to make room.
Payload/result/event retention is seven days. Completed
read-only inspections expire after one hour; repeating an expired inspection is
safe. Mutation UUID tombstones remain indefinitely, with a 100,000-mutation
workspace ceiling. Retained inspections have a separate 5,000-entry ceiling.
These limits never evict a mutation tombstone to permit accidental redelivery.

Local transport defaults admit one large v1 pipeline per worker, covering body
ingestion through parsing, validation, database work and claim serialization
until response completion. Unknown-length bodies also use that lane. Occupied
capacity returns 429 with `Retry-After`; there is no unbounded waiter queue.
Ordinary bodies share an 8 MiB aggregate wire budget, and at most eight ordinary
responses are retrieved/encoded concurrently. A computer catalog is rejected
with 413 if its aggregate stored snapshots and metadata exceed 1 MiB; v1 has no
catalog pagination. Ordinary catalogs and receipts enforce the actual 1 MiB
escaped response limit before headers are written. Event pages return the fitting
prefix and advance the cursor only through returned rows; one oversized legacy
event or receipt reports 413 instead of silently truncating it. JSON is limited to depth 32 and 50,000 lexical values before
full parsing, and base64 validation uses aligned 64 KiB chunks. Parsing and
SQLite work run off the HTTP event loop; cancellation keeps permits until real
thread completion. Compressed bodies are rejected.

Long polls use shared leases: two per credential, 32 per workspace and 256 per
worker. Notifications trigger durable rereads with a five-second fallback;
maintenance runs independently in bounded batches. A worker must be sized from
measured RSS, container memory, CPU, query latency, disk and bandwidth. The
single large-payload lane is the compatible v1 ceiling; increasing it requires
new resource evidence. A negotiated streaming blob protocol remains future work.

The computer connector uses the same retention policy for delivered results:
inspections expire after one hour; mutation responses become an explicit
`history_expired` receipt after seven days. Pending result uploads are retained.
The connector keeps mutation UUIDs and canonical request hashes indefinitely,
so changing a replayed request cannot execute a new command. Legacy journal
records receive the migration time as their age; an unknown legacy request hash
never implies that a newly received command matches it.

Invitations are single-use, use 256 bits of randomness and expire after ten
minutes. Node/device tokens use the same entropy and only SHA-256 token hashes
are stored. Push registrations and session data remain readable in the trusted
relay database. Pairing and failed authentication are rate limited. Events and
push jobs have additional hard global ceilings of 100,000 and 10,000 rows.

Events come from snapshot transitions. The first snapshot is silent. A new
attention identity, a new async question fingerprint, busy-to-idle completion or
a new error emits a generic event. Async question fingerprints come from the
connector's bounded round-robin inspection, so notifications can lag by a scan
cycle when many sessions are running.

Each `mobile_attention` item is `{question_id, question_hash}`. An optional
Codex async question, one the agent keeps working without (`source`
`codex_async` with `optional` exactly `true` in `hgs inspect`), also carries
`optional: true`; blocking items never carry the key. Question text and other
card fields never leave the computer. Both relays fingerprint whole items and
treat optional items like any other: they emit the same `attention` event and
alert, and the phone decides whether an optional item notifies. A connector
without the marker omits the key, so the first heartbeat after upgrading emits
one `attention` event for each session with a pending optional question.


## Native projects and attachments

The legacy `snapshot.projects` object contains CLI directory aliases and does not
represent desktop logical groups. The connector reads `hgs swarm get` every
15 seconds with a three-second timeout and attaches `snapshot.mobile_projects`.
It includes all native group headers (`id`, `name`, `color`, `accessible`) plus
this computer's folders, exact session names and archive IDs. It excludes peer
configuration, catalog conflicts and history. Each session receives
`mobile_project_id` from its exact native assignment or the verified default.
Custom worktree names never determine the logical project. Shared project keys
include `swarm_id`; an uninitialized/unavailable catalog has an explicit stable
local fallback identity and no invented project list. Transient failures retain
the previous catalog with `stale: true`.

Phone attachments use the existing native send payload:

```json
{
  "attachments": [{
    "name": "example.txt",
    "mime": "text/plain",
    "data_base64": "ZXhhbXBsZQ==",
    "reference": "[File #1]"
  }]
}
```

This is part of the identity-checked `send` payload, alongside its request UUID,
text and expected run/conversation IDs. Limits match native input: eight files,
10 MiB per file, 20 MiB total decoded bytes and 64 KiB UTF-8 text. The relay and
connector validate independently. Names must be plain filenames; filesystem
paths, URLs and unexpected fields are rejected. References are optional native
`[Image #N]` or `[File #N]` labels and must be unique. Base64 is standard and
canonical. The phone supplies bytes, never a path on the computer.

The native `hgs send` implementation creates immutable private copies using its
existing attachment storage and durable input receipts. The connector passes the
validated native ABI without introducing a shell or path-based upload command.
Its permanent request fingerprint includes the attachment bytes, so uncertain
or duplicate sends cannot repeat delivery. Request polling claims at most one
command per batch and permits bounded 29 MiB node responses; native inspection
and result limits remain unchanged. At most one large upload or claim response is read
concurrently; small authenticated requests retain separate capacity.

`GET /v1/capabilities` exposes these limits in `attachment_limits`. A TLS proxy
must allow 29 MiB only on `POST /v1/requests` with at least 75-second upstream
read/write deadlines; keep the 1 MiB limit on other endpoints. The supplied
Caddy configuration applies this route distinction.

`attachment_read` lets a phone open a sent file. It names a receipt
(`message_id` from a history row's `attachments[].request_id`) and `index`,
never a path, and asks for one range of at most 512 KiB. The connector
advertises it when native help lists
`attachment <session> --request ID --index N`, verifies run/conversation with
`hgs inspect`, and reads the file once through `hgs attachment`, which enforces
receipt ownership, the private store, no symlinks and the 10 MiB bound. It keeps
the verified bytes in memory for at most 120 seconds (30 MiB per machine) and
returns `{…, bytes, sha256, offset, length, data_base64}`; the journal drops
delivered ranges after 120 seconds. See `docs/mobile-architecture.md`.

## Archived conversations

Current, paused and stopped conversations keep the ordinary `inspect` payload
`{}`. Archived browsing supplies `{"archive_id":"CANONICAL_UUID"}` instead.
The connector requires the exact session name and immutable archive UUID in its
native snapshot, no older than 45 seconds, and invokes only
`hgs inspect SESSION --archive UUID`. The returned name and archive ID must
match. An ordinary inspection cannot silently return an archive if a live name
disappears during the request. Any nonblank archive ID identifies an archive,
even if a compatibility snapshot omits its `state` field.

Archives are read-only through mobile. All mutations reject archive IDs;
archived-only names cannot fall back to a current session. Project membership
uses the archive UUID, so a saved conversation and its same-name live session
can remain in different logical projects. Archive rows do not affect live
notification transitions.

For synthetic five-filter QA, set `ZERUS_MOBILE_FIXTURE_CATALOG_MODE=filters`
when running the test connector, or put `{"all_states":true,"send_delay":8}`
in `fixture-config.json` inside `ZERUS_MOBILE_FIXTURE_DIR`. The next fixture CLI
call picks it up without restarting any agent. It exposes seven rows: current,
working, attention, paused, stopped and two distinct archives sharing the current
session name. The General group is empty, current/saved rows share the work
project and both archives belong to a separate history project. Native-shaped
archive inspections return distinct synthetic conversation IDs and reply text.
The same private configuration accepts `inspect_delay` in seconds (maximum 30)
and `history_count` (maximum 500 journal/provider rows) for cache and rendering
checks. These options alter only inspection output; generated messages have
stable native IDs and do not modify the saved fixture session.

## Context usage and controls

Inspect results preserve native `session_usage.context` (`used`, `limit` and
optional estimate metadata), `session_usage.prompt_cache`, `cache_hint`,
`compact_context_request` and the two native context support flags. Missing
measurements remain unknown. A native saving suggestion is distinct from a
reported cold cache; provider cache deadlines are estimates supplied by native
usage, separate from the phone's encrypted history cache.

The authenticated relay capabilities include `operations`. A current connector
adds `snapshot.mobile_capabilities` with `protocol_version: 1` and its supported
transport operations. Both declarations are required before enabling new context
controls; an older node with no declaration receives HTTP 409 for these operations.
Ordinary version 1 sends retain their behavior. The connector additionally reads a
fresh inspect result, checks exact name/run/conversation identity and requires the
corresponding native support flag before attempting a control. An older native
binary or unsupported agent therefore cannot trigger a terminal command.

`compact_context` and `clear_context` accept only this payload:

```json
{
  "request_id": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
  "expected_run_id": "example-run",
  "expected_conversation_id": "example-conversation"
}
```

The UUID must match the outer request; both native IDs must be nonempty. The only
commands are fixed `hgs compact-context SESSION --json` and
`hgs clear-context SESSION --json`, using JSON stdin. Archives, extra fields and
arbitrary provider commands are denied. Native acknowledgements must match the
request UUID, session name and original run/conversation. Compaction acknowledges
`submitted`; clear acknowledges `submitted` or `confirmed`. A completed relay
envelope means that this acknowledgement arrived, not that compaction finished.
Once a mutation is invoked, an error or unverified acknowledgement is uncertain
and its UUID cannot execute again.

Compact-before-send uses a separate send UUID and optional
`expected_compaction_id`. Before delivery the connector verifies a fresh native
`compact_context_request` with that exact UUID, current run/conversation,
`status: completed`, and idle activity/phase. Native input performs its own final
check. The relay never automatically chains a send. The client must preserve the
draft and stop a continuation if the draft, target or navigation changes, or
compaction fails, is cancelled, is unchanged or is uncertain. A confirmed clear
acknowledges the original identity; the client then refreshes the same run to find
the new conversation. A submitted clear is ambiguous: native inspect does not
export a request-correlated clear completion, so it must not authorize automatic
draft migration.

Synthetic context QA can use `fixture-config.json`:

```json
{
  "all_states": true,
  "compact_delay": 8,
  "compact_status": "completed",
  "clear_status": "confirmed",
  "session_usage": {
    "status": "ok", "source": "claude", "scope": "conversation",
    "context": {"used": 60000, "limit": 200000},
    "prompt_cache": {"status": "unknown", "source": "native_usage"}
  },
  "cache_hint": {"status": "cold", "source": "native_footer", "tokens": 60000}
}
```

Telemetry configuration applies to a fresh synthetic conversation. Compaction
progresses from submitted to compacting, then after `compact_delay` seconds
(default 2, maximum 60) to `compact_status`: `completed`, `failed`, `cancelled`,
`unchanged` or `uncertain`. Terminal completion shrinks reported context and warms
the synthetic cache. `clear_status` is `confirmed` by default, generating a new
conversation ID and empty history, or `submitted`, leaving the identity unchanged.
`context_supported: false` disables both controls. For uncertain receipt handling,
`context_failure_after_submit: true` records one synthetic mutation and exits
without acknowledging it. These controls never invoke real hgs or tmux.

## Local validation

```sh
PYTHONPATH=services/mobile .deps/mobile-venv/bin/python \
  -m unittest discover -s services/mobile/tests -v
```

Run from the repository root with an environment containing the pinned package.
For the Rust server, also run the contracts in [relay/README.md](relay/README.md)
on a disposable PostgreSQL server. Set `ZERUS_RELAY_BINARY` to its built
executable to run the released HTTP assertions against Rust, and
`ZERUS_RELAY_TEST_DATABASE_URL` to that server, with `tests/requirements.txt`
installed, for the connector HTTP suites; without the URL those suites skip.
Tests use temporary databases, synthetic sessions and loopback HTTP. No live
agent, tmux server or provider credentials are required.

Scoped lifecycle and Terminal operations require native help advertisement.
Terminal input uses an exact pane/process binding and a relay-issued five-second
deadline; its `submitted` receipt confirms bytes delivered, not command completion.
The connector never replays a claimed input UUID. Literal text is staged through
private tmux stdin buffers, and multiline input requires verified bracketed paste.
Terminal screen reads retain at most 128 KiB of screen text with explicit truncation;
finished relay reads expire after 120 seconds even if the phone abandoned its poll.
Connector read receipts expire 120 seconds after relay delivery; undelivered results
remain in the durable outbox. Input mutation tombstones keep the normal no-replay
retention. Terminal never resizes the desktop pane, and DeepSeek/archived/child
conversations report that raw Terminal is unavailable.

Synthetic fixture Terminal is available through the same fixed ABI and never
executes the supplied text. The fixture help advertises Terminal alongside scoped
lifecycle; `fixture_terminal_text` records only the synthetic screen content.

Terminal screen polling is capped at four frames per second, with slower idle
polling. Authenticated credential budgets are 2,400 requests per minute and are
workspace-scoped. Anonymous pairing and failed-login counters use separate
bounded state, so failed credentials do not debit authenticated quotas. Pairing and failed-login limits
retain their stricter independent budgets. These transport limits include receipt
polling and node result uploads; they do not permit replaying delayed input.

### Public history and session recovery

`history` is a scoped read operation with the original request UUID, expected run
and conversation, optional explicit archive/child identity, and a page limit up
to 100. Choose one opaque `before`, `after` or `around` cursor, or
`around_incoming_seq` together with the exact complete `history_epoch`.
Public journal/provider/attachment rows preserve their native identities and
canonical order. Equal timestamps remain distinct. History epochs survive a
confirmed same-conversation resume; source replacement, historical backfill or
an unavoidable canonical sequence change explicitly resets the epoch. Unknown
incoming totals remain null while indexing or when a source is partial.

The native derived SQLite index contains public projections only, uses private
files, and bounds each call to two MiB of provider input plus bounded journal,
receipt and reconciliation work. It caps each index at 100,000 rows/512 MiB;
exhaustion is disclosed as a source gap. Individual message text is bounded to
32 KiB with `detail_truncated`; whole pages fit the ordinary one-MiB transport
limit and their cursors advance only across returned rows. A valid final
unterminated source row is visible with partial status. An older resident DSH
adapter that cannot page its retained history reports partial availability,
unknown totals and `indexing:false`; no host reload is performed.

For a newest-head request the connector advances at most 32 bounded native reads
within two seconds, stopping at completion, permanent partial status or no
progress. Cursor reads execute once. History results use the existing one-hour
read retention and do not create permanent mutation tombstones.

`recovery_action` accepts only `job_id` and `action:now|cancel` in addition to
exact request/run/conversation identities. The native writer lock verifies the
current waiting job and its full five-field identity before changing it. Retry
now preserves `not_before`; `scheduled` means the job was rescheduled, not that
input was delivered. Durable UUID receipts prevent replay. Global recovery
policies, archived targets and child recovery controls are excluded.

`mark_read` targets one exact session and carries only `run_id`,
`conversation_id`, `reply_id` and `attention_signature` (`null` for a field that
is not being marked; the signature is lowercase hex SHA-256). The connector
advertises it only when native help lists `read <session> --json` and runs
`hgs read <session> --json` on the owning computer, locally or through the
guarded peer route. The result is the native answer: `{"ok": true, "applied":
…, "read": …}` or `{"ok": false, "error": "not_found"}`. Marks are idempotent
and forward-only on the host, so a phone may send a newer mark under a new
request ID. The operation is read-class, so the relay prunes it within the
read retention window like `inspect`. Within that window the journal replays
the receipt for an existing request ID; after pruning, a repeated ID may run
again, which is safe because marks are idempotent and forward-only.

`set_mobile_delivery` changes the swarm-wide phone notification timing. It is a
computer-scoped gateway request: the session is `""` and the payload is exactly
`{"value": ...}` with `immediate`, `away`, `away_5`, `away_10`, `away_15` or
`away_30`. The connector runs
`hgs swarm preference set mobile_delivery <value> --json` on its gateway computer
and returns the native `{"ok": true, "mobile_delivery": ...}` object; any other
answer is not a confirmation. A gateway connector advertises the operation for
its own computer only when native help lists that command; direct peers never
advertise it, and the relay returns 409 until the computer's snapshot does. The
operation is mutation-class: its request UUID is a permanent tombstone, so a
repeated ID returns the original receipt and never runs the command again, and an
error or unverified acknowledgement after the native command started is
uncertain. Replaying an old change could revert a newer choice, so every new
choice uses a new request ID.

Synthetic paging QA uses `history_count` up to 5,000 for history pages while
ordinary inspect remains capped at 500 fixture rows. Set `history_indexing:true`
for a stable incomplete head, `history_gap:true` for permanent partial source
status, or `history_epoch` to exercise epoch replacement. Configure `recovery`
as a native waiting job with `id`, `state`, `due_at`, `not_before`, `attempt`, and
`identity:[run_id,conversation_id,model,account_id,host_generation]`.

### Managed relay transport migration

Only the exact official HTTPS root origin `https://zerus.dev.guthub.dev`
(empty or `/` path and default/443 port, no credentials/query/fragment) transports
requests through `https://relay.zerus.dev`. Custom servers remain unchanged.
The connector hashes its original validated URL and node token for its private
receipt journal and fallback project identity. Neither is rekeyed by transport
migration, and claimed commands are never replayed.

To change an existing managed connector's stored `server_url` to the canonical
URL, explicitly retain its previous validated URL in `identity_url`. This override
is accepted only for the exact old-official-to-new-official pair (or an unchanged
identical URL). Preserve the node token and `state_dir`; retain the precise original
URL spelling if it included a default port. An arbitrary identity override is
rejected before opening the journal. A genuinely different relay requires a new
credential and separate private state directory, preserving the old receipts.
Saving configuration alone does not restart the connector or any native agents.
