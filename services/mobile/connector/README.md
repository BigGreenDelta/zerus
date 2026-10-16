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
operations, recovery_action and set_mobile_delivery) exactly as Python does,
journals every outcome and uploads it. Every other operation (terminal,
attachments, voice, speech, launch, directories, worktree creation and the
one-hop gateway) is claimed, refused with Python's `unsupported operation`
receipt (which phones show as "update required") and never advertised, until
its slice implements it (`src/capabilities.rs`, `IMPLEMENTED`;
`tests/ported.json` lists the same operations for the black-box suites).
Nothing packages or starts this binary yet; gateways keep running the Python
connector.

Archive paging is also deferred: `archive_list` is refused and never
advertised, even when the CLI offers it. Heartbeats retain the legacy listing
and report `mobile_archive.available=false`. The full legacy heartbeat is
compared with current Python's captured legacy-native contract in
`heartbeat-legacy.json`; the paged Python oracle remains separate.

## Build and test

```sh
cd services/mobile/connector
cargo +1.85.0 fmt --check
cargo +1.85.0 clippy --locked --all-targets -- -D warnings
cargo +1.85.0 test --locked
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
| `ZERUS_CONNECTOR_TEST_HGS` | A built `hgs`, such as `target/debug/hgs` after `cargo +1.85.0 build --locked --bin hgs`. |

The black-box suites (`services/mobile/tests/BLACKBOX.md`) run against a
build with the test seams:

```sh
cargo +1.85.0 build --locked --features test-hooks
ZERUS_CONNECTOR_COMMAND=$PWD/target/debug/zerus-mobile-connector \
  python -m unittest discover -s ../tests -p 'test_bb_*.py' -v
```

`cargo test` rebuilds that binary without the feature, so build it again
after testing. Only the modules listed in `services/mobile/tests/ported.json`
run then. `test_bb_check.py` runs only in that mode: it reads the journal of a
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
| `ZERUS_CONNECTOR_TEST_CLOCK_SCALE` | Divides the spacing between attempts of a result the relay refuses (30 s, divergence b). |

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

## Divergences from the Python connector

Each divergence is listed in the design; this build implements every row
except c and d, which arrive with the one-hop gateway. None survives a
rollback to Python.

| # | Python | Rust | Tests |
|---|---|---|---|
| a | Archive-aware startup now waits for the first capability probe before listing and claiming; a failed later probe disables every optional operation for 300 s. | Claims also wait for the first probe, at most 3 s; a failed later probe keeps the previous flags. | `test_bb_runtime.DivergenceTests.test_claims_wait_for_the_first_capability_probe`, `runtime_vectors::a_failed_probe_keeps_the_previous_flags` |
| b | A result the relay refuses with 400, 413 or 422 is retried before every poll, so it blocks every later claim. | That row gets three attempts at least 30 s apart, polls shorten to keep that schedule, then it is marked delivered locally with "Relay rejected a stored result; preserving local outcome"; other rows and claims go on. A new receipt is made acceptable first: an error is cut to 4,096 code points, a non-string error becomes "native command reported an error that was not text", and a receipt whose canonical form with its request_id exceeds 1 MiB becomes "native result exceeds relay size limit" without its result (failed for reads, uncertain for mutations). | `test_bb_runtime.DivergenceTests.test_a_refused_read_result_is_retired_after_three_spaced_attempts`, `runtime_vectors::receipts_fit_python_and_the_relay` |
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
- An operation of a later slice is refused after its claim, before any
  validation, so its receipt is `unsupported operation` where Python would
  report a validation error or run it.
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
aws-lc-rs.
