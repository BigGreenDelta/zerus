# Rust mobile connector

`zerus-mobile-connector` will replace the Python connector in
`services/mobile/zerus_mobile` (design:
`docs/superpowers/specs/2026-10-15-rust-mobile-connector-design.md`). Phones,
relays and `hgs` must not be able to tell the two apart, except in the
deliberate divergences below.

This build runs the relay runtime with the read operations and the session
mutations: it heartbeats the projected native listing, long-polls the relay,
runs `inspect`, `process_output`, `mark_read`, `history`, `catalog`, `dirs` and
`worktrees`, and the mutations of `src/mutations.rs` (send with inline
attachments, answer, interrupt, send_now, settings, the context commands with
their preflight, the DeepSeek child-send preflight, process_stop, lifecycle
operations, recovery_action and set_mobile_delivery), and the terminal,
attachment and file reads, voice, speech and speech configuration of
`src/media.rs`, `src/files.rs` and `src/speech.rs`, and the launch, directory
and worktree creations of `src/launch.rs`, and the archive pages of
`src/archives.rs` exactly as Python does, journals every outcome and uploads
it. As a one-hop gateway (`src/fleet.rs`) it discovers its direct peers
through `hgs swarm mobile-peers`, lists them in its heartbeats, sends each
peer's heartbeat and runs routed requests in that peer's context through
`hgs swarm mobile-peer`, rechecking the route at every call. Every black-box
module runs against it (`tests/ported.json`, with the operations it implements
under `rust_operations`); an unknown operation is refused with Python's
`unsupported operation` receipt. Arch packages, signed macOS bundles and new Windows portable ZIPs include
this binary; source installs build it only when requested or when updating an
existing Rust copy. Installation does not start or switch a gateway. Follow the
explicit [per-computer cutover](../../../docs/mobile-deployment.md#move-a-computer-to-the-rust-connector)
procedure; existing Python gateways remain supported until they move.

With native `hooks-health-v1`, `hooks_status` reads content-free configuration and
observed-callback metadata for an exact current session/run/conversation. Unknown
native trust/effective settings remain unknown. No account-wide or repair call
is transported; CLI hook repair remains explicit and native-only. The connector's
`src/hooks.rs` is unrelated: it contains non-default black-box test seams.

With native `Mobile Git status ABI: git-status-v1`, the Rust connector also
advertises the computer-scoped `git_status` read. Its exact payload is
`{request_id,path}` with an empty session; v1 uses the existing POSIX mobile
folder contract: a leading slash, no controls and at most 4096 UTF-8 bytes.
Windows Git transport is unsupported and unqualified in this version. It invokes only
`hgs git-status --path PATH --json`, without stdin, within five seconds. A
32 KiB whitelist projection keeps checkout/publication metadata, with absent
counts remaining absent and no file lists or remote URLs. The result is
`{schema:1,request_id,requested_path,snapshot}`: `requested_path` preserves the
original argument while the native snapshot may report a canonical path.
The read uses the existing verified one-hop route and a separate indexed
journal class within the shared read quota. Delivered terminal rows expire
after one hour; existing persisted SQL predicates and mutation tombstones stay
unchanged. The Python connector does not list or advertise this operation.

The optional `clear_context` field `dismiss_prompt` is a strict Boolean. Either
value requires the distinct `clear_context_dismiss` feature, offered only for an
exact trimmed native HELP line `Mobile clear-context ABI: dismiss-prompt-v1`.
For `true`, a fresh inspect must match the exact session/run/conversation,
remain unarchived and report the actual Boolean `clear_context_dismiss: true`.
Phase or question-source hints do not grant eligibility. The original payload
reaches native unchanged; omitted ordinary clear and `compact_context` keep their
existing validation. Durable duplicate/replay handling precedes the new gate,
so capability loss never authorizes another mutation identity. Both connectors
support this additive field; it does not change native clear behavior.

With a CLI that pages archives (`archives --json` in its help), heartbeats
list live sessions only (`ls --json --local --no-archives`) plus a bounded
preview of recent archives, read again when the listing's archive revision
changes and otherwise every 300 s (15 s after a failure), and `archive_list`
serves checked pages. An archive a page, a read or `hgs archives lookup`
admitted stays admitted for an hour, bounded to 2,000; any other archive is
looked up natively before anything runs for it, and a confirmed restore or
forget revokes it. Without the marker, heartbeats keep the legacy listing with
archives and report `mobile_archive.available=false`. The runtime vectors
compare both heartbeats with Python: `heartbeat-legacy.json` and the paged
`heartbeat-snapshot` of `projections.jsonl`.

## Build and test

```sh
cd services/mobile/connector
cargo +1.99.1 fmt --check
cargo +1.99.1 clippy --locked --all-targets -- -D warnings
cargo +1.99.1 test --locked
```

The unit and integration tests replay the golden vectors in
`services/mobile/vectors` (`config.jsonl`, `identities.json`,
`request_hash.jsonl`, `operations.json`, `journal_traces.jsonl`,
`journal_expect.json` and the journal fixtures; `tests/runtime_vectors.rs`
adds `validation/*.jsonl`, `projections.jsonl`, `sizes.jsonl` and
`receipts.json` for the runtime) and drive the built binary.
`tests/handoff.rs` also runs the Python `Journal` and `hgs status` against the
same state directory; it needs two variables, and fails without them when
`CI=true`:

| Variable | Meaning |
|---|---|
| `ZERUS_CONNECTOR_TEST_PYTHON` | A Python 3.11+ interpreter with aiohttp, for example the venv of `pip install ./services/mobile`. The tests add `services/mobile` to its `PYTHONPATH`. |
| `ZERUS_CONNECTOR_TEST_HGS` | A built `hgs`, such as `target/debug/hgs` after `cargo +1.99.1 build --locked --bin hgs`. |

The black-box suites (`services/mobile/tests/BLACKBOX.md`) run against a
build with the test seams:

```sh
cargo +1.99.1 build --locked --features test-hooks
ZERUS_CONNECTOR_COMMAND=$PWD/target/debug/zerus-mobile-connector \
  python -m unittest discover -s ../tests -p 'test_bb_*.py' -v
```

`cargo test` rebuilds that binary without the feature, so build it again
after testing. Only the modules listed in `services/mobile/tests/ported.json`
run then; that is every black-box module except the harness self-test, which
runs in every mode.

The parity gate (design slice C7) adds three parts, all in CI's connector job
(`BLACKBOX.md` has the details):

- **Differential traces.** Both suite runs set `ZERUS_CONNECTOR_TRACE_DIR`,
  and `python3 ../tests/differential.py compare PYTHON_DIR RUST_DIR` fails on
  any difference in native argv, parsed stdin, environment keys, relay uploads,
  heartbeats and final journal rows that `differential-allowlist.json` does
  not attribute to one of the divergences below.
- **Journal handoffs.** `test_bb_handoff` passes one state directory from
  Python to Rust and back under SIGKILLed mutations, replays, conflicting
  redeliveries, tombstones, the outbox and foreign bindings, against this
  tree's Python and each rollback baseline in
  `services/mobile/tests/rollback-baselines.json`.
- **Soak.** `ZERUS_CONNECTOR_SOAK_SECONDS=300` runs `test_bb_soak`: Terminal
  screens at four a second with `poll_interval` 1 must leave resident memory
  and open descriptors flat. `test_bb_check.py` runs only in that mode: it reads the journal of a
running Python connector with `--check`. `test_bb_runtime.py` holds the
runtime's own contracts (TLS roots, proxy variables, redirects, output caps,
stop signals and divergences a, b, g and h) with reads as the vehicle.

