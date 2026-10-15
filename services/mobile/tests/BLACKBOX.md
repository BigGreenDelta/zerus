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

Outside Python mode only the modules listed under `rust` in `ported.json` run;
the others skip. `test_bb_harness.py` checks the harness itself and always
runs.

The 39 tests took 23-28 s on a Linux workstation against the Python
connector (2026-10-15); each test starts its own connector processes.

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
  200 identical). `rule()` programs status answers, optionally limited by
  count or a predicate on the parsed body. All traffic is kept in
  `exchanges`.
- **`scripted_hgs.py`**: the `hgs` on the connector's PATH. It answers from
  `script.json`, re-read on every call, by exact argv or prefix, with JSON or
  text, an exit status, a delay, or `kill_parent` (SIGKILL the connector after
  the native work, to model a crash before journaling). It logs a `start`
  event (argv, stdin, environment, open descriptors) and an `end` event per
  call. `default_rules()` in the harness gives a minimal live computer with
  one non-Codex session, so no background attention scan runs.
- **`Journal`** reads a journal read-only; **`stopped_journal`** writes one only
  while it holds the connector lock, so a running connector is never edited.

## Reviewed divergences

`IMPLEMENTATION` selects the expected outcome where the connector README
(added with the Rust connector) records a deliberate difference:

| Divergence | Test | Python | Rust |
|---|---|---|---|
| b: a result the relay rejects | `test_bb_requests.test_rejected_result_blocks_later_claims_in_python`, `test_bb_native_limits.RefusedResultTests` | A 400, 413 or 422 upload refusal is retried before every poll, so it blocks every later claim. A native error beyond 4,096 code points, a non-string one, and a receipt within `MAX_RESULT` whose canonical form with its request_id exceeds 1 MiB are journaled as they are and refused that way. | The relay accepts the sanitised receipt: the error cut to 4,096 code points or a fixed text, an oversized receipt uncertain without its result. A stored result the relay keeps refusing is retired after three attempts, which needs the `test-hooks` clock scale (`CLOCK_SCALE_HOOK`). |
| h (amended): SIGTERM | `test_bb_signals` | Dies at once (exit -15); the in-flight native call finishes on its own; restart journals the mutation uncertain. | Stops claiming, lets the in-flight call finish, journals its real outcome, exits 0. |

Under h, neither may kill the native mutation child, and no request is
claimed after the signal. SIGINT is not covered: Python's KeyboardInterrupt
cancels the request and SIGKILLs the hgs child, which amended divergence h
does not address.

## Heartbeat key review

`test_bb_heartbeat.LISTING_KEYS` classifies every top-level key of
`hgs ls --json --local` as forwarded, withheld or carried beside the snapshot.
`test_reviewed_listing_keys_match_the_hgs_source` reads `local_json` in
`src/cli.rs`, so a key that hgs gains fails until someone classifies it; the
heartbeat test then fails on any snapshot key outside the reviewed set. The
table records what the Python connector forwards today.

## Coverage of the in-process connector tests

Status: **covered** (same contract black-box), **partial** (the rest is
planned where named), **planned** (slice or vector named).

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
| `test_attention_round_robin_budget_and_exact_identity_cache` | H3 | planned |
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

The other `AttachmentRelayTests` are relay cases.

| In-process test | Black-box coverage | Status |
|---|---|---|
| `AttachmentValidationTests.test_paths_urls_controls_and_unknown_fields_are_rejected` | `test_bb_send.test_attachments_and_text_beyond_the_limits_never_reach_native` | covered |
| `AttachmentValidationTests.test_native_file_and_total_limits_and_utf8_text` | `test_bb_send.test_attachments_at_the_native_limits_reach_native_unchanged`, `test_attachments_and_text_beyond_the_limits_never_reach_native` | covered |
| `AttachmentRelayTests.test_connector_accepts_large_request_response_independently` | `test_bb_send.test_attachments_at_the_native_limits_reach_native_unchanged` (a request above 26 MiB), `test_attachments_and_text_beyond_the_limits_never_reach_native` | covered |

### `test_mobile_child_send.py` (`ChildSendTests`)

| In-process test | Black-box coverage | Status |
|---|---|---|
| `test_exact_parent_and_continuable_child_verified_before_one_native_send` | `test_bb_send.test_child_send_checks_the_exact_parent_and_child_before_one_send` | covered |
| `test_other_provider_or_child_readonly_identity_never_mutates` | same | covered |
| `test_wrong_child_receipt_is_uncertain_and_no_replay` | same | covered |
| `test_arbitrary_child_paths_and_context_continuations_rejected` | `test_bb_send.test_attachments_and_text_beyond_the_limits_never_reach_native` | covered |

### `test_mobile_context.py` (`ContextConnectorTests`, `ContextFixtureTests`)

`ContextRelayTests` has no connector case.

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

