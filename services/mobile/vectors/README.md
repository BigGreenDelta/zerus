# Mobile connector golden vectors

Language-neutral contracts captured from the Python connector
(`services/mobile/zerus_mobile`). The Rust connector and relay tests read these
files; `services/mobile/tests/test_vectors.py` recomputes every one of them
from the Python code and fails on any byte of difference.

```
python3 services/mobile/vectors/capture.py                   # rewrite the files
python3 services/mobile/vectors/capture.py --check           # recompute and compare
python3 services/mobile/vectors/capture.py --refresh-oracle  # also recompute the relay-oracle column
```

Run it in an environment where `zerus_mobile` and aiohttp import, for example
the venv the relay CI job builds with `pip install ./services/mobile`. In
`services/mobile/relay`, `ZERUS_BLESS=1 cargo +1.99.1 test --locked --test
vectors` rewrites only the `relay` column of `validation/*.jsonl`.

## Provenance and freeze rule

- The capture is offline and deterministic: a fixed clock at `EPOCH`
  (1760000000.0), umask 022, UUIDv5 identities, synthetic `fixture-` and
  `synthetic-` tokens, reserved example domains (`example.test`,
  `example.invalid`), scratch state directories, and no network connection or
  native process. Scratch paths appear as `$SCRATCH`, `$HOME` and `$CWD`.
- Every value comes from running the real Python code: `Journal`,
  `Connector.validate`, `Connector.execute` and `Connector.snapshot` with a
  scripted `_native`, one poll of `Connector.requests()` per request or batch
  body, `connector.main()`, and the pure projection functions.
  The managed relay hostnames appear only where `managed_relay.py` names them.
- A change in Python behaviour must show up as a reviewed diff of these files.
  A vector frozen on a Python bug encodes that bug; the deliberate Rust
  divergences (plan section 3.4, rows a-k) are marked with a `divergence` field
  where a vector touches them.
- The `oracle` column of `validation/*.jsonl` is frozen data. `--check` and
  `test_vectors.py` copy it from the committed files. Only `--refresh-oracle`
  recomputes it, through `services/mobile/tests/relay_oracle.py`: a verbatim
  copy of the reference relay's `validate_request` that delegates only to
  connector validators, so deleting `zerus_mobile.server` breaks neither.
  `test_vectors.py` checks that the copy reproduces the frozen column, and,
  while `server.py` exists, that it matches that source. A frozen decision is
  reused only for the exact submission it was computed from; a new or changed
  case needs `--refresh-oracle`.
- The `relay` column is the Rust relay's decision (`protocol::request`). The
  capture copies it like the oracle column, and a new or changed submission has
  none until `ZERUS_BLESS=1` computes it.
  `services/mobile/relay/tests/vectors.rs` recomputes the column and lists every
  case where it differs from the oracle column.
- When the Python connector is deleted, these files stay as frozen data and the
  capture script goes with it; record the last commit that produced them here.

Every file is at most 64 KiB, ASCII, and contains no lone surrogate, so a
strict JSON reader can load it. JSONL files hold one record per line.

## Files

