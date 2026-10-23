# Black-box connector tests

The `test_bb_*.py` modules run the outbound connector as a separate process,
the way it is deployed, and observe it only through its command line, its
configuration file, its exit status and log, a fake relay, a scripted `hgs`
and its journal (read-only while it runs). They never import the connector, so
the same suites can check the Python connector now and the Rust connector
later. The in-process `test_connector*.py` and `test_mobile_*.py` modules stay
until the Python connector is deleted; the tables below record which
black-box test covers each of their connector cases.

## Running

```sh
python -m unittest discover -s services/mobile/tests -p 'test_bb_*.py' -v
```

By default the harness runs `python -m zerus_mobile.connector` from this
source tree with the interpreter running the tests, which needs `aiohttp`
(install `services/mobile`). To run another build:

| Variable | Meaning |
|---|---|
| `ZERUS_CONNECTOR_COMMAND` | Command line, split like a shell, run in place of the Python connector. Give an absolute program path: the connector's PATH holds only the scripted `hgs` and the system directories. |
| `ZERUS_CONNECTOR_IMPLEMENTATION` | `python` or `rust`; defaults to `python` without a command and `rust` with one. Assertions on a reviewed divergence depend on it. |
| `ZERUS_CONNECTOR_TRACE_DIR` | Write one differential trace per test there and seed each test's `uuid4` from its id ([Differential traces](#differential-traces)). |
| `ZERUS_CONNECTOR_ROLLBACK_COMMANDS` | Console scripts of rollback baseline venvs, separated by `:`, for `test_bb_handoff` ([Journal handoffs](#journal-handoffs)). |
| `ZERUS_CONNECTOR_SOAK_SECONDS` | Run `test_bb_soak` for that long; it skips without it. |

Outside Python mode only the modules listed under `rust` in `ported.json` run;
the others skip. Within a ported module, a known operation missing from
`rust_operations` expects the `unsupported operation` receipt; the connector's
`runtime_vectors` test keeps that list equal to what the build implements.
`rust-only-operations.json` lists the relay operations only the Rust connector
executes, with the marker that gates each and its class; a Rust connector
advertises them under that marker (`session_harness.advertised()`), and the
connector's `CONNECTOR_ONLY` table must equal it. `test_bb_harness.py` and
`test_bb_attachment_read.ScriptedAnswerTests` check the harness itself and
always run. `test_bb_directories.NativeDirectoryTests`,
`test_bb_speech.SpeechConfigurationTests.test_real_cli_manages_a_profile_without_inference`
and `test_bb_peer_transport` also run a built `hgs`
(`ZERUS_DIRECTORY_TEST_HGS`, `HGS_TEST_BIN` or `target/debug/hgs`); without
one they skip, except with `CI=true`, where they fail.

The 211 tests in 32 modules took 320 s on a Linux workstation against the
Python connector, with `target/debug/hgs` built (2026-10-15), and 327-332 s
each in four concurrent runs confined to four CPUs, each in its own scratch
directory. Seven skipped: the two Rust-only tests of `test_bb_native_limits`
and the five of `test_bb_check`, since `--check` exists only in the Rust
connector. Against the Rust connector, the ported modules and the harness
self-tests (63 tests) took 90 s on a Linux workstation (2026-10-15), mostly
`test_bb_runtime` (47 s) and `test_bb_computer` (32 s); `test_bb_runtime`
adds three Rust-only tests that Python mode skips. Each test starts its own
connector processes, and the time goes to the connector's own clocks rather
than to the machine:
`test_bb_peer_routes` (33-38 s), `test_bb_fleet` (34-35 s), `test_bb_computer`
(31 s, the 15 s project refresh), `test_bb_gateway` (30-31 s),
`test_bb_gateway_downgrade` (26-28 s) and `test_bb_native_limits` (25 s) take
58% of a run; every other module takes 13 s or less. By slice, the base
modules (42 tests besides `test_bb_check`) took 28-29 s, the session
operations of H2 (58 tests) 56-60 s, the computer, read and media operations
of H3 (74 tests) 72-74 s and the peers of H4 (32 tests) 158-162 s.

## Isolation

Each test gets a scratch directory holding HOME, the XDG directories, TMPDIR,
HGS_CONFIG_DIR, HGS_STATE_DIR and the connector's `state_dir`. The connector
inherits none of the caller's environment. Its configuration may name only a
loopback relay, or a reserved `.invalid` host that startup must refuse. On
Linux a guard samples the connector's sockets and fails the test on any
non-loopback peer. The node token must never appear in the connector's log.
Teardown kills the connector's own session, which holds only it and the
scripted `hgs` processes it started; no tmux server or agent is involved.

## Harness parts

- **`FakeRelay`** (`connector_harness.py`): one node credential on loopback.
  It enforces what a connector must meet in front of the real relay and its
  nginx: `Content-Type: application/json` and a `Content-Length` on every POST,
  no `Content-Encoding` other than `identity`, at most 1 MiB per POST (the nginx
  default on `/v1/` and the relay's node limit), strict JSON within the relay's
  depth 32 and 50,000 values, valid heartbeat and result receipts, no
  duplicate query parameters, and only known endpoints. It answers a breach as
  the relay or nginx would and records it in `violations`; every test fails
  on a recorded violation unless it provoked one on purpose. Submissions are
  queued as a phone would make them, refused above the relay's parse limits,
  and handed out one per long poll in the relay's batch envelope
  (`{"requests": [...]}` without `computer_id`, plus `gateway_route` and
  `expires_at`), so a batch can exceed the limits its request met. Results
  follow the relay's claim rules (404 unknown, 409 not claimed or different,
  200 identical). `inject()` hands out a delivery no relay would queue (a
  non-canonical identity, a malformed envelope or a non-object), in its own
  batch. As the relay does, it answers a peer heartbeat with 404 when its
  route is not in the last heartbeat manifest it accepted (a heartbeat
  without gateway fields withdraws every route) and with 409 when that route
  is offline or names another machine, or when that heartbeat's snapshot did
  not offer `gateway_one_hop`, which leaves its routes unguarded.
  `rule()` programs status answers,
  optionally limited by count or a predicate on the parsed body. All traffic
  is kept in `exchanges`.
- **`scripted_hgs.py`**: the `hgs` on the connector's PATH. It answers from
  `script.json`, re-read on every call, by exact argv or prefix. A rule's
  `stdin_fields` also requires stdin to be a JSON object whose listed fields
  have exactly those JSON values, so `true` never matches `1`; the terminal,
  speech and voice suites pick an answer by its `action` this way and the
  gateway suites by a peer envelope's `via`, target machine and inner argv.
  It answers with JSON (with `{{stdin.a.b}}` and `{{argv.N}}` templates, and
  `fill` placeholders that expand to megabytes as the golden vectors do),
  text or a file's bytes (`stdout_path`), an exit status, a delay, `exec`
  (delegate to a real hgs) or `kill_parent` (SIGKILL the connector after the
  native work, to model a crash before journaling). It logs a `start` event
  (argv, stdin, environment, open descriptors) and an `end` event per call.
  `ScriptedHgs` in the harness refuses a rule key the script does not read,
  which would otherwise widen the rule. `default_rules()` gives a minimal
  live computer with one non-Codex session, so no background attention scan
  runs.
- **Shared suite parts**, which take identities, help lines, receipt texts
  and limits from the golden vectors: `session_harness.py` (the session
  operations, `SessionCase`), `bb_operations.py` (computer, read and media
  operations, `OperationCase`) and `gateway_harness.py` (a gateway's fleet).
  `NATIVE_TIMEOUT_HOOK` and `CLOCK_SCALE_HOOK` in `connector_harness.py` name
  the settings a Rust build reads only with its `test-hooks` feature.
- **`Journal`** reads a journal read-only; **`stopped_journal`** writes one only
  while it holds the connector lock, so a running connector is never edited.

## Reviewed divergences

`IMPLEMENTATION` selects the expected outcome where the design
(`docs/superpowers/specs/2026-10-15-rust-mobile-connector-design.md`) records a
deliberate difference; the connector README lists those the Rust build
implements so far:

| Divergence | Test | Python | Rust |
|---|---|---|---|
| a: capability probing | `test_bb_runtime.DivergenceTests.test_claims_wait_for_the_first_capability_probe` | Archive-aware startup now probes before listing and claiming, so the optional read succeeds and polls use `wait=25`. A failed later probe still disables optional operations. | Also waits for the first probe, at most 3 s; a failed later probe keeps the previous flags (a Rust unit test). |
| b: a result the relay rejects | `test_bb_requests.test_rejected_result_blocks_later_claims_in_python`, `test_bb_native_limits.RefusedResultTests`, `test_bb_runtime.DivergenceTests` | A 400, 413 or 422 upload refusal is retried before every poll, so it blocks every later claim. A native error beyond 4,096 code points, a non-string one, and a receipt within `MAX_RESULT` whose canonical form with its request_id exceeds 1 MiB are journaled as they are and refused that way. | The relay accepts the sanitised receipt: the error cut to 4,096 code points or a fixed text, an oversized receipt uncertain without its result. A stored result the relay keeps refusing is retired after three attempts, which needs the `test-hooks` clock scale (`CLOCK_SCALE_HOOK`). |
| d: a relay that refuses the extended heartbeat | `test_bb_gateway_downgrade` | Keeps gateway sharing off until restart. | The same within the shared tests; `test_rust_shares_the_gateway_again_after_its_retry_time` offers the manifest again after 300 s, scaled by `CLOCK_SCALE_HOOK`. |
| g: a stored row the relay cannot accept | `test_bb_runtime.DivergenceTests.test_a_stored_row_the_relay_cannot_accept_uploads_a_substitute` | Re-encodes a stored NaN, which the relay refuses, so the row blocks every later claim. | Uploads a fixed substitute, failed for a read and uncertain for a mutation; the row stays as stored. |
| h (amended): SIGTERM | `test_bb_signals`, `test_bb_worktrees.test_sigterm_never_cuts_a_creation_short`, `test_bb_runtime.StopTests` | Dies at once (exit -15); the in-flight native call finishes on its own; restart journals the mutation uncertain. | Stops claiming, lets the in-flight call finish, journals its real outcome, exits 0. |

Divergence c, a peer `attachment_read`, no longer differs. Python failed
with "connector could not record native outcome" before it asked the peer,
because the peer callback took no output bound; since main's cc42fff9 gave it
one, Python reads the file through the peer and serves the requested range as
Rust does, which
`test_bb_peer_operations.test_peer_attachment_read_reads_the_file_through_the_peer`
pins for both.

Under h, neither may kill the native mutation child, and no request is
claimed after the signal. For a mutation SIGINT is not covered: Python's
KeyboardInterrupt cancels the request and SIGKILLs the hgs child, which
amended divergence h does not address. The design's runtime section stops
the Rust connector on SIGINT as on SIGTERM, which
`test_bb_runtime.StopTests.test_sigint_stops_like_sigterm` pins with a read.

## Differential traces

The C7 parity gate runs the suites once against each connector with
`ZERUS_CONNECTOR_TRACE_DIR` set, then compares the two directories:

```sh
python3 services/mobile/tests/differential.py compare PYTHON_TRACES RUST_TRACES
```

`differential.py` writes a trace when a test ends: every native `hgs` call
(argv, parsed stdin and environment keys), every relay exchange (poll query,
parsed upload or heartbeat, answer status) and the final journal rows, read
from a copy so a killed connector's write-ahead log does not matter. The test's
scratch path, relay URL, node token and relay binding become placeholders,
clock readings (`*_at` numbers) and the live `metrics` of a snapshot become
markers, and large values a digest. A `request_hash` that equals Python's hash
of a body the relay handed out is marked as such, so both implementations must
hash every request as Python does even when the request carries the test's
clock. Each test seeds `uuid.uuid4` from its id, so both runs use the same
identities.

What a connector's clock drives differs between two runs of the same connector
and is reported but not compared: polls, single heartbeats, presence and peer
heartbeats, and the periodic native reads (`PERIODIC`, also through a peer
envelope). Instead each run's accepted heartbeats are summarized in a shape (the
union of their body and snapshot keys and capability operations and features),
which must match after two explicit capability projections. The maintained
Python connector never advertises `opencode_launch_effort`; the Rust connector
must advertise exactly that additional feature when its operations include
`launch`, and must omit it otherwise. Python also never advertises an operation
of `rust-only-operations.json`, the relay operations only the Rust connector
executes (`remove_queued`, OpenCode queue removal, and the separate `git_status`
read). Rust must advertise such an
operation exactly when its operations include every operation its frozen
marker gates in Python: `remove_queued` comes with the session actions
(`forget` and the others), under the session-action marker. An operation behind
a marker the vectors do not know is left out of the Rust shape, since a Python
shape cannot show that marker; `session_harness.advertised()` checks it in the
suites instead. In particular, the exact `git-status-v1` help line gates only
the Rust Git read; it adds no operation to Python's frozen tables. The comparison checks complete local and
peer heartbeat shapes, rejects malformed or duplicate arrays and keeps all
other body/snapshot keys, operations and features exact. Raw traces stay intact.
Native work and journal rows must match in number; result
uploads must match as distinct receipts and answers, since a result whose
answer was lost on a busy runner is uploaded and accepted again. Delivery flags
race the teardown's SIGKILL and are left out of journal rows.

A difference fails unless `differential-allowlist.json` names it. Each entry
gives a test pattern, the kind (`native`, `relay` or `journal`), the side it
appears on (`python`, `rust` or `count`), a regular expression for the entry
and one of the design's divergences a-k with a reason; an entry that matches
nothing fails too, so the list never outlives a fix. Key order and whitespace
cannot differ (everything is parsed) and logs are not traced. Tests marked
`@untraced` are reported but not compared: those that run a real `hgs`, whose
machine ids and paths differ between any two runs. Tests that skip on one side
(Rust-only tests, the handoffs) are counted, not compared.

## Journal handoffs

`test_bb_handoff` passes one state directory from Python to Rust and back,
as a cutover and its rollback do, while each implementation holds rows the
other wrote: a mutation SIGKILLed after its native success (recovered as
uncertain, replayed, never executed again), delivered replays and conflicting
redeliveries (no native work, no upload, no journal change), seven-day
tombstones, undelivered outbox rows (uploaded unchanged) and journals bound to
other credentials (refused, unchanged). It runs in a Rust run, against this
tree's Python connector and each venv in `ZERUS_CONNECTOR_ROLLBACK_COMMANDS`.
CI builds one venv per commit in `rollback-baselines.json`; a gateway's
recorded pre-cutover commit belongs there before its cutover (design E0 and
step 2 of the cutover runbook).

The tombstone handoff retains its SIGKILLs. Before its final stopped byte and
row checks, one bounded schema read through `stopped_journal` permits SQLite's
normal hot rollback recovery under the exclusive connector lock. It invokes no
connector migration or prune operation. Live journal observations remain
`mode=ro`; a held connector lock or unrelated SQLite error still refuses.
`StoppedJournalTests` uses one disposable, bounded SQLite writer to require the
extended `SQLITE_READONLY_ROLLBACK` failure before recovery and unchanged committed
tombstones, schema, database bytes and file modes afterward. This controlled
stand-in does not identify a historical CI database's unretained error subtype.

## Soak

`test_bb_soak` heartbeats every second while a phone polls one Terminal screen
four times a second for `ZERUS_CONNECTOR_SOAK_SECONDS`, sampling the
connector's resident memory and open descriptors from `/proc` after a
warm-up. The last third of the samples may exceed the first by the allocator
margin in the module at most. CI soaks the Rust build for 300 s beside the two
suite runs.

## Heartbeat key review

`test_bb_heartbeat.LISTING_KEYS` classifies every top-level key of
`hgs ls --json --local` as forwarded, withheld or carried beside the snapshot.
`test_reviewed_listing_keys_match_the_hgs_source` reads `local_json` in
`src/cli.rs`, so a key that hgs gains fails until someone classifies it; the
heartbeat test then fails on any snapshot key outside the reviewed set. The
table records what the Python connector forwards today.

## Coverage of the in-process connector tests

Status: **covered** (same contract black-box), **partial** (the rest is
planned where named), **planned** (slice or vector named). A "Relay only"
row names the Rust relay contract that replaces a relay case;
[`ORACLE.md`](../relay/tests/ORACLE.md) maps every test of the Python reference relay.

### `test_connector.py` (`ConnectorTests`)

| In-process test | Black-box coverage | Status |
|---|---|---|
| `test_process_observation_clock_does_not_change_snapshot_work` | `test_bb_heartbeat.test_heartbeat_withholds_private_keys_and_carries_only_reviewed_ones` | covered |
| `test_ended_launches_never_leave_the_computer` | same | covered |
| `test_desktop_presence_travels_beside_the_snapshot_with_old_relay_fallback` | same, and `test_presence_falls_back_for_a_relay_without_the_field`; the 300 s re-probe and presence vanishing when the desktop stops need an injected clock (Rust unit test) | partial |
| `test_cli_version_survives_enrichment_and_legacy_snapshots_remain_supported` | `hgs_version` forwarding in the heartbeat test; a listing without it: `test_bb_controls.test_the_snapshot_reports_the_setting_with_or_without_an_hgs_version` | covered |
| `test_fixed_argv_and_exact_stdin_for_all_allowlisted_operations` | `test_bb_requests` (`inspect`, `send`, `interrupt`), `test_bb_session_validation.test_laxer_answer_and_interrupt_payloads_reach_native_unchanged` (`answer`) | covered |
| `test_scrubs_native_identity_but_preserves_machine_paths` | `test_bb_requests.test_mutation_gets_exact_stdin_and_no_calling_agent_identity` | covered |
| `test_rejects_remote_alias_options_controls_and_unknown_session` | `test_bb_requests.test_unknown_session_fails_without_native_execution`, `test_bb_session_validation.test_aliases_options_and_control_characters_never_become_arguments` | covered |
| `test_shell_syntax_in_exact_known_name_is_literal_argv` | `test_bb_session_validation.test_shell_syntax_in_an_exact_known_name_stays_one_argument` | covered |
| `test_rejects_invalid_request_and_payload_identity_before_spawn` | `test_bb_session_validation.test_identities_no_relay_queues_are_dropped_without_blocking` (handed out by `FakeRelay.inject`), `test_payload_identity_must_match_before_native_execution` | covered |
| `test_completed_duplicate_replays_native_result_without_execution` | `test_bb_requests.test_duplicate_identity_replays_without_native_execution` | covered |
| `test_timeout_is_uncertain_for_mutation_and_never_retried` | `test_bb_native_limits.test_a_mutation_out_of_its_budget_is_uncertain_and_never_retried` (the delivery setting's 10 s budget); the 45 s default: `test_the_default_native_timeout_through_the_rust_test_hook`, Rust only | covered |
| `test_timeout_read_fails_without_marking_mutation` | `test_bb_native_limits.test_a_read_out_of_its_budget_fails_and_its_child_is_killed` (the 2 s budget of `hgs history`) | covered |
| `test_output_bounds_and_safe_native_errors` | `test_bb_native_limits.test_native_output_is_capped_at_eight_mebibytes`, `test_failing_or_invalid_native_output_reaches_no_receipt` | covered |
| `test_cancelled_mutation_is_durable_and_uncertain` | `test_bb_signals` covers the process-level stop (SIGTERM, divergence h), which in Python leaves the row running for restart recovery; the cancellation itself runs only on SIGINT, which awaits an owner decision | partial |
| `test_abrupt_process_exit_after_native_execution_cannot_repeat_write` | `test_bb_restart.test_crash_after_native_success_restarts_as_uncertain_without_repeating` | covered |
| `test_journal_excludes_second_connector` | `test_bb_journal.test_second_connector_on_the_same_journal_is_refused`, `test_first_start_creates_a_private_journal_bound_to_its_credentials` | covered |
| `test_outbox_survives_restart_and_retries_identical_result` | `test_bb_restart.test_undelivered_result_survives_restart_and_uploads_unchanged` | covered |
| `test_redirect_does_not_forward_token_or_repeat_result_elsewhere` | `test_bb_requests.test_redirects_are_never_followed` | covered |
| `test_result_conflict_is_retired_and_does_not_block_subsequent_commands` | `test_bb_requests.test_result_conflict_is_retired_without_blocking_later_requests` | covered |
| `test_result_too_large_is_failed_read_or_uncertain_write` | `test_bb_native_limits.test_a_result_fits_only_within_the_relay_limit` | covered |
| `test_journal_refuses_config_switch_and_unsafe_files_without_chmod` | `test_bb_journal.test_other_credentials_never_take_over_a_journal`, `test_unsafe_state_directory_is_refused_without_repair`, `test_unsafe_journal_files_are_refused_without_repair` | covered |
| `test_attention_round_robin_budget_and_exact_identity_cache` | `test_bb_computer.AttentionTests.test_two_live_codex_sessions_are_inspected_per_heartbeat` | covered |
| `test_attention_marks_only_optional_codex_async_questions` | `test_bb_computer.AttentionTests.test_optional_codex_async_questions_carry_one_boolean` | covered |
| `test_https_and_explicit_loopback_only_http` | `test_bb_startup.test_invalid_configuration_is_refused_before_any_state_or_network`, `test_loopback_http_requires_its_explicit_development_flag`; the full matrix: P2 `config.jsonl` | covered |

The poison pill of divergence b is pinned for Python by
`test_bb_requests.test_rejected_result_blocks_later_claims_in_python` and by
receipts the relay refuses on their own in
`test_bb_native_limits.RefusedResultTests`, whose
`test_rust_retires_a_stored_result_the_relay_keeps_refusing` runs only against
Rust, since its retry spacing needs the `test-hooks` clock scale.

### `test_connector_retention.py`

| In-process test | Black-box coverage | Status |
|---|---|---|
| `JournalRetentionTests.test_delivered_inspect_expires_and_is_safe_to_read_again` | `test_bb_retention.test_delivered_read_expires_after_an_hour_and_may_execute_again` | covered |
| `JournalRetentionTests.test_mutation_content_scrubs_without_losing_durable_identity` | `test_bb_retention.test_delivered_mutation_is_scrubbed_after_seven_days_and_never_repeats` | covered |
| `JournalRetentionTests.test_pending_results_never_expire` | `test_bb_retention.test_undelivered_results_never_expire` | covered |
| `JournalRetentionTests.test_legacy_migration_is_conservative_and_preserves_outbox` | `test_bb_journal.test_legacy_journal_is_migrated_in_place_and_keeps_its_outbox` | covered |
| `JournalRetentionTests.test_retained_read_limit_does_not_consume_mutation_capacity` | `test_bb_session_validation.test_retained_reads_at_their_cap_never_take_mutation_capacity`; the mutation cap: P2 `journal/caps.sql` | covered |
| `ConnectorRetentionTests.test_changed_payload_never_executes_and_expired_result_is_explicit` | `test_bb_retention.test_delivered_mutation_is_scrubbed_after_seven_days_and_never_repeats` | covered |

### `test_mobile_actions.py` (`LifecycleTransportTests`)

| In-process test | Black-box coverage | Status |
|---|---|---|
| `test_fixed_dispatch_payload_and_identical_retry_never_repeat` | `test_bb_actions.test_each_action_reaches_native_once_with_its_scoped_payload` | covered |
| `test_authoritative_rename_target_can_be_inspected_before_heartbeat` | `test_bb_actions.test_a_renamed_target_can_be_inspected_before_the_next_heartbeat` | covered |
| `test_native_definite_failed_and_uncertain_status_are_preserved` | `test_bb_actions.test_native_failed_and_uncertain_statuses_are_preserved` | covered |
| `test_unknown_ack_or_changed_identity_is_uncertain_without_replay` | `test_bb_actions.test_mismatched_acknowledgements_are_uncertain_and_never_repeated` | covered |
| `test_old_cli_and_invalid_payload_never_invoke_mutator` | `test_bb_actions.test_hgs_without_the_session_action_abi_advertises_and_runs_nothing`; the payloads: the `control` validation vectors in `test_bb_session_validation.test_rejected_vectors_fail_with_their_receipt_and_run_nothing` | covered |
| `test_archive_restore_selects_exact_pair_with_fresh_cache` | `test_bb_actions.test_restore_targets_only_the_exact_fresh_archive`; a snapshot older than 45 s: vector `restore/archive-stale` and a Rust unit test with an injected clock | partial |
| `test_bounded_help_probe_does_not_infer_support_from_arbitrary_text` | `test_bb_actions.test_hgs_without_the_session_action_abi_advertises_and_runs_nothing` (help text that names the command without its marker), `test_each_action_reaches_native_once_with_its_scoped_payload` (the marker); the probe's 3 s budget: Rust unit test | partial |

### `test_mobile_attachments.py` (inline attachments of a send)

The other `AttachmentRelayTests` are relay cases, mapped in
[`ORACLE.md`](../relay/tests/ORACLE.md).

| In-process test | Black-box coverage | Status |
|---|---|---|
| `AttachmentValidationTests.test_paths_urls_controls_and_unknown_fields_are_rejected` | `test_bb_send.test_attachments_and_text_beyond_the_limits_never_reach_native`; the attachment vectors of `validation/conversation.jsonl` in `test_bb_session_validation.test_rejected_vectors_fail_with_their_receipt_and_run_nothing` | covered |
| `AttachmentValidationTests.test_native_file_and_total_limits_and_utf8_text` | `test_bb_send.test_attachments_at_the_native_limits_reach_native_unchanged`, `test_attachments_and_text_beyond_the_limits_never_reach_native` (`sizes.jsonl` `send-text-64KiB` and one byte more) | covered |
| `AttachmentRelayTests.test_connector_accepts_large_request_response_independently` | `test_bb_send.test_attachments_at_the_native_limits_reach_native_unchanged` (a request above 26 MiB), `test_attachments_and_text_beyond_the_limits_never_reach_native` | covered |

### `test_mobile_child_send.py` (`ChildSendTests`)

| In-process test | Black-box coverage | Status |
|---|---|---|
| `test_exact_parent_and_continuable_child_verified_before_one_native_send` | `test_bb_send.test_child_send_checks_the_exact_parent_and_child_before_one_send` | covered |
| `test_other_provider_or_child_readonly_identity_never_mutates` | same | covered |
| `test_wrong_child_receipt_is_uncertain_and_no_replay` | same | covered |
| `test_arbitrary_child_paths_and_context_continuations_rejected` | `test_bb_send.test_attachments_and_text_beyond_the_limits_never_reach_native` | covered |

### `test_mobile_context.py` (`ContextConnectorTests`, `ContextFixtureTests`)

`ContextRelayTests` has no connector case;
[`ORACLE.md`](../relay/tests/ORACLE.md) maps it.

| In-process test | Black-box coverage | Status |
|---|---|---|
| `ContextConnectorTests.test_model_settings_use_the_same_utf8_bound_as_native_launch` | `test_bb_controls.test_a_model_name_is_bounded_by_its_utf8_bytes_like_a_native_launch` | covered |
| `ContextConnectorTests.test_fixed_argv_and_strict_native_acknowledgement` | `test_bb_send.test_context_acknowledgements_must_match_or_stay_uncertain`, `test_compaction_continuation_sends_only_after_native_completion` | covered |
| `ContextConnectorTests.test_invalid_payload_archive_and_unknown_operation_never_spawn` | the context vectors in `test_bb_session_validation.test_rejected_vectors_fail_with_their_receipt_and_run_nothing`, `test_aliases_options_and_control_characters_never_become_arguments` (`compact-context`) | covered |
| `ContextConnectorTests.test_fresh_identity_and_native_support_rejection_before_mutation` | `test_bb_send.test_context_preflight_identity_and_support_gate_the_command` | covered |
| `ContextConnectorTests.test_mismatched_or_unknown_ack_is_uncertain_and_never_replayed` | `test_bb_send.test_context_acknowledgements_must_match_or_stay_uncertain` | covered |
| `ContextConnectorTests.test_timeout_after_invocation_and_duplicate_claim_do_not_replay` | the timeout path: `test_bb_native_limits.test_a_mutation_out_of_its_budget_is_uncertain_and_never_retried`; a relay never redelivers a running claim, and a restart turns it uncertain (`test_bb_restart`); "request is already in progress": vector `replay_in_progress_returned` | partial |
| `ContextConnectorTests.test_continuation_requires_exact_native_completion_and_idle` | `test_bb_send.test_compaction_continuation_sends_only_after_native_completion` | covered |
| `ContextConnectorTests.test_ordinary_old_send_needs_no_context_preflight` | `test_bb_send.test_text_and_attachments_reach_native_unchanged_without_a_preflight` | covered |
| `ContextConnectorTests.test_archived_only_name_never_allows_a_context_command` | `test_bb_send.test_an_archived_only_name_never_takes_a_context_command` | covered |
| `ContextFixtureTests.test_fixture_confirmed_compact_continue_exactly_one_compact_and_send` | `test_bb_send.test_compaction_continuation_sends_only_after_native_completion` | covered |
| `ContextFixtureTests.test_fixture_pending_and_failed_compaction_do_not_send` | same | covered |
| `ContextFixtureTests.test_clear_confirmed_receipt_old_identity_and_fresh_new_conversation` | `test_bb_send.test_context_acknowledgements_must_match_or_stay_uncertain`; the new conversation afterwards is `fixture_hgs.py` state | covered |
| `ContextFixtureTests.test_fixture_telemetry_passthrough_capabilities_and_all_states` | the operations advertised per help marker: `test_bb_actions` (`advertised()` from P2 `operations.json`); inspection passthrough: `test_bb_requests.test_read_executes_once_and_uploads_its_result` | covered |
| `ContextFixtureTests.test_fixture_uncertain_receipt_records_only_one_mutation` | `test_bb_send.test_context_acknowledgements_must_match_or_stay_uncertain` | covered |
| `ContextFixtureTests.test_fixture_submitted_clear_does_not_change_identity` | same | covered |

### `test_mobile_controls.py` (`CoreConnectorTests`, `CoreFixtureTests`)

`CoreRelayTests` has no connector case;
[`ORACLE.md`](../relay/tests/ORACLE.md) maps it.

| In-process test | Black-box coverage | Status |
|---|---|---|
| `CoreConnectorTests.test_queue_exact_fresh_id_and_native_receipt_with_no_duplicate_delivery` | `test_bb_controls.test_send_now_sends_only_the_exact_fresh_queue` | covered |
| `CoreConnectorTests.test_settings_default_effort_and_scheduled_receipt_are_preserved` | `test_bb_controls.test_settings_apply_or_schedule_only_the_exact_change` | covered |
| `CoreConnectorTests.test_scheduled_ack_without_native_pending_id_is_uncertain_without_replay` | same | covered |
| `CoreConnectorTests.test_wrong_queue_or_model_ack_is_uncertain_and_never_replayed` | `test_bb_controls.test_send_now_sends_only_the_exact_fresh_queue`, `test_settings_apply_or_schedule_only_the_exact_change` | covered |
| `CoreConnectorTests.test_process_argv_is_fixed_and_failure_read_vs_stop_is_distinct` | `test_bb_controls.test_process_commands_use_fixed_arguments_and_exact_acknowledgements` | covered |
| `CoreConnectorTests.test_exact_archived_process_read_never_targets_live_or_allows_stop` | same, and vector `process_stop/archive` | covered |
| `CoreConnectorTests.test_scoped_incremental_and_child_inspection_identity` | `test_bb_controls.test_scoped_and_child_inspection_check_the_exact_identity` | covered |
| `CoreConnectorTests.test_invalid_fields_tokens_or_extensions_never_spawn` | the `conversation` validation vectors in `test_bb_session_validation.test_rejected_vectors_fail_with_their_receipt_and_run_nothing` | covered |
| `CoreFixtureTests.test_synthetic_queue_settings_child_process_and_cold_message_window` | the tests above; the cold message window is `fixture_hgs.py` behaviour | covered |

### `test_mobile_delivery.py`

| In-process test | Black-box coverage | Status |
|---|---|---|
| `MobileDeliveryConnectorTests.test_exact_native_command_and_receipt_are_returned_once` | `test_bb_controls.test_each_value_runs_its_fixed_command_once`; its 10 s budget: `test_bb_native_limits.test_a_mutation_out_of_its_budget_is_uncertain_and_never_retried` | covered |
| `MobileDeliveryConnectorTests.test_mismatched_acknowledgement_is_uncertain_and_never_replayed` | `test_bb_controls.test_a_mismatched_acknowledgement_is_uncertain_and_never_repeated` | covered |
| `MobileDeliveryConnectorTests.test_invalid_values_sessions_or_fields_never_spawn` | `test_bb_controls.test_invalid_values_or_fields_never_reach_native` (empty, missing and non-object payloads, extra fields), the `set_mobile_delivery` vectors in `test_bb_session_validation.test_rejected_vectors_fail_with_their_receipt_and_run_nothing` | covered |
| `MobileDeliveryConnectorTests.test_unsupported_native_cli_fails_before_any_mutation` | `test_bb_controls.test_hgs_without_the_preference_abi_advertises_and_runs_nothing` | covered |
| `MobileDeliveryFixtureTests.test_only_the_gateway_advertises_and_its_snapshot_reports_the_value` | `test_bb_controls.test_each_value_runs_its_fixed_command_once` (advertised), `test_the_snapshot_reports_the_setting_with_or_without_an_hgs_version`; a peer never advertising it: `test_bb_peer_operations.test_the_delivery_setting_belongs_to_the_gateway` | covered |

### `test_mobile_history_recovery.py` (`HistoryRecovery`)

| In-process test | Black-box coverage | Status |
|---|---|---|
| `test_history_is_read_only_scoped_and_old_nodes_deny` | `test_bb_history_recovery.test_hgs_without_the_history_abi_runs_nothing`; the fields: the `history/*` vectors in `test_bb_session_validation.test_rejected_vectors_fail_with_their_receipt_and_run_nothing` and, with the ABI offered, `test_bb_history_recovery.test_a_page_of_anything_else_fails_closed`; the read class: `test_bb_read_classes` | covered |
| `test_fixture_pages_over_500_and_wrong_cursor_scope_rejected` | cursors pass unchanged: `test_bb_history_recovery.test_history_reads_pass_their_scoped_payload_unchanged`, `test_a_cursor_read_is_never_repeated_for_indexing`; paging is `fixture_hgs.py` behaviour | covered |
| `test_wrong_history_ack_and_archive_match_fail_closed` | `test_bb_history_recovery.test_a_page_of_anything_else_fails_closed` | covered |
| `test_recovery_five_field_identity_and_duplicate_no_repeat` | `test_bb_history_recovery.test_recovery_action_checks_the_waiting_job_then_acts_once`, `test_a_changed_job_or_a_mismatched_acknowledgement_never_repeats` | covered |
| `test_head_locally_advances_bounded_index_and_partial_stops` | `test_bb_history_recovery.test_a_head_read_follows_local_indexing_while_it_progresses`; the 2 s budget: `test_bb_native_limits.test_a_read_out_of_its_budget_fails_and_its_child_is_killed` | covered |
| `test_history_cursor_reads_are_not_repeated_for_indexing` | `test_bb_history_recovery.test_a_cursor_read_is_never_repeated_for_indexing` | covered |
| `test_native_recovery_uncertain_preserved` | `test_bb_history_recovery.test_recovery_action_checks_the_waiting_job_then_acts_once` | covered |

### `test_mobile_read.py`

`MarkReadValidation.test_relay_oracle_validates_the_same_envelope` is a relay
case, mapped in [`ORACLE.md`](../relay/tests/ORACLE.md).

| In-process test | Black-box coverage | Status |
|---|---|---|
| `MarkReadValidation.test_payload_is_exact_identity_with_nullable_marks` | `test_bb_controls.test_invalid_marks_or_targets_never_reach_native`, `test_mark_read_returns_the_native_receipt_once` (null marks) | covered |
| `MarkReadValidation.test_results_must_be_native_read_receipts` | `test_bb_controls.test_only_a_native_read_receipt_of_this_conversation_completes`, `test_mark_read_returns_the_native_receipt_once` | covered |
| `MarkReadConnector.test_maps_to_native_read_and_returns_its_stdout_once` | `test_bb_controls.test_mark_read_returns_the_native_receipt_once` | covered |
| `MarkReadConnector.test_not_found_is_a_completed_result` | same | covered |
| `MarkReadConnector.test_invalid_or_unknown_targets_never_spawn` | `test_bb_controls.test_invalid_marks_or_targets_never_reach_native` | covered |
| `MarkReadConnector.test_old_native_without_read_abi_is_not_advertised_or_executed` | `test_bb_controls.test_hgs_without_the_read_abi_advertises_and_runs_nothing`, `test_mark_read_returns_the_native_receipt_once` (advertised with its marker) | covered |
| `MarkReadConnector.test_old_native_snapshot_omits_mark_read` | `test_bb_controls.test_hgs_without_the_read_abi_advertises_and_runs_nothing` | covered |
| `MarkReadConnector.test_mark_read_is_read_class` | failed, never uncertain, receipts in `test_bb_controls.test_only_a_native_read_receipt_of_this_conversation_completes`; P2 `operations.json` | covered |
| `MarkReadFixture.test_native_exit_status_one_still_returns_not_found` | `test_bb_controls.test_mark_read_returns_the_native_receipt_once` | covered |
| `MarkReadFixture.test_success_receipt_requires_exit_status_zero` | `test_bb_controls.test_only_a_native_read_receipt_of_this_conversation_completes` | covered |

### `test_mobile_voice.py`

`VoiceValidationTests` call the relay's `validate_request`; the connector's own checks run through the voice vectors of `validation/speech.jsonl`.

| In-process test | Black-box coverage | Status |
|---|---|---|
| `VoiceValidationTests.test_operations_keep_session_and_exact_native_identity` | the voice vectors in `test_bb_session_validation.test_rejected_vectors_fail_with_their_receipt_and_run_nothing` and `test_bb_speech.SpeechJobTests.test_vector_rejections_never_reach_native` | covered |
| `VoiceValidationTests.test_routes_are_machine_identities_not_caller_aliases` | same | covered |
| `VoiceConnectorTests.test_acceptance_is_scoped_durable_and_duplicate_never_reinvokes_native` | `test_bb_voice.test_voice_operations_reach_native_with_their_action_once` | covered |
| `VoiceConnectorTests.test_wrong_conversation_ack_is_uncertain_and_not_replayed` | `test_bb_voice.test_a_mismatched_voice_acknowledgement_never_repeats` | covered |
| `VoiceConnectorTests.test_old_host_rejects_without_native_and_status_read_is_short_lived` | `test_bb_voice.test_hgs_without_the_voice_abi_advertises_and_runs_nothing`, `test_a_delivered_status_read_is_kept_only_two_minutes`, `test_bb_speech.SpeechConfigurationTests.test_speech_and_its_configuration_are_separate_capabilities`, `test_bb_read_classes` | covered |

### Other modules with connector cases

| Module | Coverage |
|---|---|
| `test_mobile_actions.py`, `test_mobile_attachments.py`, `test_mobile_child_send.py`, `test_mobile_context.py`, `test_mobile_controls.py`, `test_mobile_delivery.py`, `test_mobile_history_recovery.py`, `test_mobile_read.py`, `test_mobile_voice.py` | H2: mapped per test in the tables above |
| `test_mobile_archives.py`, `test_mobile_terminal.py`, `test_mobile_history.py`, `test_mobile_attachment_read.py`, `test_mobile_launch.py`, `test_mobile_project_launch.py`, `test_mobile_projects.py`, `test_mobile_directory_create.py`, `test_mobile_worktrees.py`, `test_mobile_accounts.py`, `test_mobile_speech.py`, `test_mobile_speech_config.py` | H3: mapped per test under [Computer, read and media operations](#computer-read-and-media-operations-h3) |
| `test_connector_peers.py`, `test_peer_end_to_end.py`, the peer classes of `test_mobile_directory_create.py` and `test_mobile_worktrees.py` | Mapped under [Peers and the one-hop gateway](#peers-and-the-one-hop-gateway) |
| `test_computer_file_read.py` | `test_bb_file_read`: one capture per open served by digest (`test_ranges_follow_one_capture_of_the_file`, which also checks the native stdin; the 512 KiB bound and identity rules through the `file_read` vectors in `test_bb_session_validation`), a new open reads the current file, a changed or unreadable file fails as a read, and a CLI without the marker; its two-minute class in `test_bb_read_classes`. The 10 MiB real-CLI case stays in-process; the peer file half of the peer case is left to the peer suites (C6), the existing-attachment half is `test_bb_peer_operations.test_peer_attachment_read_reads_the_file_through_the_peer` |
| `test_managed_relay.py` | P2 `identities.json` |
| `test_end_to_end.py`, `test_presence_end_to_end.py` | Real-relay end-to-end tests on PostgreSQL |

Connector halves inside relay classes have rows in the tables of their
modules (for example
`AttachmentRelayTests.test_connector_accepts_large_request_response_independently`
and `TerminalRetention`); the rest of those classes are relay cases.

### Computer, read and media operations (H3)

These modules take their expected texts from the golden vectors through
`bb_operations.py`: help markers from `operations.json`, rejections from
`validation/*.jsonl` (each must fail with its recorded message and reach no
native command), the `execute` runs of `projections.jsonl` (native argv,
stdin and the stored receipt) and boundaries from `sizes.jsonl`. They run on
the vectors' synthetic computer: 74 tests in 10 modules, 72-74 s in the runs
above, of which `test_bb_computer.py` takes 31 s because it waits for the 15 s
project refresh. `test_bb_attachment_read.ScriptedAnswerTests` checks the
scripted answers these suites add and, like `test_bb_harness.py`, always
runs. The real-CLI cases reach the built hgs through the scripted hgs's
`exec` rule, so only `--help`, `dirs` and `speech` reach it. Python reports a refused worktree
catalog or creation receipt with its generic "connector could not record
native outcome", which `test_bb_worktrees` pins.

`test_mobile_attachments.py`, `test_mobile_history_recovery.py` and
`test_mobile_voice.py` are mapped in their own tables above. H2 and H3 both
wrote suites for them; the integration kept one module per subject: H3's
inline attachment, history page and voice message tests were merged into
`test_bb_send`, `test_bb_history_recovery.HistoryTests` and
`test_bb_voice.VoiceMessageTests` (keeping the cases those lacked), and
H3's `test_bb_history_pages` kept only its inspection bounds, renamed
`test_bb_inspection_bounds`.

| In-process test | Black-box coverage | Status |
|---|---|---|
| `test_mobile_terminal.TerminalTransport.test_fixed_raw_dispatch_and_duplicate_never_repeat` | `test_bb_terminal.test_input_runs_once_after_a_fresh_preflight`, `test_vector_screens_and_input_reach_fixed_native_argv` | covered |
| `test_mobile_terminal.TerminalTransport.test_expired_wrong_binding_and_old_cli_fail_before_handoff` | `test_bb_terminal.test_input_fails_closed_before_handoff`, `test_without_the_terminal_abi_nothing_runs` | covered |
| `test_mobile_terminal.TerminalTransport.test_ambiguous_native_ack_never_replays` | `test_bb_terminal.test_unconfirmed_input_is_uncertain_and_never_replays` | covered |
| `test_mobile_terminal.TerminalTransport.test_payload_is_literal_bounded_and_rejects_hidden_control` | `test_bb_terminal.test_literal_text_is_bounded_and_rejects_hidden_controls`, `test_every_allowlisted_key_and_no_other` | covered |
| `test_mobile_terminal.TerminalFixture.test_real_fixture_subprocess_roundtrip_does_not_execute_text` | `test_bb_terminal.test_input_runs_once_after_a_fresh_preflight` (shell syntax stays literal stdin) | covered |
| `test_mobile_terminal.TerminalRateBudget.test_four_frames_per_second_receipts_and_node_uplink_for_two_minutes` | Relay only: `operation_contracts::terminal_frames_fit_each_credentials_rate_budget` | covered |
| `test_mobile_terminal.TerminalJournalRetention.test_sustained_screens_leave_the_journal_and_inputs_stay` (the Journal half, split out of `TerminalRetention`) | `test_bb_terminal.test_screens_leave_the_journal_after_two_minutes_and_inputs_stay`, `test_bb_read_classes.test_reads_leave_the_journal_by_class_and_mutations_stay`; the read cap that sustained reads meet without taking mutation capacity: P2 `journal/caps.sql` in `connector/tests/journal_vectors.rs` (`python_written_and_legacy_journals_reopen_as_python_reopens_them`) | covered |
| `test_mobile_terminal.TerminalRetention.test_sustained_reads_and_abandoned_results_expire_without_mutation_eviction` (the relay half) | Relay only: `operation_contracts::sustained_screens_and_abandoned_reads_expire_without_evicting_input`, `terminal_input_needs_a_fresh_computer_and_expires_before_delivery` | covered |
| `test_mobile_history.HistoryTests.test_large_unicode_history_keeps_full_question_identity_and_newest_user` | `test_bb_inspection_bounds.test_large_inspection_loses_only_whole_oldest_rows_and_says_so` | covered |
| `test_mobile_history.HistoryTests.test_mandatory_metadata_too_large_fails_without_cropping` | `test_bb_inspection_bounds.test_oversized_disclosures_fail_without_cropping` | covered |
| `test_mobile_history.HistoryTests.test_small_results_unchanged_and_no_event_is_partially_cropped` | `test_bb_inspection_bounds.test_small_inspection_is_unchanged`, `test_vector_inspections_keep_their_recorded_rows` | covered |
| `test_mobile_history.HistoryConnectorTests.test_connector_read_returns_bounded_history_or_clear_metadata_failure` | the same two inspection tests | covered |
| `test_mobile_attachment_read.ReadValidationTests.test_exact_receipt_position_and_bounded_range` | `test_bb_attachment_read.AttachmentReadTests.test_invalid_positions_and_fields_never_reach_native`, `test_foreign_identities_and_answers_are_refused` (with the `conversation.jsonl` cases) | covered |
| `test_mobile_attachment_read.ReadValidationTests.test_operation_is_a_short_lived_read_outside_the_indexed_read_text` | `test_bb_read_classes.test_reads_leave_the_journal_by_class_and_mutations_stay`; the `READ_SQL` text: `operations.json` | covered |
| `test_mobile_attachment_read.ReadValidationTests.test_memory_cache_is_bounded_least_recent_and_expires` | `test_bb_attachment_read.AttachmentReadTests.test_verified_files_are_kept_least_recently_used_up_to_three`, `test_the_cache_budget_is_bytes_not_files`; the 120 s expiry needs an injected clock: Rust unit test (C4) | partial |
| `test_mobile_attachment_read.ConnectorReadTests.test_ranges_return_exact_bytes_digest_and_reuse_one_native_read` | `test_bb_attachment_read.AttachmentReadTests.test_ranges_return_exact_bytes_from_one_native_read`, `test_vector_reads`, `test_native_output_is_capped_at_one_encoded_file` | covered |
| `test_mobile_attachment_read.ConnectorReadTests.test_archived_and_child_identity_reach_native_flags` | `test_bb_attachment_read.AttachmentReadTests.test_archived_and_child_reads_name_their_flags` | covered |
| `test_mobile_attachment_read.ConnectorReadTests.test_foreign_session_identity_and_answers_are_refused` | `test_bb_attachment_read.AttachmentReadTests.test_foreign_identities_and_answers_are_refused` | covered |
| `test_mobile_attachment_read.ConnectorReadTests.test_unsupported_native_is_not_advertised_or_executed` | `test_bb_attachment_read.AttachmentReadTests.test_without_the_attachment_abi_nothing_runs` | covered |
| `test_mobile_attachment_read.ConnectorReadTests.test_delivered_chunks_leave_the_journal_like_terminal_snapshots` | `test_bb_read_classes.test_reads_leave_the_journal_by_class_and_mutations_stay` | covered |
| `test_mobile_speech.SpeechValidationTests.test_all_computer_operations_have_exact_shapes` | `test_bb_speech.SpeechJobTests.test_vector_rejections_never_reach_native` | covered |
| `test_mobile_speech.SpeechValidationTests.test_invalid_audio_identity_and_extra_fields_are_rejected` | the same | covered |
| `test_mobile_speech.SpeechValidationTests.test_maximum_audio_has_bounded_valid_encoding` | `test_bb_speech.SpeechJobTests.test_recordings_are_bounded_to_two_minutes` | covered |
| `test_mobile_speech.SpeechConnectorTests.test_submit_returns_job_without_waiting_for_inference_and_never_replays` | `test_bb_speech.SpeechJobTests.test_submit_returns_its_job_once`, `test_vector_submit_keeps_no_transcript` | covered |
| `test_mobile_speech.SpeechConnectorTests.test_mismatched_job_is_uncertain_and_old_cli_never_runs_speech` | `test_bb_speech.SpeechJobTests.test_unconfirmed_jobs_are_uncertain_and_never_replay`, `SpeechConfigurationTests.test_speech_and_its_configuration_are_separate_capabilities` | covered |
| `test_mobile_speech.SpeechConnectorTests.test_status_is_exact_and_read_receipts_are_pruned` | `test_bb_speech.SpeechJobTests.test_reads_and_cancellation`, `test_bb_read_classes` | covered |
| `test_mobile_speech.SpeechHttpServiceTests.test_maximum_recording_crosses_http_cli_socket_once_and_is_erased` | Connector half: `test_bb_speech.SpeechJobTests.test_recordings_are_bounded_to_two_minutes`; relay, CLI and speech service: R2 end to end | partial |
| `test_mobile_speech.SpeechHttpServiceTests.test_status_and_cancel_are_bound_to_phone_and_computer` | Relay only: `operation_contracts::speech_and_management_status_belong_to_the_original_phone_and_computer`, `contracts::speech_jobs_preserve_owner_identity_and_discard_audio_after_ack` | covered |
| `test_mobile_speech_config.SpeechConfigValidationTests.test_voice_routes_preserve_optional_exact_profile_revision` | `test_bb_speech.SpeechJobTests.test_vector_rejections_never_reach_native` | covered |
| `test_mobile_speech_config.SpeechConfigValidationTests.test_all_operations_are_computer_scoped_and_exact` | the same | covered |
| `test_mobile_speech_config.SpeechConfigValidationTests.test_plaintext_credentials_and_unbounded_envelopes_are_rejected` | the same | covered |
| `test_mobile_speech_config.SpeechConfigValidationTests.test_receipts_reject_unexpected_content_and_wrong_identity` | `test_bb_speech.SpeechConfigurationTests.test_unexpected_native_content_never_becomes_a_receipt`, `test_reads_are_allowlisted_and_bound_to_the_computer` | covered |
| `test_mobile_speech_config.SpeechConfigConnectorTests.test_capability_is_independent_and_request_is_not_replayed` | `test_bb_speech.SpeechConfigurationTests.test_sealed_change_runs_once_and_keeps_only_its_receipt`, `test_speech_and_its_configuration_are_separate_capabilities` | covered |
| `test_mobile_speech_config.SpeechConfigConnectorTests.test_malformed_result_never_persists_a_key_or_replays` | `test_bb_speech.SpeechConfigurationTests.test_unexpected_native_content_never_becomes_a_receipt` | covered |
| `test_mobile_speech_config.SpeechConfigConnectorTests.test_redacted_management_reads_expire` | `test_bb_read_classes.test_reads_leave_the_journal_by_class_and_mutations_stay` | covered |
| `test_mobile_speech_config.SpeechConfigConnectorTests.test_transport_stderr_does_not_enter_durable_receipts` | `test_bb_speech.SpeechConfigurationTests.test_unexpected_native_content_never_becomes_a_receipt` | covered |
| `test_mobile_speech_config.SpeechConfigHttpTests.test_sealed_mutation_is_erased_and_keeps_no_replay_receipt` | Relay only: `contracts::speech_config_receipts_erase_bodies_and_expire_reads`; the plaintext key refused before persistence: `operation_contracts::speech_and_management_status_belong_to_the_original_phone_and_computer` | covered |
| `test_mobile_speech_config.SpeechConfigHttpTests.test_status_is_owned_by_original_phone_and_computer` | Relay only: `operation_contracts::speech_and_management_status_belong_to_the_original_phone_and_computer` | covered |
| `test_mobile_speech_config.SpeechConfigHttpTests.test_real_cli_management_roundtrip_without_inference_runtime` | Connector and CLI: `test_bb_speech.SpeechConfigurationTests.test_real_cli_manages_a_profile_without_inference`; relay half: R2 | covered |
| `test_mobile_launch.LaunchTransport.test_exact_marker_required_and_duplicate_cannot_launch_again` | `test_bb_launch.LaunchTests.test_launch_runs_once_and_returns_its_exact_target`, `test_without_the_launch_abi_nothing_runs` | covered |
| `test_mobile_launch.LaunchTransport.test_preflight_old_cli_invalid_fields_and_wrong_provider_do_not_launch` | `test_bb_launch.LaunchTests.test_failed_preflights_never_launch`, `test_without_the_launch_abi_nothing_runs` | covered |
| `test_mobile_launch.LaunchTransport.test_native_handoff_failure_is_uncertain_and_never_replayed` | `test_bb_launch.LaunchTests.test_an_unconfirmed_launch_is_uncertain_and_never_repeats` (also the 5 s verification) | covered |
| `test_mobile_launch.LaunchTransport.test_opencode_exact_offered_model_reaches_fixed_native_launch_once` | `test_bb_launch.LaunchTests.test_opencode_models_must_still_be_offered` | covered |
| `test_mobile_launch.LaunchTransport.test_changed_or_other_account_model_blocks_before_directory_or_launch` | the same | covered |
| `test_mobile_launch.LaunchTransport.test_opencode_default_model_does_not_require_or_inject_a_model` | the same | covered |
| `test_mobile_launch.LaunchTransport.test_model_route_is_bounded_and_specific_to_native_opencode` | `test_bb_launch.LaunchTests.test_failed_preflights_never_launch` (`launch/model-*` vectors) | covered |
| `test_mobile_launch.LaunchTransport.test_fixture_catalog_launch_then_inspect_and_send_new_exact_target` | `test_bb_launch.LaunchTests.test_launch_runs_once_and_returns_its_exact_target`; inspecting and messaging the new target before the next listing has no black-box test yet (a renamed target: `test_bb_actions.test_a_renamed_target_can_be_inspected_before_the_next_heartbeat`) | partial |
| `test_mobile_launch.CatalogPrivacy.test_inherited_opencode_configuration_cannot_retarget_connector_calls` | `test_bb_requests.test_mutation_gets_exact_stdin_and_no_calling_agent_identity` (the OpenCode variables are context keys) | covered |
| `test_mobile_launch.CatalogPrivacy.test_cached_limits_are_projected_without_native_errors_or_credentials` | `test_bb_launch.LaunchTests.test_vector_catalog_projection` | covered |
| `test_mobile_launch.CatalogPrivacy.test_only_selected_public_fields_are_forwarded` | `test_bb_launch.LaunchTests.test_vector_catalog_projection`, `ProjectLaunchTests.test_catalog_lists_accessible_projects_with_local_folders` | covered |
| `test_mobile_launch.CatalogPrivacy.test_opencode_models_project_only_bounded_ids_labels_and_provider` | `test_bb_launch.LaunchTests.test_vector_catalog_projection`; the bounds: `projections.jsonl` `model-options` in a Rust unit test (C5) | partial |
| `test_mobile_launch.CatalogPrivacy.test_model_catalog_cannot_exceed_public_byte_bound` | Rust unit test (C5) | planned |
| `test_mobile_project_launch.ProjectLaunchTests.test_successful_placement_and_exact_uuid_retry_do_not_launch_twice` | `test_bb_launch.ProjectLaunchTests.test_assignment_follows_a_confirmed_launch_once` | covered |
| `test_mobile_project_launch.ProjectLaunchTests.test_created_target_retained_after_assignment_failure_uncertainty_or_wrong_ack` | `test_bb_launch.ProjectLaunchTests.test_failed_assignment_keeps_the_created_session` | covered |
| `test_mobile_project_launch.ProjectLaunchTests.test_missing_old_cli_or_changed_project_rejects_before_creation` | `test_bb_launch.ProjectLaunchTests.test_without_the_assignment_abi_a_project_launch_never_starts`, `test_saved_and_related_folders_are_checked_natively` | covered |
| `test_mobile_project_launch.ProjectLaunchTests.test_symlink_saved_folder_uses_fresh_directory_equivalence` | `test_bb_launch.ProjectLaunchTests.test_saved_and_related_folders_are_checked_natively` | covered |
| `test_mobile_project_launch.ProjectCatalogTests.test_exact_profiles_default_and_cached_email_survive_without_paths_or_secrets` | `test_bb_launch.LaunchTests.test_vector_catalog_projection`; profiles with one label: Rust unit test (C5) | partial |
| `test_mobile_project_launch.ProjectCatalogTests.test_project_catalog_has_only_local_folders_and_accessible_groups` | `test_bb_launch.ProjectLaunchTests.test_catalog_lists_accessible_projects_with_local_folders` | covered |
| `test_mobile_project_launch.ProjectCatalogTests.test_project_fields_and_existing_folder_intent_are_validated_before_mutation` | `test_bb_launch.LaunchTests.test_failed_preflights_never_launch` (`launch/project-*` vectors); identifier bounds: Rust unit test (C5) | partial |
| `test_mobile_projects.NativeProjectsTests.test_all_groups_local_folders_and_exact_memberships_only` | `test_bb_computer.ProjectTests.test_projects_carry_local_folders_and_exact_memberships` | covered |
| `test_mobile_projects.NativeProjectsTests.test_uninitialized_catalog_is_stable_unavailable_without_alias_invention` | `test_bb_computer.ProjectTests.test_an_uninitialized_catalog_is_a_stable_local_fallback` | covered |
| `test_mobile_projects.ProjectCacheTests.test_cache_reuses_fifteen_seconds_and_keeps_stale_memberships` | `test_bb_computer.ProjectTests.test_projects_are_read_every_fifteen_seconds_and_kept_when_a_read_fails` | covered |
| `test_mobile_accounts.ProjectionTests.test_native_opencode_account_and_models_keep_configuration_private` | `test_bb_computer.AccountTests.test_account_view_carries_only_public_fields` | covered |
| `test_mobile_accounts.ProjectionTests.test_only_allowlisted_fields_leave_native_projection` | `test_bb_computer.AccountTests.test_usage_is_refreshed_in_the_background_one_account_at_a_time` | covered |
| `test_mobile_accounts.ProjectionTests.test_invalid_numbers_resets_sources_and_auth_status_are_not_coerced` | `projections.jsonl` `usage-hostile` in a Rust unit test (C5) | planned |
| `test_mobile_accounts.ProjectionTests.test_catalog_bound_identity_and_duplicate_validation` | `test_bb_computer.AccountTests.test_an_unavailable_catalog_is_reported_without_native_text`; the identity rules: `account-cache-refuses-unknown-provider` in a Rust unit test (C5) | partial |
| `test_mobile_accounts.ProjectionTests.test_malformed_nested_values_and_huge_numbers_are_safe` | `projections.jsonl` `usage-hostile` in a Rust unit test (C5) | planned |
| `test_mobile_accounts.ProjectionTests.test_actual_ascii_expanded_catalog_bytes_are_bounded` | Rust unit test (C5) | planned |
| `test_mobile_accounts.CacheTests.test_refresh_is_single_flight_nonblocking_and_preserves_native_age` | `test_bb_computer.AccountTests.test_usage_is_refreshed_in_the_background_one_account_at_a_time`; staleness after 300 s needs an injected clock: Rust unit test (C5) | partial |
| `test_mobile_accounts.CacheTests.test_credential_change_and_removal_discard_old_usage_and_late_result` | Needs the 30 s catalog clock: Rust unit test (C5) | planned |
| `test_mobile_accounts.CacheTests.test_fair_rotation_refresh_interval_and_missing_provider` | Rotation: `test_bb_computer.AccountTests.test_usage_is_refreshed_in_the_background_one_account_at_a_time`; the 300 s interval: Rust unit test (C5) | partial |
| `test_mobile_accounts.CacheTests.test_dsh_missing_auth_revision_signout_invalidates_usage` | Needs the 30 s catalog clock: Rust unit test (C5) | planned |
| `test_mobile_accounts.CacheTests.test_failures_retain_stale_values_without_errors_and_shutdown_cancels` | `test_bb_computer.AccountTests.test_usage_is_refreshed_in_the_background_one_account_at_a_time`, `test_an_unavailable_catalog_is_reported_without_native_text`; the cancellation on shutdown: Rust unit test (C5) | partial |
| `test_mobile_accounts.CacheTests.test_connector_snapshot_exposes_capability_without_waiting_for_provider` | `test_bb_computer.AccountTests.test_usage_is_refreshed_in_the_background_one_account_at_a_time`, `test_account_view_carries_only_public_fields` | covered |
| `test_mobile_accounts.CacheTests.test_shutdown_reaps_only_the_owned_account_inspection_child` | Rust unit test (C5); a Python SIGTERM leaves the child running | planned |
| `test_mobile_directory_create.DirectoryCreate.test_fixed_literal_argv_and_exact_parent_receipt_replay_once` | `test_bb_directories.DirectoryTests.test_creation_runs_once_and_returns_only_the_exact_receipt` | covered |
| `test_mobile_directory_create.DirectoryCreate.test_missing_capability_fails_before_native_or_parent_read` | `test_bb_directories.DirectoryTests.test_capability_is_separate_from_launch` | covered |
| `test_mobile_directory_create.DirectoryCreate.test_invalid_wire_fields_paths_and_uuid_are_rejected_without_native` | `test_bb_directories.DirectoryTests.test_an_unconfirmed_parent_never_dispatches_creation` (`directory_create/*` vectors); a non-canonical request UUID cannot be queued: `validation/envelope.jsonl` | covered |
| `test_mobile_directory_create.DirectoryCreate.test_parent_failure_or_malformed_parent_does_not_dispatch_mutation` | `test_bb_directories.DirectoryTests.test_an_unconfirmed_parent_never_dispatches_creation` | covered |
| `test_mobile_directory_create.DirectoryCreate.test_unconfirmed_native_outcome_is_uncertain_and_duplicate_never_retries` | `test_bb_directories.DirectoryTests.test_an_unconfirmed_creation_is_uncertain_and_never_retried`; a mutation out of its budget: `test_bb_native_limits.test_a_mutation_out_of_its_budget_is_uncertain_and_never_retried` (the delivery setting), not a creation | partial |
| `test_mobile_directory_create.DirectoryCreate.test_cancellation_after_dispatch_is_uncertain_without_replay` | Process level: `test_bb_directories.DirectoryTests.test_a_crash_after_creation_never_creates_again`; task cancellation: Rust unit test (C5) | partial |
| `test_mobile_directory_create.DirectoryCreate.test_duplicate_during_native_flight_preserves_one_admission` | Rust unit test (C5): requests run one at a time, so no relay redelivers during the flight | planned |
| `test_mobile_directory_create.DirectoryCreate.test_restart_recovers_inflight_as_mutation_and_retains_identity_after_prune` | `test_bb_directories.DirectoryTests.test_a_crash_after_creation_never_creates_again` | covered |
| `test_mobile_directory_create.DirectoryCreate.test_help_discovery_is_additive_independent_and_downgrade_safe` | `test_bb_directories.DirectoryTests.test_capability_is_separate_from_launch` | covered |
| `test_mobile_directory_create.DirectoryCreate.test_catalog_reports_selected_target_creation_support` | `test_bb_directories.DirectoryTests.test_capability_is_separate_from_launch`, `test_bb_launch.LaunchTests.test_vector_catalog_projection` | covered |
| `test_mobile_directory_create.NativeDirectoryCreate.test_real_native_symlink_parent_literal_folder_and_durable_duplicate` | `test_bb_directories.NativeDirectoryTests.test_real_cli_creates_one_literal_folder_through_a_link` | covered |
| `test_mobile_directory_create.RustDirectoryHttp.test_actual_http_capability_validation_and_receipt_never_reclaim`, `test_actual_http_connector_and_guarded_native_cli_create_one_folder` | Real-relay end to end on PostgreSQL (R2) | planned |
| `test_mobile_directory_create.RustDirectoryHttp.test_actual_http_peer_route_is_exact_and_withdrawal_preserves_uncertainty` | The connector side: `test_bb_peer_operations.test_folders_and_worktrees_are_created_only_on_the_selected_machine`; mapped under [Peer cases in other modules](#peer-cases-in-other-modules) | partial |
| `test_mobile_worktrees.Worktrees.test_confirmed_canonical_destination_and_duplicate_do_not_create_again` | `test_bb_worktrees.test_creation_runs_once_after_fresh_evidence` | covered |
| `test_mobile_worktrees.Worktrees.test_foreign_result_is_uncertain_and_never_replayed` | `test_bb_worktrees.test_an_unconfirmed_creation_is_uncertain_and_never_repeats` | covered |
| `test_mobile_worktrees.Worktrees.test_changed_repository_fails_before_mutation` | `test_bb_worktrees.test_changed_repository_fails_before_creation` | covered |
| `test_mobile_worktrees.Worktrees.test_legacy_local_downgrade_uses_old_safe_namespace` | `test_bb_worktrees.test_vector_catalog_uses_the_state_namespace`, `test_creation_runs_once_after_fresh_evidence` | covered |
| `test_mobile_worktrees.Worktrees.test_nested_independent_repository_and_ordinary_subdir_not_members` | `test_bb_launch.ProjectLaunchTests.test_saved_and_related_folders_are_checked_natively`; nested and stale catalogs: Rust unit test (C5) | partial |
| `test_mobile_worktrees.Worktrees.test_strict_fields_and_result_identity` | `test_bb_worktrees.test_catalog_is_projected_or_refused`, `test_an_unconfirmed_creation_is_uncertain_and_never_repeats` | covered |
| `test_mobile_worktrees.Worktrees.test_repeated_cancellation_drains_only_owned_bounded_create` | Process level: `test_bb_worktrees.test_sigterm_never_cuts_a_creation_short`; repeated task cancellation: Rust unit test (C5) | partial |
| `test_mobile_worktrees.Worktrees.test_cancel_while_spawn_returns_retains_one_owned_process` | Rust unit test (C5) | planned |
| `test_mobile_worktrees.Worktrees.test_related_project_uses_exact_anchor_without_adding_folder` | `test_bb_launch.ProjectLaunchTests.test_saved_and_related_folders_are_checked_natively` | covered |
| `test_mobile_worktrees.WorktreeRestart.test_restart_of_claimed_creation_never_calls_native_again` | `test_bb_worktrees.test_sigterm_never_cuts_a_creation_short` (Python restarts it uncertain without native) | covered |
| `test_mobile_worktrees.PeerWorktrees` (2 tests) | `test_bb_peer_operations.test_folders_and_worktrees_are_created_only_on_the_selected_machine`, `test_bb_peer_routes.test_rebound_and_withdrawn_routes_fail_before_native_work`; mapped per test under [Peer cases in other modules](#peer-cases-in-other-modules) | covered |
| `test_mobile_archives.ArchiveConnectorTests.test_live_and_archive_same_name_use_distinct_native_arguments` | `test_bb_archives.test_a_live_session_and_its_archive_use_distinct_native_arguments` | covered |
| `test_mobile_archives.ArchiveConnectorTests.test_invalid_unknown_and_wrong_name_archives_never_spawn` | `test_bb_archives.test_invalid_unknown_and_renamed_archives_never_spawn` | covered |
| `test_mobile_archives.ArchiveConnectorTests.test_archived_only_name_has_no_legacy_or_mutation_fallback` | `test_bb_archives.test_an_archived_name_has_no_live_or_mutation_fallback` | covered |
| `test_mobile_archives.ArchiveConnectorTests.test_native_missing_or_wrong_archive_identity_fails_without_fallback` | `test_bb_archives.test_answers_for_another_target_fail_without_fallback` | covered |
| `test_mobile_archives.ArchiveConnectorTests.test_expired_archive_snapshot_is_not_used_to_spawn_native_inspection` | `validation/conversation.jsonl` `inspect/archive-stale` (46 s old snapshot) in a Rust unit test with an injected clock (C4); black-box it needs a 45 s wait | planned |
| `test_mobile_archives.ArchiveConnectorTests.test_archive_and_live_same_name_have_distinct_project_assignments` | `test_bb_archives.test_a_live_session_and_its_archive_belong_to_their_own_projects` | covered |
| `test_mobile_archives.ArchiveConnectorTests.test_archive_id_without_state_still_requires_exact_archive_selection` | `test_bb_archives.test_an_archive_id_without_state_is_still_an_archive` | covered |
| `test_mobile_archives.ArchiveConnectorTests.test_ordinary_inspect_rejects_raced_archive_or_different_name_response` | `test_bb_archives.test_answers_for_another_target_fail_without_fallback` | covered |
| `test_mobile_archives.ArchiveRelayTests` (2 tests) | Relay only: `operation_contracts::archive_ids_are_exact_on_reads_and_refused_on_ordinary_mutations`, `archived_rows_never_change_the_live_event_baseline` | covered |
| `test_mobile_archives.ArchiveFixtureTests.test_optional_synthetic_states_and_archive_pass_through` | `test_bb_archives.test_a_live_session_and_its_archive_use_distinct_native_arguments` | covered |
| `test_archive_pages.test_read_class_strict_bounds_and_sessionless_envelope` | the `archive_list` vectors in `test_bb_session_validation.test_rejected_vectors_fail_with_their_receipt_and_run_nothing`; its one-hour class: `test_bb_journal` | covered |
| `test_archive_pages.test_page_provenance_survives_active_snapshot_and_native_recheck_remains` | `test_bb_archives.ArchivePageTests.test_a_page_admits_its_archives_for_later_reads` | covered |
| `test_archive_pages.test_bad_native_pages_never_grant_archive_admission` | `test_bb_archives.ArchivePageTests.test_an_invalid_native_page_fails_as_a_read` | partial: admission after a refused page is a Rust unit test |
| `test_archive_pages.test_uncached_archive_requires_exact_native_lookup_before_any_action` | `test_bb_archives.ArchivePageTests.test_an_archive_no_page_listed_is_looked_up_once`, `test_a_lookup_for_another_target_runs_nothing` | covered |
| `test_archive_pages.test_only_successful_deliverable_archive_reads_extend_admission` | the one-hour clock needs an injected clock: Rust `archives::tests::remembered_archives_expire_and_stay_bounded` | partial |
| `test_archive_pages.test_real_native_pages_stay_lazy_and_admit_exact_archived_inspection` | heartbeat previews and the marker: `test_bb_archives.ArchivePageTests.test_heartbeats_list_live_sessions_and_a_bounded_preview`, `test_a_failed_preview_is_reported_and_admits_nothing`, `test_a_cli_without_the_marker_keeps_the_legacy_listing`; the real CLI stays in-process | partial |

### Peers and the one-hop gateway

Seven modules cover discovery, routes, claims, peer operations and peer
heartbeats: `test_bb_fleet`, `test_bb_peer_routes`, `test_bb_gateway`,
`test_bb_peer_operations`, `test_bb_peer_heartbeats`,
`test_bb_gateway_downgrade` and `test_bb_peer_transport`. Their 32 tests took
8-38 s per module and 158-162 s in all in the runs above.

`gateway_harness.py` scripts a gateway's `hgs` as a fleet: `swarm mobile-peers
--json` answers with a synthetic inventory, `swarm mobile-peer --json` with
each online peer's own listing, help, accounts and session (matched on the
envelope's `via`, target machine and inner argv through the scripted hgs's
`stdin_fields`), and `swarm __mobile-peer-local --json` with the gateway's
own guarded machine. Route IDs, `MAX_PEERS`, the 1 MiB peer snapshot bound,
help markers and receipt texts come from the golden vectors. As the relay
does, `FakeRelay` answers a peer heartbeat with 404 when its route is not in
the last manifest it accepted, and with 409 when that route is offline or
names another machine. Only `test_bb_peer_transport` runs the built
`hgs` (`HGS_TEST_BIN`, `ZERUS_DIRECTORY_TEST_HGS` or `target/debug/hgs`; it
fails in CI without one), behind a synthetic `ssh` that runs the peer's hgs
locally, with stand-ins for tmux and the agent CLIs.

#### `test_connector_peers.py` (`PeerConnectorTests`)

| In-process test | Black-box coverage | Status |
|---|---|---|
| `test_same_session_names_have_separate_guarded_contexts` | `test_bb_gateway.test_equal_session_names_reach_only_their_own_machine` | covered |
| `test_publication_strips_aliases_and_peer_of_peer_inventory` | `test_bb_fleet.test_peers_are_shared_by_route_id_without_their_aliases`, `test_bb_peer_heartbeats.test_peer_heartbeats_carry_only_the_peer_projection`; the forged third-peer route: `test_bb_gateway.test_routes_outside_the_current_inventory_fail_before_native_work` | covered |
| `test_removed_rebound_and_forged_alias_fail_before_native_mutation` | `test_bb_peer_routes.test_rebound_and_withdrawn_routes_fail_before_native_work`, `test_bb_gateway.test_routes_outside_the_current_inventory_fail_before_native_work` (an alias in the route); native refusal of a rebound target: `test_bb_gateway.test_native_guard_refusal_after_dispatch_is_uncertain_and_never_repeated` | covered |
| `test_bound_local_uuid_guard_prevents_execution_after_identity_reset` | `test_bb_gateway.test_native_guard_refusal_after_dispatch_is_uncertain_and_never_repeated`; a gateway route under another machine: `test_bb_gateway.test_routes_outside_the_current_inventory_fail_before_native_work` | covered |
| `test_route_is_in_journal_identity_and_duplicate_survives_withdrawal` | `test_bb_gateway.test_route_is_part_of_the_request_identity` | covered |
| `test_last_verified_offline_peer_is_advertised_but_never_dispatched` | `test_bb_peer_routes.test_unreachable_verified_peer_is_shared_offline_and_never_contacted` (inspect, send and catalog; every operation passes the same route check) | covered |
| `test_changed_verified_identity_replaces_the_offline_route` | `test_bb_peer_routes.test_changed_identity_replaces_the_route_and_unverified_aliases_are_withdrawn` | covered |
| `test_peer_presence_travels_beside_its_snapshot` | `test_bb_peer_heartbeats.test_peer_heartbeats_carry_only_the_peer_projection` | covered |
| `test_restart_claimed_mutation_does_not_replay_peer` | `test_bb_gateway.test_restart_never_repeats_a_claimed_peer_mutation` | covered |
| `test_accounts_history_launch_and_followup_inspection_target_peer` | `test_bb_peer_operations.test_catalog_history_and_project_launch_use_the_peer_context` | covered |
| `test_project_launch_preflight_and_assignment_use_selected_peer` | same | covered |
| `test_slow_discovery_and_peer_snapshot_do_not_delay_local_heartbeats` | `test_bb_peer_routes.test_slow_discovery_and_peer_snapshots_never_hold_local_heartbeats` | covered |
| `test_shared_remote_semaphore_and_deadlines_are_bounded` | `test_bb_gateway.test_peer_commands_share_four_native_slots`; a peer deadline: `test_bb_peer_transport` | covered |
| `test_long_native_name_is_projected_with_relay_manifest_bound` | `test_bb_fleet.test_peer_names_are_projected_within_the_manifest_bounds` | covered |
| `test_peer_snapshot_size_and_inventory_count_are_independently_bounded` | `test_bb_peer_heartbeats.test_oversized_peer_snapshots_are_withheld_from_their_route_only`, `test_bb_fleet.test_invalid_inventories_fail_closed_and_keep_the_local_identity` | covered |
| `test_claim_poll_opt_in_tracks_guarded_routing_capability` | `test_bb_fleet.test_peers_are_shared_by_route_id_without_their_aliases`, `test_native_without_the_peer_abi_is_never_asked_for_peers`, `test_failed_first_discovery_never_opts_in_and_recovers` | covered |
| `test_cancellation_during_target_snapshot_records_failed_receipt_without_native_replay` | The receipt text: `vectors/receipts.json` (`cancelled_during_route_resolution`); the cancellation itself runs only on SIGINT, which awaits an owner decision (divergence h): a Rust unit test in C6 | planned |
| `test_each_claim_dispatches_only_target_without_full_peer_discovery` | `test_bb_gateway.test_claims_never_wait_for_discovery`, `test_claims_never_wait_for_another_peer` | covered |
| `test_startup_poll_transitions_from_short_legacy_wait_to_guarded_wait` | `test_bb_peer_routes.test_slow_discovery_and_peer_snapshots_never_hold_local_heartbeats` | covered |
| `test_missing_peer_route_keeps_other_peer_and_local_guarded_routing` | `test_bb_peer_heartbeats.test_a_missing_peer_route_never_downgrades_gateway_sharing` | covered |
| `test_pre_gateway_relay_fallback_chain_and_presence_probe` | `test_bb_gateway_downgrade.test_a_relay_without_gateway_support_downgrades_once_and_restart_probes_again`, `test_refused_gateway_fields_downgrade_with_404_and_422`; the 300 s presence and (divergence d) gateway re-probes need an injected clock | partial |
| `test_old_native_never_probes_dispatch_and_old_relay_falls_back` | `test_bb_fleet.test_native_without_the_peer_abi_is_never_asked_for_peers` | covered |
| `test_mark_read_reaches_only_the_selected_machine` | `test_bb_peer_operations.test_mark_read_reaches_only_the_selected_machine` | covered |

The vectors pin two receipts these runs do not reach. "guarded native routing
is unavailable" (`receipts.json`, `execute:gateway-route-unavailable`) needs a
routed claim handed to a one-hop poll that was already waiting when gateway
sharing was turned off: the relay hands routed claims only to one-hop polls,
which a connector makes only while it can route them, and withdraws them at
the next heartbeat without gateway support. The directory route mismatch is
below.

#### Peer cases in other modules

| In-process test | Black-box coverage | Status |
|---|---|---|
| `test_mobile_directory_create.PeerDirectoryCreate.test_exact_peer_and_gateway_creation_cannot_cross_machines` | `test_bb_peer_operations.test_folders_and_worktrees_are_created_only_on_the_selected_machine` | covered |
| `test_mobile_directory_create.PeerDirectoryCreate.test_foreign_computer_nested_route_and_unavailable_peer_fail_without_creation` | `test_bb_gateway.test_routes_outside_the_current_inventory_fail_before_native_work`, `test_bb_peer_routes.test_rebound_and_withdrawn_routes_fail_before_native_work`; a claim whose `computer_id` differs from its route: `projections.jsonl` (`directory-create-route-mismatch`), since no relay delivers `computer_id` | covered |
| `test_mobile_directory_create.PeerDirectoryCreate.test_old_peer_capability_does_not_inherit_new_gateway_support` | `test_bb_peer_operations.test_folders_and_worktrees_are_created_only_on_the_selected_machine` | covered |
| `test_mobile_directory_create.PeerDirectoryCreate.test_local_native_identity_reset_after_preflight_cannot_create` | `test_bb_gateway.test_native_guard_refusal_after_dispatch_is_uncertain_and_never_repeated` | covered |
| `test_mobile_directory_create.RustDirectoryHttp.test_actual_http_peer_route_is_exact_and_withdrawal_preserves_uncertainty` | The relay's exact `gateway_route` and its withdrawal before delivery: real-relay end-to-end tests on PostgreSQL; the connector side of an exact peer route: `test_bb_peer_operations.test_folders_and_worktrees_are_created_only_on_the_selected_machine` | partial |
| `test_mobile_worktrees.PeerWorktrees.test_worktree_create_keeps_selected_native_context_and_exact_guard` | `test_bb_peer_operations.test_folders_and_worktrees_are_created_only_on_the_selected_machine` | covered |
| `test_mobile_worktrees.PeerWorktrees.test_rebound_peer_cannot_create_on_local_or_new_identity` | `test_bb_peer_routes.test_rebound_and_withdrawn_routes_fail_before_native_work` | covered |
| `test_peer_end_to_end.PeerEndToEndTests.test_phone_reaches_peer_only_through_enrolled_gateway` | The connector half: `test_bb_fleet`, `test_bb_gateway.test_equal_session_names_reach_only_their_own_machine`, `test_mutations_forward_exact_payloads_and_upload_their_results`, `test_route_is_part_of_the_request_identity`; the relay half: real-relay end-to-end tests on PostgreSQL | partial |
| `test_peer_end_to_end.PeerEndToEndTests.test_unreachable_peer_stays_listed_offline_under_its_gateway` | The connector half: `test_bb_peer_routes.test_unreachable_verified_peer_is_shared_offline_and_never_contacted`; the relay half: real-relay end-to-end tests on PostgreSQL | partial |
| `tests/test_mobile_peers.py`: `MobilePeers.test_connector_timeout_and_cancellation_reap_owned_transports` | `test_bb_peer_transport.test_peer_commands_reach_the_peer_and_a_deadline_reaps_the_owned_transport` (the deadline); the cancellation runs only on SIGINT (divergence h) | partial |

### Additive mobile repository acquisition

`test_bb_repositories` exercises the actual Rust connector against a private loopback
relay and scripted native process: exact machine guard and original clone UUID,
duplicate delivery, native fingerprint refusal, status-only recovery, old-native
capability refusal and bounded failure projection. It is Rust-only, like Git status;
the frozen Python operation vectors stay unchanged. Native `tests/test_repositories.py`
also runs guarded acquisition with actual local Git, preserving a foreign source and
refusing a replaced/wrong machine before destination creation. Neither suite contacts
an external repository or starts a native session.