`CoreRelayTests` has no connector case.

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
| `MobileDeliveryConnectorTests.test_invalid_values_sessions_or_fields_never_spawn` | the `set_mobile_delivery` vectors in `test_bb_session_validation.test_rejected_vectors_fail_with_their_receipt_and_run_nothing` | covered |
| `MobileDeliveryConnectorTests.test_unsupported_native_cli_fails_before_any_mutation` | `test_bb_controls.test_hgs_without_the_preference_abi_advertises_and_runs_nothing` | covered |
| `MobileDeliveryFixtureTests.test_only_the_gateway_advertises_and_its_snapshot_reports_the_value` | `test_bb_controls.test_each_value_runs_its_fixed_command_once` (advertised), `test_the_snapshot_reports_the_setting_with_or_without_an_hgs_version`; a peer never advertising it: H4 | partial |

### `test_mobile_history_recovery.py` (`HistoryRecovery`)

| In-process test | Black-box coverage | Status |
|---|---|---|
| `test_history_is_read_only_scoped_and_old_nodes_deny` | `test_bb_history_recovery.test_hgs_without_the_history_abi_runs_nothing`; the fields: the `history/*` vectors in `test_bb_session_validation.test_rejected_vectors_fail_with_their_receipt_and_run_nothing` | covered |
| `test_fixture_pages_over_500_and_wrong_cursor_scope_rejected` | cursors pass unchanged: `test_bb_history_recovery.test_history_reads_pass_their_scoped_payload_unchanged`, `test_a_cursor_read_is_never_repeated_for_indexing`; paging is `fixture_hgs.py` behaviour | covered |
| `test_wrong_history_ack_and_archive_match_fail_closed` | `test_bb_history_recovery.test_a_page_of_anything_else_fails_closed` | covered |
| `test_recovery_five_field_identity_and_duplicate_no_repeat` | `test_bb_history_recovery.test_recovery_action_checks_the_waiting_job_then_acts_once`, `test_a_changed_job_or_a_mismatched_acknowledgement_never_repeats` | covered |
| `test_head_locally_advances_bounded_index_and_partial_stops` | `test_bb_history_recovery.test_a_head_read_follows_local_indexing_while_it_progresses`; the 2 s budget: `test_bb_native_limits.test_a_read_out_of_its_budget_fails_and_its_child_is_killed` | covered |
| `test_history_cursor_reads_are_not_repeated_for_indexing` | `test_bb_history_recovery.test_a_cursor_read_is_never_repeated_for_indexing` | covered |
| `test_native_recovery_uncertain_preserved` | `test_bb_history_recovery.test_recovery_action_checks_the_waiting_job_then_acts_once` | covered |

### `test_mobile_read.py`

`MarkReadValidation.test_relay_oracle_validates_the_same_envelope` is a relay case.

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

`VoiceValidationTests` call the relay's `validate_request`; the connector's own checks run through the voice vectors in `test_bb_session_validation.test_rejected_vectors_fail_with_their_receipt_and_run_nothing`.

| In-process test | Black-box coverage | Status |
|---|---|---|
| `VoiceConnectorTests.test_acceptance_is_scoped_durable_and_duplicate_never_reinvokes_native` | `test_bb_controls.test_voice_operations_reach_native_with_their_action_once` | covered |
| `VoiceConnectorTests.test_wrong_conversation_ack_is_uncertain_and_not_replayed` | `test_bb_controls.test_a_mismatched_voice_acknowledgement_never_repeats` | covered |
| `VoiceConnectorTests.test_old_host_rejects_without_native_and_status_read_is_short_lived` | `test_bb_controls.test_hgs_without_the_voice_abi_advertises_and_runs_nothing`, `test_a_delivered_status_read_is_kept_only_two_minutes` | covered |

### Other modules with connector cases

| Module | Planned coverage |
|---|---|
| `test_mobile_archives.py`, `test_mobile_terminal.py`, `test_mobile_history.py` | H3; the H2 session-operation modules have their tables above |
| `test_mobile_attachments.py`, `test_mobile_attachment_read.py`, `test_mobile_launch.py`, `test_mobile_project_launch.py`, `test_mobile_projects.py`, `test_mobile_directory_create.py`, `test_mobile_worktrees.py`, `test_mobile_accounts.py`, `test_mobile_speech.py`, `test_mobile_speech_config.py` | H3, computer, read and media operations |
| `test_connector_peers.py`, `test_peer_end_to_end.py`, the peer classes of `test_mobile_directory_create.py` and `test_mobile_worktrees.py` | H4, peers and the one-hop gateway |
| `test_managed_relay.py` | P2 `identities.json` |
| `test_end_to_end.py`, `test_presence_end_to_end.py` | Real-relay end-to-end tests on PostgreSQL |

Connector halves inside relay classes (for example
`AttachmentRelayTests.test_connector_accepts_large_request_response_independently`
and `TerminalRetention`) need rows here before the Python reference relay is
deleted.