### Test seams

The non-default `test-hooks` feature compiles two settings in; without it
the variable names are not even in the binary, which CI checks on every
build, and packages never enable it.

| Variable | Meaning |
|---|---|
| `ZERUS_CONNECTOR_TEST_NATIVE_TIMEOUT` | The default native timeout in seconds, Python's 45 s otherwise. Calls with their own budget (2, 3, 5, 25 s) keep it. |
| `ZERUS_CONNECTOR_TEST_CLOCK_SCALE` | Divides the spacing between attempts of a result the relay refuses (30 s, divergence b) and the wait before a relay that refused the gateway gets the manifest again (300 s, divergence d). |

## Command line

```
zerus-mobile-connector --config PATH [--allow-insecure-localhost]
zerus-mobile-connector --check [--json] --config PATH [--allow-insecure-localhost]
zerus-mobile-connector --version
```

`--config=PATH` also works and a repeated option keeps its last value, as with
Python's argparse. A usage error exits 2. A refused start logs only
`ERROR: Connector could not start; verify private configuration and journal
availability` and exits 1: the reason can name private paths, so `--check`
reports it instead.

### `--check`

A read-only report for a cutover or a support question. It creates and
modifies nothing, even beside a running connector: it reads the
configuration, inspects the state directory and its files with `lstat`, probes
`connector.lock` with the shared lock `hgs status` uses (only when the file
exists), and reads the journal in one short read-only transaction, never with
`integrity_check` or `quick_check`. A hot rollback journal is reported, not
recovered; while a connector holds the lock, a journal that looks hot belongs
to its write in progress, and the check reports `busy` instead. The Python
connector's 5 s busy timeout never expires because of a check, which holds its
read lock for milliseconds.