| File | Contents |
|---|---|
| `request_hash.jsonl` | Canonical JSON and `request_hash`: key order (code point order, not UTF-16), duplicate keys, Unicode and every escape class, lone surrogates, the serde magic key, number spelling (`-0`, `-0.0`, exponents, the 1e16 and 1e-5 boundaries, subnormals, the largest double, integers beyond 2^64 and 2^80, 4,300 and 4,301 digits), and the rejects. |
| `request_envelopes.jsonl` | One claimed request per operation as the relay delivers it, `gateway_route` variants, batch rules (at most 100 requests, one invalid request rejects only itself) and a request at the relay's exact parse limits inside its batch envelope. |
| `identities.json` | Relay binding, `local:` swarm fallback, uuid5 `route_id`, `checked_url`, managed transport and `identity_url` rules. |
| `config.jsonl` | `Connector(config)` outcomes with exact messages, effective values and the paths created, GUI-written files, and `main()` exit status and log lines. |
| `journal/*.sql` | Journal fixtures: `python-*.sql` written by the real `Journal` and dumped deterministically; `legacy-5-column.sql`, `no-metadata.sql`, `foreign-binding.sql` and `caps.sql` written by hand. |
| `journal_expect.json` | Each fixture reopened by the Python `Journal`: schema (`sqlite_master` SQL, `table_info`, `index_list`, `index_xinfo`, persistent and connection pragmas), rows, pending uploads, claims, replays and conflicts. |
| `journal_traces.jsonl` | Step-by-step `Journal` traces on fresh state directories, including file modes, symlinks, hard links, sidecars and a second instance. |
| `receipts.json` | Fixed receipt texts exactly as stored and uploaded. |
| `validation/*.jsonl` | Submissions with the connector's decision and receipt error, the frozen relay-oracle decision and the Rust relay's decision, with the source test where a case comes from a `validate_request` call site. |
| `projections.jsonl` | Pure projection functions, the heartbeat snapshot, and `Connector.execute` runs with their native argv, stdin payloads, options and stored receipts. |
| `heartbeat-legacy.json` | Full current-Python heartbeat and native calls with the archive paging marker absent; retains archived rows and project memberships, the listing a CLI without archive pages gives. |
| `sizes.jsonl` | Python's byte measures and the limits that flip on one byte. |
| `operations.json` | Operation sets, SQL predicates, constants and the help-marker to capability map. |
| `errors.json` | AST catalogue of literal error texts, log formats, and library messages that reach receipts. |

## Conventions

- Clock values (`at`) are seconds; `EPOCH` is 1760000000.0. `snapshot_age` is
  the number of seconds since the last local snapshot.
- A string equal to a key of `fill` stands for the concatenation of its
  `[unit, count]` parts; `raw_parts` concatenates `[text, count]` parts.
- `android` marks a receipt error that `AttachmentViewCache.failure()` treats
  specially: `update_required` when it contains `unavailable` or
  `unsupported operation`, `truncated` when the phone shows only its first 200
  characters.
- `stage: before_journal` means the connector rejects the request without a
  journal row or an upload ("Rejected relay request" in the log).
- A row with `envelope: request` is polled as the only member of a relay
  batch. `rejected: parse` means the poll body is not JSON Python accepts,
  `rejected: batch` that the whole batch is refused, and `rejected: request`
  that only that request is dropped, before any journal row.
- An `OSError` is recorded by its errno only; its text names local paths. A
  JSON parse error, and a `TypeError` in the relay-oracle column, are recorded
  by type only, because their wording is Python's own.
- The `relay` column holds the decision and, for a rejection, the HTTP status
  only: the Rust relay's validation errors carry no detail.
- The order of `pending` rows is unspecified (the query has no `ORDER BY`), so
  the vectors list them sorted by request ID.

## Journal steps

A scenario starts from an empty private scratch directory. A fixture is loaded
into `state/journal.sqlite3`, which is first created with mode 0600 in a 0700
directory, as the connector does. Steps:

| `op` | Meaning |
|---|---|
| `setup` | Create directories, files, symlinks or hard links below the scratch root. |
| `open` / `close` | Open or close the journal for `binding` (`instance` defaults to `a`). |
| `claim`, `finish`, `replay`, `delivered`, `prune`, `pending` | The `Journal` method of the same name at clock `at`. |
| `rows` | Every `requests` row, ordered by `request_id`. |
| `schema` | Schema and pragmas, plus the open connection's pragmas. |
| `files` | Every path below the scratch root with its kind and mode. |
| `sql` | One statement on a separate connection (test-only aging). |

`at` stays in effect for later steps until another step sets it. `expect` is
`{}` for a step without a result, `{"returns": ...}`, or `{"error": ...,
"raised": ...}`.
