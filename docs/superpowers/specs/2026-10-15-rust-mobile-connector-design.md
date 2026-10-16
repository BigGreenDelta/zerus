# Rust mobile connector and PostgreSQL-only relay

Status: approved direction, 2026-10-15. Tracking: epics `zerus-nbzl` (rewrite the
outbound mobile connector in Rust) and `zerus-3rqx` (PostgreSQL-only relay; delete
the Python reference relay and `migrate-sqlite`). This document is slice S0
(`zerus-nbzl.1`). Facts and line references were checked against `origin/main`
`a7d8bc4`. The plan behind it went through an adversarial review on 2026-10-15;
every amendment from that review is folded in.

Nothing here authorises a deploy, a live connector restart or a cutover. Each
slice is one reviewed, revertable pull request.

## Owner decisions

The owner decided on 2026-10-15:

- «переписывай мобильный коннектор на рст» ("rewrite the mobile connector in
  Rust").
- About the Python relay oracle and `migrate-sqlite`: «да, убирай -- зачем там
  вообще sqlite? разве мы не перешли на постргресс?» ("yes, remove it -- why is
  there SQLite at all? Didn't we move to PostgreSQL?").

Design choices delegated with those decisions and adopted here; the owner may veto
any of them:

- Deliberate divergences a-k ([Divergences](#deliberate-divergences-from-python))
  are adopted, with h respecified so a graceful stop never kills an hgs mutation.
- The Python connector stays supported until every owner gateway has run the Rust
  connector for 7 days without a rollback **and** one stable release has shipped
  the connector (the E2 gate).
- Frozen relay SQLite data on the production host is kept. SQLite self-hosters are
  pointed to `v0.42.2`, the last stable release with `migrate-sqlite`.
- The packaged `hgs-recovery` and `hgs-swarm` units get `KillMode=process` in K1
  (`zerus-nbzl.2`).

Open decisions are listed in [Open owner decisions](#open-owner-decisions).

## Goals and non-goals

Goals: a Rust `zerus-mobile-connector` replaces `zerus_mobile/connector.py`,
`fleet.py` and their helpers, and phones, relays and `hgs` cannot tell it apart
from Python except in the listed divergences; every [durable
contract](#durable-contracts-to-preserve) holds; it ships in the existing packages
without new release assets and without starting or restarting anything; the relay
becomes PostgreSQL-only without the Python reference relay, the Python PostgreSQL
store or `migrate-sqlite`; gateways cut over one at a time, with a rollback that
deletes one file, never stopping native agents, tmux servers or DeepSeek hosts.

Non-goals: Windows as a mobile gateway (the crate refuses non-Unix targets with
`compile_error!`); new protocol, operations or phone features; moving the
connector journal off SQLite (it is client state; only the relay drops SQLite);
changing the infrastructure repository's packager or the relay image layout.

Rejected: reproducing known Python defects first and fixing them later (Rust
would stall on purpose meanwhile; the divergence table replaces it), renaming the
user's unit file at cutover (a marked drop-in does it), deleting in-process Python
tests before Rust is proven (they stay until E2), and a `status` subcommand
(`--check` replaces it).

## Architecture

### Crate layout

```
services/mobile/
  relay/        zerus-relay. Unchanged paths, lock, Dockerfile and image.
                [features] default = ["server"]; server-only dependencies are
                optional. lib.rs always builds json, protocol, error and the
                private speech_config; alerts, config, db, http, push, registry
                and store need "server". error.rs keeps Error(http::StatusCode,
                &'static str); IntoResponse and From<sqlx::Error> need "server".
                [[bin]] zerus-relay has required-features = ["server"].
  connector/    zerus-mobile-connector: binary plus a library for tests. Own
                Cargo.lock, .cargo/config.toml and .gitignore (/target/).
                rust-version 1.85, edition 2021, unsafe_code = "forbid".
                protocol = { package = "zerus-relay", path = "../relay",
                             default-features = false }
  vectors/      Language-neutral golden vectors captured from Python.
  zerus_mobile/ The Python connector until E2. Server modules go in R1 and R6.
```

This leaves the relay image, `relay/Cargo.lock` and the deployment packager
untouched, keeps `arbitrary_precision` and the HTTP/TLS stack out of `hgs`, and
lets AUR builds fetch only connector dependencies. A Cargo workspace with a
separate protocol crate was rejected: it removes `relay/Cargo.lock`, which the
packager requires at that path, so relay builds would fail closed until the
infrastructure repository changed in lockstep. The cost is a third Dependabot
cargo directory and a partly duplicated HTTP/TLS stack. The same vectors run in
both crates to catch codec drift; a lock version-equality check would fail grouped
Dependabot PRs, so there is none.

The deployment packager takes only `relay/Cargo.toml`, `relay/Cargo.lock`,
`relay/.cargo/config.toml` and `relay/src/**/*.rs|*.sql` and skips any path
component named `tests`; `services/mobile/Dockerfile:3-5` copies the same set.
Every slice therefore keeps `relay/` free of `build.rs`, explicit `[[test]]` or
`[[bench]]` targets (a missing test file fails manifest loading inside Docker),
`include_str!`/`include_bytes!` in `src` of other files, and path dependencies
outside `relay/`.

### Shared protocol code

The connector uses the relay library's `json::parse`, `canonical` and `digest`
for `request_hash` and for all untrusted JSON (relay batches, hgs stdout, stored
journal rows), and its operation sets and shared limits. Each parse passes
explicit limits:

- Relay batches: at least depth 34 and 50,064 values. The relay admits a request
  at depth 32 with 50,000 values (`relay/src/http.rs:225-227`,
  `config.rs:72-73`), and `Claim::decode` (`store/requests.rs:22-34`) wraps it in
  `{"requests":[...]}` and adds `gateway_route` and `expires_at`. Parsing at the
  relay's own limits would reject a legitimate request before it runs and let its
  claim expire to uncertain.
- hgs stdout and journal rows: limits pinned by vectors, so nothing Python accepts
  within the byte caps is refused.

In P1 `encode` sorts object keys itself (output unchanged; CI rejects serde_json
`preserve_order`), and new `python_default_encode`/`python_default_len(value,
ensure_ascii)` (separators `", "` and `": "`) serve every bound Python measures
with plain `json.dumps`: `bound_inspection`, `MAX_RESULT`, `MAX_PAYLOAD` stdin,
the peer snapshot and the account and catalog projections.

The connector keeps its own validators with Python's phone-visible messages and
its own frozen `READ_SQL` text; the relay keeps its opaque `invalid request`
validators. Native stdin and journal text are compared semantically; only their
lengths must match Python.

### Connector runtime

- **Runtime.** tokio `current_thread`; requests run sequentially in claim order,
  as in Python. `spawn_blocking` only where Python uses `to_thread`: validation,
  attachment decoding and chunking, and JSON over 1 MiB.
- **HTTP client.** hyper 1, hyper-util `client-legacy`, hyper-rustls with `ring`
  and system roots (`SSL_CERT_FILE`/`SSL_CERT_DIR`, Keychain on macOS; none loaded
  fails closed). HTTP/1.1 only, no proxy environment, no redirects, 35 s per call,
  response caps of 8 MiB or 30,412,800 bytes for polls. The bearer token goes only
  to the `server_url` transport origin. No reqwest, url crate or IDNA (i).
- **HTTP request requirements**, imposed by the relay and the reference
  deployment, met by the client and enforced by FakeRelay:
  1. `Content-Type: application/json` on every body (`http.rs:161-171`, else 415).
  2. No `Content-Encoding` other than `identity` (`http.rs:428-435`).
  3. `Content-Length` on every POST; a body of unknown length takes the single
     large-payload slot (`http.rs:187-188`, `config.rs:71`) and gets 429 under load.
  4. Heartbeat and result bodies at most 1 MiB on the wire: the reference nginx
     sets no `client_max_body_size` on `/v1/`, so its 1 MiB default applies.
  5. No duplicate query parameters (`http.rs:470-475`, else 400).
- **Spawn gate, macOS only.** The macOS standard library marks pipes close-on-exec
  only after creating them, so one async mutex is held across every
  `Command::spawn`, like `hgs`'s `platform::spawn_in_turn`
  (`src/platform/unix.rs:233`). Linux uses `pipe2` and `SOCK_CLOEXEC` and has no
  gate. DNS is never resolved under the gate: a slow `getaddrinfo` would block
  every spawn, including `terminal_input` preflights inside their `(now, now+6]`
  window. A black-box test checks that children see only fds 0-2 and the default
  SIGPIPE disposition.
- **Journal.** rusqlite with `bundled`, like hgs; `journal.sqlite3` pre-created
  0600 with `O_NOFOLLOW`, sidecars checked as in Python; rollback journal
  (`DELETE`, never WAL), `synchronous=FULL`, `secure_delete=ON`,
  `busy_timeout=5000`. Each Python `with self.db:` becomes an explicit DEFERRED
  transaction; a claim commits before any native call. The schema stays frozen
  while a Python rollback is supported, then allows only additive nullable changes.
- **Lock.** fs2 exclusive non-blocking `flock(2)` on `connector.lock`, which
  excludes Python and is visible to `hgs status`. The effective uid comes from a
  safe library call (rustix or nix `geteuid`), not a tempfile probe, so `--check`
  creates nothing and works with a read-only or full `TMPDIR`.
- **Native layer.** `tokio::process::Command(hgs_path)` behind a `Native` trait:
  PATH lookup, the environment minus exactly the 23 `CONTEXT_KEYS`
  (`connector.py:46-52`), stdin always a pipe, capped output with stderr
  discarded, Python's exit-code and `failure_json` rules. A timeout or
  cancellation SIGKILLs only the direct child and drains its pipes for 0.5 s;
  `worktrees create --mobile` gets a detached 35 s drain. The `__state` prefix,
  peer envelopes and frozen help-marker table (re-probed every 300 s) stay.
- **CLI.** `--config PATH` or `--config=PATH` (required) and
  `--allow-insecure-localhost`, spelled exactly (divergence j); a usage error
  exits 2, as argparse does. `--version` prints `zerus-mobile-connector <crate
  version>`. A startup failure logs Python's fixed line "Connector could not
  start; verify private configuration and journal availability" and exits 1.
- **`--check [--json]`.** Read-only; creates nothing. One short `mode=ro` read
  transaction, never `integrity_check` or `quick_check`; a hot journal is reported,
  not recovered. It reports configuration validity and divergence findings (e, f,
  k), whether the binding matches `metadata.relay_binding`, the schema (current,
  or legacy plus the pending migration), row counts by state and delivery, and the
  lock state through a shared flock probe only when the lock file exists.
- **Signals (divergence h).** SIGTERM and SIGINT stop claiming. An in-flight
  native call runs to completion or to its own timeout, which kills it exactly as
  in normal operation; the stop path adds no kill and never SIGKILLs an hgs
  mutation child. The real outcome is journaled, a worktree creation drains for up
  to 35 s, and the process exits 0. Every per-call budget stays below systemd's
  default 90 s `TimeoutStopSec`, which a unit test asserts. A second signal exits
  at once without touching the child, leaving the row `running` for restart
  recovery: exactly Python's state after an abrupt stop.
- **Test seams.** The `Native` trait and an injected clock for unit tests; a
  non-default `test-hooks` feature lets black-box tests set the native timeout,
  output cap and clock scale through `ZERUS_CONNECTOR_TEST_*`. Packages never build
  it, and release binaries are checked for its absence.

## Durable contracts to preserve

### `connector.json`

Read from the `--config` path; the GUI and docs use
`$XDG_CONFIG_HOME/hgs/mobile/connector.json` (`~/.config/...` when unset). Keys as
`connector.py:253-283` reads them; unknown keys are ignored, and the GUI preserves
them (`tray/src/RelayConfiguration.h:96-103`):

| Key | Contract |
|---|---|
| `server_url` | Required. HTTPS, or loopback HTTP only with `--allow-insecure-localhost`. Managed relay URLs map to their transport origin. |
| `identity_url` | Optional. Keeps the spelling used by the binding hash after a managed-relay migration. |
| `node_token` | Required. Printable ASCII 33-126. Never logged. |
| `hgs_path` | Default `"hgs"` (PATH lookup), no NUL; the GUI writes `"hgs"` for new files. It is executed, hence divergence e. |
| `poll_interval` | Default 5; a finite number of at least 1 (divergence k for booleans). |
| `state_dir` | Default `~/.local/state/hgs/mobile-connector`; `null` and `""` mean the default; `~` and `~/...` expand (divergence f for `~user`). |

The binding is `sha256(identity_url + "\0" + node_token)` in hex
(`connector.py:281`), stored as `metadata.relay_binding`. A journal with another
binding is refused with Python's text; nothing is ever rebound.

### State directory and journal

- `state_dir` is an owned 0700 directory. `journal.sqlite3`, its `-journal`,
  `-wal` and `-shm` sidecars and `connector.lock` are owned regular 0600 files with
  one link, opened without following symlinks (`connector.py:108-152`).
- Schema: `metadata(key TEXT PRIMARY KEY, value TEXT NOT NULL)` and
  `requests(request_id TEXT PRIMARY KEY, operation, state, response, delivered,
  request_hash, created_at, updated_at, delivered_at, response_expired)`, with the
  exact DDL text, the legacy five-column upgrade and the six indexes of
  `connector.py:153-185`, `user_version` 0, no other objects, no VACUUM.
- On open, `running` rows become `uncertain` (mutations) or `failed` (reads) with
  "connector stopped before recording the native result".
- Retention (`connector.py:188-204`): 120 s after delivery for
  `terminal_snapshot`, `attachment_read` and the speech, speech_config and voice
  reads; 3,600 s for other reads; after 7 days a mutation's response becomes a
  `history_expired` tombstone ("Native result history expired; this request will
  not execute again") and the row stays.
- Claim (`connector.py:206-222`): an existing `request_id` with another operation
  or hash is a conflict; the same one replays the stored response, or "request is
  already in progress". Caps: 5,000 read-class rows, 100,000 others. `pending()`
  uploads at most 100 undelivered rows per flush.
- `request_hash` is the SHA-256 of `json.dumps(request, sort_keys=True,
  separators=(",", ":"), allow_nan=False)` over the request as received,
  `gateway_route` and `expires_at` included (`connector.py:807`). Rust reproduces
  it byte for byte; without `arbitrary_precision` about 45% of random requests
  diverge.

### Receipts and phone-visible text

- A receipt is `{"state", "result", "error"}` with Python's states and fixed
  texts, pinned by `vectors/receipts.json` and `errors.json`.
- Android matches substrings: `AttachmentViewCache.kt:115-121` turns an error
  containing "unavailable" or "unsupported operation" into "update required" and
  cuts other errors to 200 characters. `errors.json` marks such messages.
- `mobile_capabilities.operations` and `features` stay sorted, with the same
  feature names and help-marker mapping.

### Snapshot privacy

The heartbeat snapshot is `hgs ls --json --local` with a deny-list
(`connector.py:458-474`): `peers` and `ended_launches` are removed,
`process_work.sampled_at` is dropped from each row, and `desktop_presence` is sent
beside the snapshot, never inside it. Every other top-level key is forwarded,
including keys `hgs` adds later (`src/cli.rs:955-971`). Invariant for both
implementations: a heartbeat never contains `ended_launches`, `peers` or terminal
screen text. While both exist, a new strip lands in both, and the C2 snapshot
allowlist test catches drift.

### Names, paths and consumers

- Binary `zerus-mobile-connector` (the venv console script's name, so a cutover
  changes only the ExecStart path), unit `zerus-mobile-connector.service`,
  LaunchAgent label `com.hgdev.zerus-mobile-connector`.
- `hgs status` reads `connector.json` from the hgs config directory, resolves
  `state_dir` with the same default and `~/` rule, and probes `connector.lock` with
  a shared flock (`src/status.rs:131-144`).
- The GUI probes `systemctl --user show` (`tray/src/RelaySettings.h:136`), offers
  copyable start and restart commands (`:51-52`), and never runs them itself.
- Native argv and stdin per operation, the `CONTEXT_KEYS` rule and the peer
  envelopes are contracts with `hgs`, pinned by the C7 traces.

## Deliberate divergences from Python

Each divergence has a Rust-only black-box test, a row in
`services/mobile/connector/README.md` and an entry in the C7 differential
allowlist. Everything else is reproduced: error strings, retention windows, caps,
timeouts, and the connector's laxer send/answer/interrupt validation. No
divergence survives a rollback to Python.

| # | Python today | Rust |
|---|---|---|
| a | Archive-aware startup now probes before listing and claiming, matching Rust's initial wait. Flags still reset to false before the await, so a failed later probe disables everything for 300 s. | Claims wait for the first probe, at most 3 s. New flags are swapped in atomically; a failed probe keeps the previous ones. |
| b | One result the relay rejects (400/413/422) blocks every claim: `flush` re-raises it before polling (`connector.py:1226-1237`, `1277-1303`). | That row gets 3 attempts at least 30 s apart, then is marked delivered locally with the fixed log line "Relay rejected a stored result; preserving local outcome"; the relay's claim expires to uncertain anyway (`claim_ttl` 90 s). Other rows and polling never wait. New receipts are sanitised before journaling: a non-string error gets a fixed text, errors are cut to 4,096 code points, and the canonical receipt with `request_id` is at most 1 MiB. Stored rows are never rewritten. |
| c | Retired: since main's cc42fff9 a peer `attachment_read` reads the file through the peer callback with an output bound. It used to raise TypeError, giving "connector could not record native outcome". | The same, with a 14,046,552-byte cap; `test_bb_peer_operations` pins both. |
| d | Any 400/404/422 on the extended heartbeat disables gateway sharing until restart. | Re-probed after 300 s, like presence. |
| e | No checks on `connector.json`. | Refused if not owned by the effective uid or if group- or world-writable (`hgs_path` is executed); a warning only if group- or world-readable. `--check` reports it before cutover. |
| f | `state_dir` goes through `expanduser`, so `~user` works. | `~`, `~/...`, absolute and CWD-relative paths work; `~user` is refused with an explicit error and reported by `--check`. Neither the GUI nor the docs produce it. |
| g | A stored response holding NaN, Infinity or a lone surrogate re-uploads as invalid JSON and poisons the outbox. | Rows upload verbatim as stored text. A row the protocol parser rejects uploads as a substitute (uncertain for mutations, failed for reads, result null, fixed text); the local row is untouched. |
| h | No SIGTERM handler: the process dies, an in-flight `hgs` child finishes on its own, and the next start recovers `running` rows. | Stop claiming, let the in-flight native call finish within its own timeout, journal its real outcome, exit 0 ([Signals](#connector-runtime)). Never SIGKILL an hgs mutation child on a graceful stop; kill-on-timeout is unchanged. A second signal leaves Python's state. |
| i | aiohttp/yarl IDNA-encode a non-ASCII transport host. | A non-ASCII transport host is refused at startup. The GUI and managed URLs are ASCII. |
| j | argparse accepts prefix abbreviations such as `--conf` and `--allow` (`connector.py:1321-1324`). | Only the exact spellings; anything else is a usage error (exit 2). |
| k | `isinstance` and truthiness coercions: `poll_interval: true` passes the number check (`connector.py:273`); a falsy non-string `state_dir` (`false`, `0`, `[]`, `{}`) means the default. | Strict JSON types: a boolean `poll_interval` and a non-string, non-null `state_dir` are refused. `null` and `""` still mean the default. |

`config.jsonl` (P2) records Python's behaviour for i-k next to Rust's.

## Tests and the black-box seam

### Golden vectors

`services/mobile/vectors/`: synthetic data and reserved example domains only, at
most 64 KiB per file, a README with provenance and the freeze rule, and
`capture.py` with a `--check` mode. Captured from and checked against Python while
it exists, then frozen at E2 ("from Python at <commit>").

- `request_hash.jsonl`: every operation with and without `gateway_route` and
  `expires_at`; key order, Unicode and duplicate keys; numeric edge cases (`-0`,
  exponents, 1e16 and 1e-5, 5e-324, the largest double, integers above 2^64,
  4,300 vs 4,301 digits); every escape class and the serde_json magic key;
  rejects; a request at the relay's maximum inside its batch envelope.
- `identities.json`, and `config.jsonl` (exact accept/reject text, effective
  values, GUI-written files, `main()` exit codes and log lines, e, f and i-k).
- `journal/*.sql` (`.dump` text) with `journal_expect.json`: fresh, legacy and
  foreign-binding journals, `running` rows of each class, retention boundaries,
  tombstones, NULL hashes, rows at the caps, `table_info` and `index_xinfo`.
- `journal_traces.jsonl`, `receipts.json`, `sizes.jsonl` (cases that flip on one
  byte), `projections.jsonl`, `operations.json` (sets, `READ_SQL`, features, help
  markers), and `errors.json` (an AST catalogue of error literals, f-strings
  flagged, substring-sensitive messages marked).
- `validation.jsonl`: per `validate_request` call site, the connector decision and
  message, the relay-oracle decision and the source test. P3 adds the Rust relay
  column and moves the oracle column to `tests/relay_oracle.py`.

### Black-box harness

Python, with aiohttp as a test-only pin. `FakeRelay` has a real long-poll,
programmable status rules, capture of heartbeats, results, queries and
authorization, and enforces the five HTTP request requirements. `ScriptedHgs`
answers by argv, re-read on every call, and logs argv, stdin, environment keys and
open descriptors; it can `kill_parent`. `ConnectorProcess` runs
`ZERUS_CONNECTOR_BINARY`, or `python -m zerus_mobile.connector` by default,
refuses non-loopback URLs and scans stderr for secrets. Journal tools are
read-only while the connector runs and age or seed rows only while it is stopped.

`ported.json` lists the modules that must pass against Rust. `BLACKBOX.md` maps
every in-process test to a black-box test, a vector or a named Rust unit test,
including the connector halves inside relay test classes:
`AttachmentRelayTests.test_connector_accepts_large_request_response_independently`,
the Journal half of `TerminalRetention`, and `ContextConnectorTests` and
`ContextFixtureTests` (their module imports `Store`, `Config` and `create_app`).
Required cases beyond the ported suites:

- **Snapshot allowlist (C2, both modes).** Fails on any top-level snapshot key not
  on a reviewed list and scans every heartbeat for `ended_launches`, `peers` and
  screen text (the C4 journal key scan does not cover heartbeats).
- **`--check` while Python writes (C1).** An `OperationalError` after Python's 5 s
  busy timeout escapes `requests()` and `main()` (`connector.py:1277-1303`,
  `:1333`) and exits it, so the test proves `--check` never causes one.
- **Graceful stop (C2).** SIGTERM during a native mutation lets the child finish,
  journals its real outcome and exits 0; a second signal leaves the row `running`.

About 13 end-to-end tests run on the Rust relay with PostgreSQL; they are the only
tests that couple the two epics.

### CI

- Since PR #105 the relay job runs every `services/mobile/tests` module in one
  discover, with an explicit 18-minute budget (the timeout stays 25).
- C1 adds a `connector` job (ubuntu-24.04, pinned actions, no caches or secrets,
  `persist-credentials: false`): `cargo +1.85.0` fmt, clippy, test and `build
  --features test-hooks`, the hgs build, the black-box modules in both modes and
  the serde_json feature check. From then on the black-box modules run there, not
  in the relay job (C1 picks the mechanism and updates #105's assertion).
- Every new job is wired into the gate's `needs` (`checks.yml:844`) **and** its
  inline `selected` dict (`:858-869`; the gate checks only jobs listed there), and
  into `tests/test_ci_security.py`: the `needs` assertion, the `base` dict and
  result matrix (`:383-409`), and the jobs that depend on `source` (`:459-460`).
- The macOS smoke (C7) is its own `macos-15` job gated on `run_macos` and listed
  in `selected`, with a pinned test venv and `cargo +1.85.0`. The `cli` leg is
  unsuitable: it installs no aiohttp (`checks.yml:283-285`), uses stable
  (`:286`) and is already the long pole.
- Cached jobs that gain a connector build (`arch-package` in D1) hash
  `services/mobile/connector/Cargo.lock` into their keys (`checks.yml:447`); the
  pinned keys in `test_ci_security.py:604-616` follow, and the cache budget is
  reviewed against the 10 GiB limit. Free standard runners only; PR caches stay
  read-only.

## Distribution

- **Packages:** `/usr/bin/zerus-mobile-connector` in the existing pacman package,
  whose tree is also the Linux archive and the binary AUR packages (no new asset
  or feed kind; provides, depends and conflicts unchanged), and
  `Zerus.app/Contents/MacOS/zerus-mobile-connector`, signed with the other Mach-O
  files. Nothing ships on Windows.
- **Source installs:** `install.sh` installs `~/.local/bin/zerus-mobile-connector`
  through `replace_if_changed` and never enables, starts or restarts the service.
  Until E1 it builds only when `HGS_MOBILE_CONNECTOR=1` or that binary already
  exists (`HGS_MOBILE_CONNECTOR=0` always skips), so hgs development installs do
  not build hyper, rustls, ring and bundled SQLite before any gateway runs Rust;
  D6 or E2 revisits the default. `mac-install.sh` lists explicit files
  (`services/mobile/connector/{Cargo.toml,Cargo.lock,.cargo,src}`,
  `services/mobile/relay/{Cargo.toml,src}`) and never a `target/` (its own rule,
  `mac-install.sh:49`).
- **Packaged unit** `packaging/linux/zerus-mobile-connector.service`, never
  auto-enabled, with no `Environment=PATH` so `hgs` resolves as it does today:

  ```ini
  ExecStart=/usr/bin/zerus-mobile-connector --config %E/hgs/mobile/connector.json
  Restart=on-failure
  RestartSec=5
  UMask=0077
  NoNewPrivileges=true
  KillMode=process
  ```

- **LaunchAgent** `com.hgdev.zerus-mobile-connector`, opt-in through
  `zerus-setup --mobile-connector install`: `AbandonProcessGroup` true, `KeepAlive
  {SuccessfulExit: false}`, PATH captured at install, logs under
  `~/Library/Logs/hgs`, never reloaded implicitly. The cask `zap` lists
  (`packaging/homebrew/zerus.rb.in:19-32`) gain the plist and log but never
  `~/.local/state/hgs/mobile-connector` or `connector.json`: the journal holds
  no-replay tombstones.
- **Shared installer** `scripts/install-mobile-connector-service.py`, installed
  by the Linux package in `/usr/lib/zerus/` and reached through `zerus-setup
  --mobile-connector ACTION`. It never edits, moves or deletes user files and
  never starts, stops or restarts a unit.
  - `install` writes a user unit (source installs: absolute ExecStart, captured
    PATH, `KillMode=process`) or the plist, only when absent or ours.
  - `protect` writes nothing when the effective `KillMode` is already `process`
    from any drop-in, an administrator's own included. Otherwise it writes only
    `50-zerus-killmode.conf` (`[Service]`, `KillMode=process`) and runs
    `daemon-reload`, which restarts nothing.
  - `migrate` refuses while `connector.lock` is held and proceeds only for the
    documented effective ExecStart shape `[<venv>/bin/zerus-mobile-connector,
    --config, PATH]`. It writes only `60-zerus-rust-connector.conf` (marker
    comment, `ExecStart=`, the new `ExecStart=` with the same `--config`,
    `KillMode=process`), reloads and re-reads the effective ExecStart; if a
    later-sorting drop-in such as a `systemctl --user edit` `override.conf` keeps
    another in effect, it removes its drop-in, reloads and refuses. With both
    `/usr/bin` and `~/.local/bin` binaries present it needs `--binary PATH`.
  - `rollback` removes only its own `60-` file and reloads.
- `scripts/check-installed-package.py:33` keeps asserting that plain `zerus-setup`
  creates no `~/.config/systemd`; D4 extends it so each `--mobile-connector`
  action writes only its named file and never enables or starts a unit.

## PostgreSQL-only relay

- `database_url` is required. Without it startup exits 2 with "zerus-relay
  requires database_url (PostgreSQL); the SQLite backend was removed", before
  touching any file. `--database` stays hidden, accepted and ignored with a notice.
- `ENTRYPOINT ["zerus-relay"]`. The deployment passes only `serve ... --config`,
  and its exec-form healthcheck bypasses ENTRYPOINT.
- Compose defaults to the PostgreSQL services; the SQLite `relay` service and the
  `relay-data` volume go; Caddy's `RELAY_HOST` default becomes `relay-postgres`.
- `relay/src/postgres.sql` stays byte-identical, so older images roll back safely.
- `v0.42.2` is the last stable release with `migrate-sqlite` (its tag contains
  `zerus_mobile/migration.py`). A SQLite self-hoster installs that tag's Python
  package with asyncpg in a venv, migrates, then moves to the PostgreSQL-only
  relay, and never runs `docker compose down -v`.

## Slices

Sizes are engineer-days. "After C1" means the slice can be written earlier but
merges once the connector job exists.

| ID | Title | Epic | Depends on | Size |
|---|---|---|---|---|
| S0 | This design (`zerus-nbzl.1`) | nbzl | — | XS |
| A0 | All mobile modules in hosted CI (PR #105) | both | — | merged |
| K1 | `KillMode=process`: connector docs, packaged worker units, installer templates (`zerus-nbzl.2`) | nbzl | — | S 1 |
| G1 | Early GUI KillMode probe and warning (may fold into K1) | nbzl | — | S 0.5 |
| E0 | Protect and refresh live Python gateways (operational, decision 0) | nbzl | K1 text | ops |
| P1 | Relay library split (`server` feature) and codec hardening | both | — | S-M 1.5 |
| P2 | Golden vectors captured from Python | both | — | M 2.5 |
| H1 | Black-box harness and core suites (Python mode) | nbzl | — | M-L 3.5 |
| R1 | Delete `migrate-sqlite` and the Python PostgreSQL relay | 3rqx | — | S 1 |
| R2 | Rust relay on PostgreSQL in every fixture | both | P1 | M 3 |
| P3 | Relay consumes the vectors; test-only oracle bridge | both | P1, P2, R2 | S-M 1.5 |
| R3 | Rewrite `test_rust_relay.py` without the Python Store | 3rqx | R2 | M 2 |
| R4 | Rust contracts for oracle-only behaviour; move connector halves | 3rqx | R2, H1 | M 2.5 |
| R5 | PostgreSQL-only Rust relay | 3rqx | P1, R2, R3 | M 2.5 |
| R6 | Delete the Python reference relay | 3rqx | R1, R3, R4, P3 | S-M 1.5 |
| R7 | Infrastructure: drop the Python runtime and SQLite branches | 3rqx | R5, R6 | S-M 1.5 |
| H2 | Session-operation black-box suites | nbzl | H1, P2; after C1 | M 2.5 |
| H3 | Computer, read and media black-box suites | nbzl | H1, P2; after C1 | L 3.5 |
| H4 | Peer and fleet black-box suites | nbzl | H1; after C1 | M 2 |
| C1 | Connector crate: config, journal, lock, `--check`, CI job | nbzl | P1, P2, H1 | M 3 |
| C2 | Runtime core, HTTP, native layer, read operations | nbzl | C1 | L 5.5 |
| C3 | Session mutations and context operations | nbzl | C2, H2 | L 4.5 |
| C4 | Terminal, history, attachments, voice, speech, speech_config | nbzl | C2, H2, H3 | L 4.5 |
| C5 | Launch, directories, worktrees, projects, accounts, attention, archives | nbzl | C2, H3 | L 4.5 |
| C6 | One-hop gateway and peers | nbzl | C3, C4, C5, H4 | M-L 3.5 |
| C7 | Parity gate | nbzl | C6, R2 | M 2.5 |
| D1 | Linux packaging, unit, AUR, licenses | nbzl | C7 | M 2.5 |
| D2 | macOS bundle, LaunchAgent, shared installer (`install`) | nbzl | C7 | M 2.5 |
| D3 | `install.sh` and `mac-install.sh` | nbzl | C7 | S 1 |
| D4 | Cutover tooling: `protect`, `migrate`, `rollback` | nbzl | D1, D2 | M 2 |
| D5 | GUI status and instructions | nbzl | D1, D2 | S-M 1.5 |
| D6 | Docs, runbook, `AGENTS.md`/`CLAUDE.md` | nbzl | D2, D3, D4, D5 | S-M 1.5 |
| E1 | Per-gateway cutover and 7-day soak (operational, owner-approved) | nbzl | D6 | S + soak |
| E2 | Delete the Python connector | nbzl | E1, R6 | S-M 1.5 |
| O1 | Optional `hgs mobile-abi --json` with help-text fallback | nbzl | C7 | S-M 1 (deferred) |

About 76 engineer-days. Critical path: H1 → C1 → C2 → {C3, C4, C5} → C6 → C7 →
{D1, D2} → D4 → D6 → E1, about 30 working days plus the soak. The relay track
(R1/R2 → R3/R4 → R5/R6 → R7, about 11 days) runs in parallel.

### Slice scope and acceptance

Every slice passes `python3 scripts/ci/check-source.py`, builds with Rust 1.85.0
where it touches Rust, validates workflow edits with actionlint and
`tests/test_ci_security.py`, and records relay-job time it adds. The sections
above define behaviour; this list names what each slice adds and what proves it.

**K1. KillMode protection** (`zerus-nbzl.2`). The unit in
`docs/mobile-deployment.md:151-175` gains `KillMode=process`, plus a "Protect
running agents" paragraph for existing units (drop-in, `daemon-reload`, verify),
so "native agents keep running" becomes true. `hgs-recovery.service`,
`hgs-swarm.service` and their installer templates gain it too: only
`hgs-tray.service` sets it today, `install.sh:53-62` restarts both workers when
hgs changes, and the recovery worker resumes agents. `docs/ci-and-aur.md:1066`,
which says packaged units already use it, is corrected. *Accept:* a recorded
rehearsal with a transient `systemd-run --user` unit starting tmux on an isolated
socket (`tmux -S <scratch dir>/sock` or `TMUX_TMPDIR`; `-L` only names a socket in
the shared default directory): the server dies on stop under `control-group` and
survives under `process`. No live connector or tmux server is touched.

**G1. Early GUI warning.** The GUI offers every Linux user a copyable restart
command (`RelaySettings.h:52`) while the documented unit has no KillMode. The
existing bounded probe adds `KillMode`; when it is not `process`, the panel warns
and points to the protection paragraph instead of offering the restart.
*Accept:* fake-systemctl cases in `tray/tests/test_relaysettings.cpp`; every
QProcess goes through `ChildProcess::inheritStdioOnly`.

**E0. Protect and refresh live gateways.** Operational; the steps and rationale
are [decision 0](#open-owner-decisions). `fbd85d4` is "Keep ended launches out of
the mobile snapshot" (`ended_launches`, `src/cli.rs:970`).

**P1. Relay library split and codec hardening.** The `server` feature and gating,
the `json.rs` changes, and `relay/tests/codec.rs`: goldens (`{"b":1,"a":2}`,
`18446744073709551616`, `-0`, the magic key, every escape class) and a seeded
20,000-case differential of `python_default_len` against CPython. The relay job
adds `clippy` and `build` with `--no-default-features --lib` and the serde_json
feature check. *Accept:* relay tests unchanged; no axum, sqlx, reqwest, tokio or
hyper without default features; Dockerfile unchanged and `docker build
services/mobile` passes; the lock gains only the `http` edge; `relay/` constraints
hold.

**P2. Golden vectors.** `services/mobile/vectors/**` and
`services/mobile/tests/test_vectors.py` (recapture in memory and compare).
*Accept:* `capture.py --check` reproduces every file byte for byte; the journal
fixtures load with Python's `sqlite3`, and the Python Journal reproduces
`journal_expect.json`.

**H1. Harness and core suites.** New files only: `connector_harness.py`,
`scripted_hgs.py`, `ported.json` (`{"rust": []}`), `BLACKBOX.md`,
`test_bb_connector.py` (about 17 core cases, today's poison pill as Python-only),
`test_bb_retention.py`, `test_bb_journal_fs.py` (modes, links, sidecars, a second
instance, `hgs status` with an isolated `HGS_CONFIG_DIR`) and
`test_bb_fd_hygiene.py`. *Accept:* all pass against Python; a self-test shows
long-poll wake under 50 ms and every status rule firing; a socket guard proves no
non-loopback connection; real-hgs tests fail rather than skip when `CI=true`.

**R1. Delete `migrate-sqlite` and the Python PostgreSQL relay.** Delete
`migration.py`, `postgres.py`, `postgres_schema.py`, the subcommand, the
`PostgresStore` branch of `open_store` (`__main__.py:76-87`), asyncpg and the
modules that test them, after porting the revoke-computer-by-alias case to a Rust
contract. Docs point SQLite self-hosters to `v0.42.2`. *Accept:* `git grep -e
migrate-sqlite -e migrate_sqlite -e asyncpg -e PostgresStore -- services docs
CONTRIBUTING.md` finds only historical notes; the new contract passes.

**R2. Rust relay on PostgreSQL in every fixture.** `relay_fixture.start_relay`
creates a `relay_test_<uuid>` database (asyncpg pinned only in a new
`services/mobile/tests/requirements.txt`), seeds through the `zerus-relay` CLI and
drops the database in `finally`; the end-to-end, peer, presence, directory and
speech HTTP suites use it without direct database reads. Rust fixtures in
`relay/tests/common/mod.rs` use per-test databases from
`ZERUS_RELAY_TEST_DATABASE_URL`, which CI sets (absorbing `ZERUS_RELAY_TEST_DSN`)
with `ZERUS_REQUIRE_RELAY_E2E=1`. `CONTRIBUTING.md:112-117` and `relay/README.md`
note that these suites need PostgreSQL (plain discover skips them) and give a
disposable `docker run postgres:17.11-alpine` line. *Accept:* every async
contract runs on PostgreSQL in CI; the switched suites run with zero skips.

**P3. Vectors in the relay; oracle bridge.** `relay/tests/vectors.rs` checks
`request_hash`, the operation sets and the relay column of `validation.jsonl`
(`ZERUS_BLESS=1` regenerates only that column).
`services/mobile/tests/relay_oracle.py` is a verbatim copy of
`server.validate_request`; connector tests and `capture.py` move to it. *Accept:*
nothing outside the relay modules imports `zerus_mobile.server`; every
bridge/relay disagreement is a listed row with a Beads issue; the `relay/`
constraints hold.

**R3. `test_rust_relay.py` without the Python Store.** The resulting
`test_relay_http.py` keeps the HTTP-only and admin cases seeded through the CLI;
time-travel cases move to `relay/tests/maintenance_contracts.rs`; the Python
receipt identity case becomes a PostgreSQL SQL fixture with Python-computed
`body_hash` values. *Accept:* no import of `zerus_mobile.server`, `store` or
`test_relay`; a mapping table for every borrowed method in `relay/README.md`.

**R4. Oracle-only contracts and connector halves.**
`relay/tests/operation_contracts.rs` adds an accept and a reject for each
operation only the oracle covers (context commands, send_now, settings,
recovery_action, process output and stop, worktree_create, terminal window, budget
and retention, attachment budgets, archive_id). The connector halves listed under
the harness move to the R2 fixture with `BLACKBOX.md` rows (the Journal half of
`TerminalRetention` split out first). *Accept:* the mapping names a passing Rust
test for every test in `test_relay.py` (including the 5 `test_rust_relay.py` does
not borrow) and `test_relay_peers.py`; no connector test needs `Store`, `Config`
or `create_app`; differences become contract questions in Beads.

**R5. PostgreSQL-only Rust relay.** Remove sqlx `sqlite`, `src/sqlite.sql`, every
SQLite branch and the SQLite startup special case; hide `--database`; fixtures and
`load.py` always use PostgreSQL; ENTRYPOINT `["zerus-relay"]`; compose, Caddy and
docs move to PostgreSQL. *Accept:* no `libsqlite3-sys` in the tree; `git grep -i
sqlite services/mobile/relay/src` is empty; `postgres.sql` unchanged; no
`database_url` exits 2 and creates no file; the image started with the
deployment's exact argv passes `zerus-relay healthcheck` and `zerus-relay check
--config`; the load profiles stay within 256 MiB.

**R6. Delete the Python reference relay.** Delete `server.py`, `store.py`,
`async_store.py`, `ingress.py`, `push.py`, `routes.py`, `work.py`, `__main__.py`,
the `zerus-mobile` console script (the Docker alias stays), the `fcm` extra, the
relay-only test modules and classes, the `create_app` fixture branch and the
speech rerun against Python; remove the Python reference and rollback notes from
`CONTRIBUTING.md:112-117`, the READMEs and `docs/relay-architecture.md`. *Accept:*
`import zerus_mobile.connector` works with only aiohttp and `import
zerus_mobile.server` fails; `test_vectors.py` still passes.

**R7. Infrastructure cleanup** (infrastructure repository, after R5 and R6):
remove the Python runtime and SQLite branches, default to PostgreSQL and refuse
SQLite; keep backup gating, the retired-host refusal and all frozen data.
*Accept:* infra tests pass; a check run proposes no production container change.

**H2. Session-operation suites.** Black-box modules for actions, controls,
context, child_send, delivery, read, history_recovery, voice and attachment send,
with rejections checked through failed receipts and the vector message.

**H3. Computer, read and media suites.** Terminal, history, attachment_read, the
connector parts of speech and speech_config, launch, project_launch, projects,
accounts, worktrees (with a SIGTERM drain variant), directory_create and archives.

**H4. Peer and fleet suites.** About 15 `test_connector_peers` cases, the peer
parts of directory_create, worktrees and delivery, and a black-box version of the
root `tests/test_mobile_peers.py` connector case.

*Accept (H2-H4):* all modules pass against Python; real-hgs and owned-transport
tests run unskipped in hosted CI; no new module imports server, store or
`create_app`; every in-process test of the area is mapped in `BLACKBOX.md`.

**C1. Connector crate foundation.** `services/mobile/connector/` with manifest,
lock, `.cargo/config.toml` and `.gitignore` (`/target/`; the root file ignores
only `/target/`, so the privacy guard and `prepare-release.py` would otherwise see
build output); config, URL splitting, journal, filesystem guards, lock, `main.rs`
(`--check`, `--version`; run mode exits 1 until C2) and `test-hooks`; Dependabot;
the `connector` job. *Accept:* every identities, config, journal and trace vector
passes as a unit test; flock excludes in both directions and `hgs status` reports
running while Rust holds it; a Rust-written journal reopens under Python with
equal schema dumps; `--check` leaves bytes, mtime and inode unchanged on a legacy
fixture and on a journal a running Python connector holds, and never makes it
fail; the lock has no axum, sqlx, reqwest, openssl-sys or native-tls.

**C2. Runtime core.** HTTP client, spawn gate, native layer; heartbeat, request
and flush loops with Python's backoffs, presence fallback, 300 s re-probe,
`wait=1|25` and 0.25 s idle sleep; snapshot projection and capabilities; the
execute pipeline; inspect, process_output and mark_read; signals; divergences a,
b, g and h. *Accept:* core, retention, journal_fs, fd_hygiene and snapshot
allowlist suites pass in both modes and enter `ported.json`; TLS works only with
`SSL_CERT_FILE` naming a throwaway CA; proxy variables aimed at a trap listener
get no connection; a 3xx is not followed and the token never reaches the redirect
host; FakeRelay's request requirements and the graceful-stop cases pass; no token
or URL on stderr; no `ZERUS_CONNECTOR_TEST_` in a release build.

**C3. Session mutations.** send (inline attachments), answer, interrupt,
send_now, settings, the context commands and their preflight, the DeepSeek
child-send preflight, process_stop, lifecycle operations with `result_target`,
recovery_action and set_mobile_delivery.

**C4. Terminal, history, attachments, voice and speech.** terminal_input's
`(now, now+6]` window and deadline recheck; history re-reads within 2 s;
attachment_read (14,046,552-byte cap, off-thread decoding, 30 MiB / 120 s LRU,
512 KiB chunks); speech_config with `failure_json` and strict allowlists.

**C5. Launch, directories, worktrees, projects, accounts, attention, archives.**
directory_create's exact receipt, the shielded 35 s worktree_create drain, launch
preflights and 5 s verification, and Python's cache lifetimes (projects 15 s,
accounts 30/300 s single-flight, attention 2 per poll, archives 45 s).

**C6. Gateway and peers.** Discovery (12 s, 128 KiB, at most 32 peers,
fail-closed), uuid5 `route_id`, per-route contexts sharing the journal, a
semaphore of 4, the 1 MiB peer snapshot, 405 handling, `gateway_one_hop`, the
`__mobile-peer-local` guard; the peer `attachment_read` (retired divergence c)
and divergence d.

*Accept (C3-C6):* the matching modules pass against Rust (C3: H2; C4: H2 and H3,
with the real-CLI speech roundtrips and the journal key-leakage scan; C5: H3 with
`ZERUS_DIRECTORY_TEST_HGS`, and a SIGTERM during worktree creation drains to
completed or uncertain; C6: H4 and `test_peer_end_to_end`, with route ids equal to
the vectors and owned transports reaped); argv, parsed stdin and Python-default
byte lengths match.

**C7. Parity gate.** `ported.json` equals every black-box module. Differential
traces compare native argv, parsed stdin, environment keys, parsed uploads and
heartbeats and final journal rows; the allowlist covers only key order,
whitespace, log text and divergences a-k. A journal handoff matrix Python → Rust
→ Python (SIGKILL mid-mutation, replays, conflicts, tombstones, outbox re-upload,
binding mismatch) runs against main's Python and every venv recorded as a
rollback baseline. Also: end-to-end with both connectors, a 5-minute soak
(`poll_interval` 1, 4 fps terminal polling) with flat RSS and descriptors, the
macOS job, and `services/mobile/connector/README.md`. *Accept:* zero unexpected
differences; the allowlist is committed and reviewed.

**D1. Linux packaging.** `package-linux.py --connector`; `check-package.py`
requires the binary and unit, records `--version`, and checks the test-hooks
string is absent, NEEDED ⊆ {libc, libm, libgcc_s, ld-linux-x86-64} and
`KillMode=process`; the PKGBUILDs fetch, build (`--frozen --release`), test and
package it with unchanged makedepends; licenses cover both manifests; the
arch-package cache key gains the connector lock. *Accept:* arch and arch-package
jobs and the nightly and release validators pass with unchanged asset sets.

**D2. macOS bundle, LaunchAgent, installer.** `macos-bundle.sh` builds the
connector (`MACOSX_DEPLOYMENT_TARGET=14.0`); packaging, bundle checks and signing
cover it; the installer's `install` action, `zerus-setup --mobile-connector` with
`--remove`, and the cask `zap` lists. *Accept:* fake HOME, systemctl and
launchctl tests cover absent, ours, foreign and loaded-and-changed definitions
with no start, stop or restart; a stopped LaunchAgent leaves a fixture descendant
alive; `dirs` and `worktrees` against `~/Documents`, `~/Desktop` and
`~/Downloads` are rehearsed under the LaunchAgent, because leaving Terminal for
Zerus.app changes TCC attribution. Merge when no sealed candidate retry is
pending: cask template edits change its rendering.

**D3. Source installs.** `install.sh` and `mac-install.sh` as described under
Distribution, with a "restart after draining" hint when the binary changed while
the unit or agent is active. *Accept:* fake cargo and systemctl tests cover
build-and-install, skip and opt-in paths with no start or restart call.

**D4. Cutover tooling.** `protect`, `migrate`, `rollback` and `zerus-setup
--mobile-connector {protect,migrate,rollback}`. *Accept:* fake systemctl and
lock-holder tests cover a foreign or unexpected unit, an administrator drop-in
(protect writes nothing), idempotent protect, migrate refused while locked, a
later-sorting override (migrate removes its drop-in and refuses), both binaries
present without `--binary`, and rollback removing only its drop-in; the extended
installed-package check passes.

**D5. GUI.** The Linux probe adds `UnitFileState`, `FragmentPath`,
`DropInPaths`, `ExecStart` and `KillMode` within the existing 3 s and 16 KiB
bounds; a missing `connector.json` shows "not configured" first; the panel labels
the Rust or legacy Python implementation, with a migration hint for a venv. macOS
gets a bounded `launchctl print` probe, whose QProcess calls
`ChildProcess::inheritStdioOnly` in its creating function before start, and copy
commands for `install` and `launchctl kickstart -k`. *Accept:* fake systemctl and
launchctl cases pass in the arch, mac-desktop and windows-desktop jobs.

**D6. Docs.** "Connect a computer" in `docs/mobile-deployment.md` (package,
Zerus.app or `install.sh`; Settings; enabling the unit or agent) with the cutover
and rollback runbook below; installation, distribution, CI, CONTRIBUTING and
README updates; `AGENTS.md` and `CLAUDE.md` identically. *Accept:* the migration
commands are rehearsed verbatim under a disposable user account with a systemd
user instance, summarised without credentials in the PR.

**E1. Per-gateway cutover and soak.** The runbook on each owner-approved gateway,
first one without peers, then 7 days on a numbered Dev or nightly build. Beads
records only sanitized pass/fail items: raw `--check --json`, `systemctl --user
cat`, `systemd-cgls` and `hgs ls --json --local` output holds home paths, relay
URLs, the binding hash, session names and PIDs, and stays local. *Accept:* clean
`--check` before the switch, every step 7 check, and 7 days without unexpected
exits or "Rejected relay request" and "could not start" lines.

**E2. Delete the Python connector**: `services/mobile/zerus_mobile`,
`pyproject.toml`, `requirements.txt`, the mapped in-process tests, the oracle
bridge, the harness's Python mode and `ported.json`; freeze the vectors; docs name
the last commit with Python. *Accept:* `git grep zerus_mobile` finds only
historical notes; all suites pass against Rust; handoff tests still open
Python-written journals (frozen SQL fixtures); the PR cites the E2 gate and each
E1 record.

**O1. Optional `hgs mobile-abi --json`**, mapping one-to-one onto today's help
markers and allowlisted for peers (`src/mobile_peers.rs`); the help table stays as
a permanent fallback for older peers. *Accept:* cli job on all three platforms.

## Merge order

CI stays green and every step stays reversible.

1. Wave 1, now and file-disjoint: S0, K1, P1, P2, H1, R1. G1 depends on nothing
   and can join any wave; E0 runs once the owner approves decision 0.
2. R2 after P1 merges (both edit the relay job and relay test layout); then P3,
   R3 and R4.
3. C1 → C2 → {C3, C4, C5} → C6 → C7, with H2-H4 merging after C1; R5, R6 and R7
   in parallel.
4. D1, D2, D3 → D4 and D5 → D6 → E1 → E2.

Merging never switches a live connector: a user unit shadows the packaged one,
installs and upgrades never enable or restart anything, the LaunchAgent is opt-in.

## Cutover

**Relay.** Production already runs the Rust relay on PostgreSQL, so no data
moves. The R5 image ships only at the next owner-approved deploy (drain, verified
backup, both workers from one reviewed image); the ENTRYPOINT change fits the
deployment as it is, and the frozen SQLite file stays mounted and unused.

**Per Linux gateway**, one machine at a time, owner-approved. `connector.json` and
`state_dir` are read in place and never copied, reset or rebound.

0. **Install** a build whose Rust binary passed C7 (package update, Zerus.app or
   `install.sh`). Keep the venv. Run `zerus-mobile-connector --version`.
1. **Protect agents:** `zerus-setup --mobile-connector protect` (packaged) or the
   installer script (source); verify `KillMode=process`. Never stop the unit or
   run `--check` before this passes.
2. **Record, read-only:** the unit (`systemctl --user cat`, `show -p
   ExecStart,KillMode,FragmentPath,DropInPaths`), its cgroup, tmux and agent PIDs,
   `hgs status`, `--check --json`, and the venv's version (`pip show` or file
   hashes). If C7's handoff tests did not cover that version, reinstall the venv
   from the Rust build's commit first, so the rollback target is tested. If
   `--check` refuses the config (e, f, k), fix it first.
3. **Drain:** pause phone actions for this computer and repeat `--check` until no
   rows are `running`. Undelivered rows need not drain.
4. **Stop only the connector:** `systemctl --user stop
   zerus-mobile-connector.service`; `hgs status` shows it stopped and the lock is
   free. Agents, tmux and DeepSeek hosts keep running.
5. **Switch** with `migrate`. Without a prior user unit, enable the packaged or
   source-installed unit instead.
6. **Start** the unit.
7. **Verify:** effective ExecStart and KillMode; `hgs status` running; same
   binding and counts in `--check` apart from pruning; no startup errors in the
   unit's journal; the phone shows the computer online with the same capabilities
   and peers (route ids are deterministic); one inspect and one disposable send,
   never to an unrelated live agent; tmux and agent PIDs match step 2; reopen
   phone actions. Early 429s while the old poll lease expires (`wait + 10` s, two
   polls per credential: `http.rs:370`, `config.rs:74`) and an offline blip past
   `online_timeout` (45 s, `config.rs:62`) are not failures.
8. **Record** sanitized pass/fail items in Beads. Keep the venv until E2.

**macOS gateway.** No Python LaunchAgent was ever documented. Drain; stop the
foreground Python connector; run `zerus-setup --mobile-connector install`, then
the printed `launchctl bootstrap gui/$(id -u)
~/Library/LaunchAgents/com.hgdev.zerus-mobile-connector.plist`; steps 2, 3 and 7
apply unchanged.

**Invariants.** Exactly one implementation holds the flock on one `state_dir`;
never run both against the same credential with different state directories.
Request hashes are byte-identical, so a re-delivered request in flight replays
instead of conflicting. Uncertain receipts stay uncertain. Nothing restarts native
agents, tmux servers or DeepSeek hosts.

## Rollback

**Connector, before E2, per machine:** drain; stop the unit (agents survive under
`KillMode=process`); `rollback` removes only `60-zerus-rust-connector.conf` and
reloads, keeping `50-zerus-killmode.conf`; start the unit and verify as in step 7.
Python opens the same journal in place: schema, pragmas and journal mode are
frozen, and the C1/C7 handoff tests, run against the venv recorded at step 2,
prove it reads what Rust wrote. A machine that started on Rust rolls back to the
previous package. Divergences a-k do not survive; the outbox stall (b) can return.

**Connector, after E2:** downgrade the package (pacman cache, a retained immutable
nightly asset, the previous cask) or build `install.sh` from an older revision.
Last resort: a Python venv from the recorded last-Python commit, which reads the
journal because only additive nullable schema changes are allowed.

**Rules for every rollback:** never copy, restore, clear or rebind
`journal.sqlite3` (an older copy loses no-replay tombstones); never resend an
uncertain mutation automatically; never kill native agents, tmux or DeepSeek
hosts.

**Code:** P1 reverts behaviour-neutrally, R5 without touching data, R1 and R6
restore files nothing in production uses; D1 and D4 revert together, because a
drop-in pointing at a removed binary fails to start. **Relay:** redeploy the
previous reviewed Rust PostgreSQL image on the same database (`postgres.sql` is
unchanged; older images keep their `--database` ENTRYPOINT and use
`database_url`). After R6 and R7 there is no Python relay target, and the frozen
SQLite copy is never activated.

## Open owner decisions

0. **Protect and refresh live gateways now (E0).** On every gateway running the
   Python connector, add the protection drop-in and `daemon-reload` (no restart),
   then upgrade the venv to current main (which includes `fbd85d4`) in one drained
   restart and record that commit as the pre-cutover rollback baseline. A venv
   older than `fbd85d4` forwards failed-launch screen text in heartbeats, and a
   unit without `KillMode=process` kills phone-launched tmux servers on restart.
1. **Live cutovers (E1).** Which gateways and when. Approval covers the drain,
   `protect`, stopping the Python unit, `migrate` and the disposable-session
   check; agents never stop, switch or restart a live connector without it.
2. **Deprecation notice.** Whether known venv self-hosters get one ahead of E2.
3. **Vetoes** of any delegated decision above: individual divergences a-k, the E2
   gate, keeping the frozen SQLite data, pointing SQLite self-hosters to `v0.42.2`.

## Watch items

- **Dependabot:** three cargo directories after C1; grouped PRs may differ per
  directory, and the shared vectors catch codec drift.
- **CI time and caches:** the relay job's 18-minute budget, the new connector
  jobs, and the 10 GiB cache limit as cached jobs gain connector builds.
- **Rust 1.85:** hyper-util and hyper-rustls declare 1.85 today; the MSRV-aware
  resolver will hold back future fixes quietly.
- **macOS:** Keychain trust differs slightly from OpenSSL (C2 TLS test, C7 smoke);
  TCC attribution under the LaunchAgent (D2). A resolver descriptor created on a
  blocking thread is outside the spawn gate; if the fd hygiene test ever sees one
  leak, resolve on the runtime thread or cache addresses with a bounded timeout
  rather than holding the gate.
- **Two implementations until E2:** snapshot strips, substring-sensitive error
  texts and relay limit changes must land in both; the snapshot allowlist,
  `errors.json`, the relay-maximum vector and the C7 traces catch drift.