The report covers the configuration (validity, the first refusal and every
divergence finding below), the effective values, the state directory and
files, the lock (`absent`, `free`, or `held` while a connector runs), and the
journal: whether its binding matches the configuration, the schema (`current`,
or `legacy` with the steps the next start takes), unexpected objects, and row
counts by state and delivery, including the `running` rows a drain waits for.
`--json` prints the same report as a JSON object. The exit status is 0 when the
connector would accept the configuration and journal, otherwise 1. The report
names local paths and the relay URL but never the node token; keep raw output
local.

## Runtime

One tokio `current_thread` runtime runs two loops, as Python's two tasks do:

- **Heartbeat**, every `poll_interval` (backing off to 60 s after a failure):
  `hgs ls --json --local` with `peers` and `ended_launches` removed,
  `desktop_presence` sent beside the snapshot as `presence` (dropped for 300 s
  after a relay refuses the field), `process_work.sampled_at` removed, plus
  `mobile_projects` (`swarm get` every 15 s, kept stale after a failure),
  `mobile_accounts` (`account ls` every 30 s and one background `account
  inspect` at a time, each account at most every 300 s), `mobile_attention`
  on two live tracked Codex sessions per heartbeat and `mobile_capabilities`.
  Every other listing key is forwarded, as Python forwards it;
  `test_bb_heartbeat` fails on any key nobody reviewed. A heartbeat beyond the
  relay's 1 MiB or JSON limits is not sent.
- **Requests**: upload undelivered receipts, then long-poll
  (`wait=1` until the first capability probe, else 25) and run each claimed
  request in claim order, then upload again. The probe reads `hgs --help`
  (3 s) at most every 300 s with Python's frozen marker table.

The relay client is hyper over HTTP/1.1 with rustls (ring) and the system
roots: `SSL_CERT_FILE` and `SSL_CERT_DIR` when set, otherwise the platform
store (the Keychain on macOS). It ignores proxy variables, never follows a
redirect, sends `Content-Type: application/json` and a `Content-Length` with
every POST and no `Content-Encoding`, gives each call 35 s, and caps answers at
8 MiB (polls at 30,412,800 bytes). Without a usable root every HTTPS call fails
as an unavailable relay, after one warning; nothing falls back.

Native calls run `hgs_path` (PATH lookup) with the connector's environment
minus the calling agent's 23 identity keys, stdin always a pipe with the JSON
payload, stdout and stderr each capped at 8 MiB (stderr is read and
discarded; either one over the cap fails the call), and Python's exit-status
and JSON rules. A timeout kills only the direct child and drains its pipes for
0.5 s. On macOS one gate serialises spawning, whose pipes become close-on-exec
only after creation; Linux needs none.

