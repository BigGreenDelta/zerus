# Rust mobile connector

`zerus-mobile-connector` will replace the Python connector in
`services/mobile/zerus_mobile` (design:
`docs/superpowers/specs/2026-10-15-rust-mobile-connector-design.md`). Phones,
relays and `hgs` must not be able to tell the two apart, except in the
deliberate divergences below.

This build has the foundation only: the command line, `connector.json`
loading and validation, the managed relay transport and binding, the private
state directory, the journal opened in place with Python's schema, the lock
the Python connector also takes, and `--check`. A valid start opens the
journal and then exits 1 with "Connector runtime is not available in this
build; keep using the Python connector": the relay runtime comes next. Nothing
packages or starts this binary yet.

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
`journal_expect.json` and the journal fixtures) and drive the built binary.
`tests/handoff.rs` also runs the Python `Journal` and `hgs status` against the
same state directory; it needs two variables, and fails without them when
`CI=true`:

| Variable | Meaning |
|---|---|
| `ZERUS_CONNECTOR_TEST_PYTHON` | A Python 3.11+ interpreter with aiohttp, for example the venv of `pip install ./services/mobile`. The tests add `services/mobile` to its `PYTHONPATH`. |
| `ZERUS_CONNECTOR_TEST_HGS` | A built `hgs`, such as `target/debug/hgs` after `cargo +1.85.0 build --locked --bin hgs`. |

The black-box suites (`services/mobile/tests/BLACKBOX.md`) run against the
binary with `ZERUS_CONNECTOR_COMMAND=$PWD/target/debug/zerus-mobile-connector`;
only the modules listed in `services/mobile/tests/ported.json` run then.
`test_bb_check.py` runs only in that mode: it reads the journal of a running
Python connector with `--check`.

The non-default `test-hooks` feature holds the black-box test seams
(`ZERUS_CONNECTOR_TEST_*`). Packages never enable it.

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

## Divergences from the Python connector

Each divergence is listed in the design; this build implements the rows that
concern configuration and the command line. The others arrive with the
runtime. None survives a rollback to Python.

| # | Python | Rust | Tests |
|---|---|---|---|
| e | No checks on `connector.json`. | Refused unless owned by the effective uid and not group- or world-writable, because `hgs_path` is executed; a group- or world-readable file only logs a warning. | `binary::check_reports_reviewed_divergences_as_refusals`, `binary::main_outcomes_match_the_python_connector` (`main-world-writable-config`), `test_bb_check` |
| f | `state_dir` goes through `expanduser`, so `~user` works. | `~`, `~/...`, absolute and working-directory-relative paths work; `~user` is refused. | `config_vectors` (`state-tilde-user`), `binary` (`main-unknown-home-user`), `test_bb_check` |
| i | A non-ASCII transport host is IDNA-encoded. | Refused at startup. | `config_vectors::relay_urls_and_bindings_match_the_python_connector`, `binary::check_reports_reviewed_divergences_as_refusals` |
| j | argparse accepts prefixes such as `--conf` and `--allow`. | Only the exact spellings; anything else exits 2. | `cli` unit tests, `binary` (`main-prefix-abbreviation`) |
| k | `poll_interval: true` passes the number check; a falsy non-string `state_dir` (`false`, `0`, `[]`, `{}`) means the default. | A boolean `poll_interval` and a non-string, non-null `state_dir` are refused; `null` and `""` still mean the default. | `config_vectors` (`poll-true`, `state-zero`, `state-false`), `test_bb_check` |

## Compatibility notes

- `connector.json` is read with the acceptance of Python's `json.loads`:
  `NaN` and `Infinity`, lone surrogate escapes and repeated keys (the last
  wins) are accepted, integers of more than 4,300 digits refused, so a file the
  Python connector starts with never stops this one. A known key whose string
  holds a lone surrogate is refused as invalid: no Rust string, UTF-8 URL or
  path can carry one, and Python fails on such values at its first use.
- Relay URLs follow Python's `urllib.parse.urlsplit` and `checked_url`
  exactly; the vectors pin both. A non-ASCII host is refused (i), so Python's
  NFKC check of such hosts has no counterpart.
- The journal is the Python connector's file, opened in place: the same file
  checks, DDL text, legacy upgrade, restart recovery, pragmas (`synchronous`
  FULL, `secure_delete` ON, a 5 s busy timeout, foreign keys off), rollback
  journal mode, retention and caps, `user_version` 0 and no other objects.
  Receipts are stored with Python's key order and spelling for `state`,
  `result` and `error`; nested native results are sorted, which keeps their
  length.
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