SIGTERM and SIGINT stop claiming and end the heartbeat; the request in
progress finishes its native call within that call's own timeout (the longest
is 45 s, below systemd's 90 s stop timeout), no further native call starts,
its outcome is journaled and the process exits 0. A second signal exits at
once (status 128 plus the signal) without touching the child, leaving its row
`running` for restart recovery. A journal failure stops the connector with
status 1, as it stops Python's.

Video messages add `voice_video` only when native help contains `voice-video-v1`
and the ordinary voice ABI. The optional bounded MP4 travels with derived PCM
in the same `voice_submit`, under its existing durable UUID. Old native binaries
refuse video before invocation; a mismatched video acknowledgement remains
uncertain. `test_bb_voice` exercises the same media/replay contract against both
connector implementations. See [voice and video messages](../../../docs/voice-messages.md).

## Divergences from the Python connector

Each divergence is listed in the design; this build implements every row.
Row c is retired: Python now reads a peer `attachment_read` through the peer
as Rust does. None survives a rollback to Python.

| # | Python | Rust | Tests |
|---|---|---|---|
| a | Archive-aware startup now waits for the first capability probe before listing and claiming; a failed later probe disables every optional operation for 300 s. | Claims also wait for the first probe, at most 3 s; a failed later probe keeps the previous flags. | `test_bb_runtime.DivergenceTests.test_claims_wait_for_the_first_capability_probe`, `runtime_vectors::a_failed_probe_keeps_the_previous_flags` |
| b | A result the relay refuses with 400, 413 or 422 is retried before every poll, so it blocks every later claim. | That row gets three attempts at least 30 s apart, polls shorten to keep that schedule, then it is marked delivered locally with "Relay rejected a stored result; preserving local outcome"; other rows and claims go on. A new receipt is made acceptable first: an error is cut to 4,096 code points, a non-string error becomes "native command reported an error that was not text", and a receipt whose canonical form with its request_id exceeds 1 MiB becomes "native result exceeds relay size limit" without its result (failed for reads, uncertain for mutations). | `test_bb_runtime.DivergenceTests.test_a_refused_read_result_is_retired_after_three_spaced_attempts`, `runtime_vectors::receipts_fit_python_and_the_relay` |
| d | A relay that refuses the extended heartbeat (400, 404 or 422) or the peer heartbeat endpoint (405) turns gateway sharing off until restart. | The manifest and peer heartbeats are offered again after 300 s. | `test_bb_gateway_downgrade.GatewayDowngradeTests.test_rust_shares_the_gateway_again_after_its_retry_time` |
| e | No checks on `connector.json`. | Refused unless owned by the effective uid and not group- or world-writable, because `hgs_path` is executed; a group- or world-readable file only logs a warning. | `binary::check_reports_reviewed_divergences_as_refusals`, `binary::main_outcomes_match_the_python_connector` (`main-world-writable-config`), `test_bb_check` |
| f | `state_dir` goes through `expanduser`, so `~user` works. | `~`, `~/...`, absolute and working-directory-relative paths work; `~user` is refused. | `config_vectors` (`state-tilde-user`), `binary` (`main-unknown-home-user`), `test_bb_check` |
| g | A stored NaN, Infinity or lone surrogate re-uploads as JSON the relay refuses, which blocks the outbox. | Rows upload as stored text. A row the relay could not accept (not strict JSON within its depth 32 and 50,000 values, not a valid receipt, or over 1 MiB) uploads as "the stored native result could not be uploaded; this request will not execute again", failed for reads and uncertain for mutations; the row itself is never rewritten. | `test_bb_runtime.DivergenceTests.test_a_stored_row_the_relay_cannot_accept_uploads_a_substitute`, `runtime_vectors::stored_rows_upload_as_stored_or_as_a_fixed_substitute` |
| h | No SIGTERM handler: the process dies, an in-flight `hgs` child finishes on its own, and the next start fails the read or doubts the mutation. SIGINT cancels the call in progress and kills its child. | SIGTERM and SIGINT: stop claiming, let the native call in progress finish within its own timeout, journal its real outcome, exit 0. A second signal leaves Python's state. | `test_bb_runtime.StopTests`, `test_bb_signals` (with the mutations of C3), `runtime_vectors::no_native_call_starts_once_the_connector_stops`, `runtime_vectors::every_native_budget_ends_before_systemd_stops_the_unit` |
| i | A non-ASCII transport host is IDNA-encoded. | Refused at startup. | `config_vectors::relay_urls_and_bindings_match_the_python_connector`, `binary::check_reports_reviewed_divergences_as_refusals`, `test_bb_check` |
| j | argparse accepts prefixes such as `--conf` and `--allow`. | Only the exact spellings; anything else exits 2. | `cli` unit tests, `binary` (`main-prefix-abbreviation`), `test_bb_check` |
| k | `poll_interval: true` passes the number check; a falsy non-string `state_dir` (`false`, `0`, `[]`, `{}`) means the default. | A boolean `poll_interval` and a non-string, non-null `state_dir` are refused; `null` and `""` still mean the default. | `config_vectors` (`poll-true`, `state-zero`, `state-false`), `test_bb_check` |

## Compatibility notes

- `connector.json` is read with the acceptance of Python's `json.loads`:
  `NaN` and `Infinity`, lone surrogate escapes and repeated keys (the last
  wins) are accepted, integers of more than 4,300 digits refused, so a file the
  Python connector starts with never stops this one. Only nesting deeper than
  1,000 levels, which Python 3.14 still reads, is refused. A known key whose
  string holds a lone surrogate is refused as invalid: no Rust string, UTF-8
  URL or path can carry one, and Python fails on such values at its first use.
- Relay URLs follow Python's `urllib.parse.urlsplit` and `checked_url`
  exactly; the vectors pin both. A non-ASCII host is refused (i), so Python's
  NFKC check of such hosts has no counterpart.
- The journal is the Python connector's file, opened in place: the same file
  checks (the state directory and its files are never symlinks, while a
  symlinked parent such as a linked `~/.local` is fine), DDL text, legacy
  upgrade, restart recovery, pragmas (`synchronous` FULL, `secure_delete` ON,
  a 5 s busy timeout, foreign keys off), rollback journal mode, retention and
  caps, `user_version` 0 and no other objects.
  Receipts are stored with Python's key order and spelling for `state`,
  `result` and `error`; nested native results are sorted, which keeps their
  length.
- A claim batch is parsed strictly, at the relay's limits plus its envelope
  (depth 34, 50,064 values). Python reads `NaN` there and rejects only the
  request that holds it (`request_envelopes.jsonl`,
  `batch-nan-only-rejects-one`); here that batch is refused whole. The relay
  never sends one: it re-encodes every request canonically.
- Native output is parsed strictly as well: `NaN`, `Infinity` and lone
  surrogates, which `hgs` never prints, are invalid JSON here, and serde_json
  refuses nesting deeper than 128 levels, where Python reads up to its
  recursion limit. A number beyond the double range (`1e400`), which Python
  reads as infinity, makes a receipt the generic "connector could not record
  native outcome".
- Where malformed native output would raise a Python TypeError outside every
  handler (an unhashable `state`, `archive_id` or `name` in the listing),
  which stops Python's heartbeat task, the value is treated as absent.
  Account balances and credits accept ASCII digits only, where Python's `\d`
  also matches other decimal digits.
- An archive page whose integer `archived_at` exceeds the double range is
  refused with the generic receipt, as Python's OverflowError gives; in the
  heartbeat preview it reports the preview as unavailable, where Python's
  heartbeat would fail.
- An operation of a later slice is refused after its claim, before any
  validation, so its receipt is `unsupported operation` where Python would
  report a validation error or run it.
- Two OpenCode extensions exist only here (`CONNECTOR_ONLY` and the
  `opencode_launch_effort` feature in `capabilities.rs`). `CONNECTOR_ONLY` is a
  table of each relay operation only this connector executes, the native gate
  that advertises it and whether it is a read or a mutation;
  `services/mobile/tests/rust-only-operations.json` mirrors it for the relay
  tests, the black-box suites and `differential.py`. `remove_queued` is
  advertised with the scoped session-action marker that gates the other
  session actions and journaled as a mutation, so an in-flight one recovers
  as uncertain after a restart. That marker predates queue removal, so an
  older hgs (for example on a one-hop peer) is advertised too and refuses the
  request, leaving an uncertain receipt; only `remove_queued` in the session's
  `allowed_actions` shows that hgs can remove the message, and phones require
  it. Python never lists either extension, so a rollback to Python withdraws
  them from phones; `differential.py` projects both. Python's own restart
  recovery does not know `remove_queued`: a removal that a killed Rust
  connector left running would become failed there, not uncertain, while hgs
  still never resends the message.
- Both implementations take an exclusive `flock(2)` on `connector.lock`, so
  they never run on one journal at once, and `hgs status` reports either one as
  running.

## Dependencies

The protocol library of `../relay` without its `server` feature (codec,
envelope validation, operation sets), serde_json with `arbitrary_precision`
for byte-exact request hashes, rusqlite with bundled SQLite (as in `hgs`, so
gateways do not depend on the system library's version and options), and
rustix for the effective uid, `flock` and `O_NOFOLLOW` through safe calls.
rustix needs no libc on Linux and replaces both fs2 and nix, whose closures
are larger.

The runtime adds tokio (current_thread runtime, processes, signals, timers),
hyper 1 with hyper-util's pooled client (HTTP/1.1 only), http-body-util,
rustls with ring through hyper-rustls, rustls-native-certs for the system
roots, and futures-util's function forms of `select` and `join`, so tokio
needs no proc-macro crates. There is no reqwest, url crate, IDNA, OpenSSL or
aws-lc-rs. The media operations add base64 0.22, the relay's version, to read
file ranges, recordings and sealed credentials as Python's strict
`base64.b64decode(validate=True)` does.

## Windows local gateway

See [Windows gateway setup](../../../docs/mobile-deployment.md#windows-gateway)
and the [design](../../../docs/superpowers/specs/2026-10-20-windows-mobile-gateway-design.md).
The bundled connector exposes `--manage status|save|start|stop --config PATH`
with bounded JSON stdin/stdout for the desktop. Status is read-only and redacts
credentials. Save and Start pin the configuration revision; Stop also pins the
current random generation. Neither control names a PID or kills a tree. Save
requires the connector to be stopped. The native executable always comes from
beside the running connector, including after a portable folder moves.

Windows uses protected current-user/SYSTEM/Administrators DACLs, reparse-point
refusal, single-link files and Windows file locks. Unix retains its existing
0700/0600, uid and flock rules. Windows native calls use hgs.exe pipes and its
SSH adapter; ssh.exe is never spawned directly. The session host remains outside
the connector/GUI lifetime. Windows Ctrl+C or a generation-bound Stop drains the
in-flight request before exit. Unsupported media/path/peer/provider operations
are refused after duplicate lookup and before any native call.

Native Windows validation (no skips count as qualification):

```sh
cargo +1.99.1 test --locked --manifest-path services/mobile/connector/Cargo.toml --test windows_gateway
cargo +1.99.1 clippy --locked --manifest-path services/mobile/connector/Cargo.toml --all-targets -- -D warnings
```

`tests/windows_scope.rs` runs policy and durable refusal controls on Unix; it
is not evidence of Windows ACL/process behavior. `tests/windows_gateway.rs`
runs the actual Windows ACL, hard-link, lock, revision, credential rotation and
stop-generation controls without contacting a relay. The isolated Windows host
and HTTPS transport suite is described in CONTRIBUTING.md.
