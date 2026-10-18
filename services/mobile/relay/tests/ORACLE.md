# Python reference relay coverage

The Python reference relay (`zerus_mobile/server.py`, `store.py`,
`async_store.py`, `ingress.py`, `push.py`, `routes.py`, `work.py`) is deleted in
slice R6 of the
[Rust connector design](../../../../docs/superpowers/specs/2026-10-15-rust-mobile-connector-design.md).
This file maps every test in `services/mobile/tests` that exercises it to the
test that keeps its coverage, so R6 can delete the Python relay and its tests
without losing a contract. It sits beside the contracts it names; the relay
README stays the operator runbook. R6 removes the Python tests listed here and
then this file.

Replacements are Rust contracts in this directory unless noted; each runs on
its own PostgreSQL database in CI (`tests/common/mod.rs`,
`ZERUS_RELAY_TEST_DATABASE_URL`). Names are `file::test` without `.rs`.

| Status | Meaning |
|---|---|
| Rust | A Rust contract replaces it. |
| R3 | Also replayed against the Rust relay by `test_rust_relay.py` today; R3 rewrites that module without the Python store. |
| P3 | A `validate_request` call site. P3 moves it to the test-only `relay_oracle.py` copy, and `vectors.rs` checks the Rust relay against the `validation/*.jsonl` cases it produced. A Rust contract on main is named where one exists. |
| Black-box | A connector half, covered by the `test_bb_*` suites; see [`BLACKBOX.md`](../../tests/BLACKBOX.md). |
| Served | Already runs against the Rust relay through `relay_fixture.start_relay`. Only its fallback without `ZERUS_RELAY_BINARY` serves the Python relay, which R6 removes. |
| Dropped | Oracle-only: behaviour of the Python implementation itself that the Rust relay does not share. |

Totals: 26 tests in `test_relay.py`, 21 in `test_relay_peers.py`, 27 in
`test_relay_resources.py`, 6 in `test_store_resources.py`, 2 of
`test_rust_relay.py`'s own, 13 in relay classes of other modules, 16
`validate_request` call sites, 1 in `test_vectors.py`, 2 in `test_relay_cli.py`
and 10 served through the fixture fallback or the `test_rust_relay.py` store.
Of these, 9 are dropped outright; every other one has a passing Rust
contract, a black-box test or an R3 or P3 owner as listed.

## `test_relay.py`

`test_rust_relay.py` replays 21 of the `RelayTests` against the Rust binary.
The other five (the four provider-mocked push tests and `FilesystemTests`) had
no Rust replacement before this slice.

| Python test | Replacement | Status |
|---|---|---|
| `RelayTests.test_health_capabilities_and_local_only_administration` | `http_contracts::health_capabilities_and_administration_stay_off_the_api` | Rust, R3 |
| `RelayTests.test_pair_once_and_expiry_and_hash_storage` | `http_contracts::pairing_is_single_use_expires_and_keeps_only_hashes`, `contracts::scoped_pairing_and_rates` | Rust, R3 |
| `RelayTests.test_pair_rate_limit` | `contracts::scoped_pairing_and_rates`, `http_contracts::an_untrusted_peer_cannot_rotate_forwarding_headers` | Rust, R3 |
| `RelayTests.test_failed_authentication_rate_limit` | `operation_contracts::terminal_frames_fit_each_credentials_rate_budget` (20 refusals, then 429), `contracts::http_rate_and_lane_refusals_are_answered_after_bounded_drain`, `http_contracts::anonymous_abuse_keeps_authenticated_and_health_access` | Rust, R3 |
| `RelayTests.test_workspace_and_role_isolation` | `http_contracts::credentials_are_scoped_to_their_role_and_workspace`, `contracts::scoped_pairing_and_rates` | Rust, R3 |
| `RelayTests.test_durable_claim_duplicate_result_and_owner` | `contracts::claims_and_receipts_never_replay` | Rust, R3 |
| `RelayTests.test_single_claim_and_request_validation` | `contracts::claims_and_receipts_never_replay`; the invalid envelopes: `operation_contracts::queue_settings_recovery_and_process_controls_are_exact_and_gated` and P3 | Rust, R3 |
| `RelayTests.test_expiry_restart_and_late_result` | `contracts::expired_claims_and_revocation_are_terminal`, `contracts::restart_retires_claims_only_on_sqlite`, `contracts::budgets_reserve_results_and_release_on_expiry` | Rust, R3 |
| `RelayTests.test_queue_limit_retention_tombstones` | `contracts::budgets_reserve_results_and_release_on_expiry` (queue limit), `operation_contracts::expired_mutation_history_leaves_a_tombstone_that_never_replays`, `contracts::claims_and_receipts_never_replay` (tombstone after retention) | Rust, R3 |
| `RelayTests.test_claim_timeout_and_revoke` | `contracts::expired_claims_and_revocation_are_terminal`, `operation_contracts::node_revocation_charges_errors_and_releases_reservations_exactly` | Rust, R3 |
| `RelayTests.test_device_logout_revokes_token_and_cancels_queue` | `http_contracts::credentials_are_scoped_to_their_role_and_workspace` | Rust, R3 |
| `RelayTests.test_old_read_history_does_not_consume_mutation_tombstones` | `operation_contracts::reads_leave_after_an_hour_and_mutations_keep_their_receipts` | Rust, R3 |
| `RelayTests.test_mark_read_is_read_class_and_pruned_like_reads` | `contracts::mark_read_is_read_class_and_pruned_like_reads` | Rust, R3 |
| `RelayTests.test_attachment_read_is_exact_gated_and_short_lived` | `contracts::attachment_read_is_exact_gated_and_short_lived` | Rust, R3 |
| `RelayTests.test_long_poll_rechecks_revocation` | `http_contracts::long_polls_recheck_revocation` | Rust, R3 |
| `RelayTests.test_heartbeat_events_baseline_transition_and_privacy` | `http_contracts::heartbeat_events_follow_transitions_and_carry_no_content`, `contracts::snapshot_events_and_push_claims` | Rust, R3 |
| `RelayTests.test_body_cap_nonfinite_and_poll_bounds` | `contracts::http_oversized_declared_body_is_answered_after_bounded_drain`, `http_contracts::cursors_waits_and_routing_flags_are_strict` | Rust, R3 |
| `RelayTests.test_async_question_fingerprints_add_change_remove` | `http_contracts::pending_question_fingerprints_announce_additions_and_changes_only` | Rust, R3 |
| `RelayTests.test_push_retired_unifiedpush_and_unavailable_fcm` | `http_contracts::push_registration_needs_a_configured_provider`, `http_contracts::health_capabilities_and_administration_stay_off_the_api` | Rust, R3 |
| `RelayTests.test_push_generic_payload_retries_and_revoke` | `push_contracts::wakes_are_generic_back_off_and_leave_with_the_device` | Rust |
| `RelayTests.test_push_retries_are_bounded_and_provider_gone_removes_registration` | `push_contracts::retries_are_bounded_and_an_unregistered_token_is_removed`, `push_contracts::fcm_messages_carry_only_generic_string_data` (the provider's unregistered answer) | Rust |
| `RelayTests.test_retired_unifiedpush_registration_is_removed_on_delivery` | `contracts::retired_unifiedpush_registration_is_removed_on_delivery`; its wake job leaves too: `http_contracts::background_maintenance_and_wakes_resume_after_failures` | Rust |
| `RelayTests.test_fcm_sdk_message_is_generic_data_only` | `push_contracts::fcm_messages_carry_only_generic_string_data` (`push::fcm_message` and `push::fcm_unregistered`, extracted from the delivery path for this) | Rust |
| `RelayTests.test_mark_read_is_exact_and_capability_gated` | `contracts::mark_read_payload_is_exact_and_capability_gated`; the declared operation set: `operation_contracts::context_commands_are_gated_per_operation_and_protocol_version` | Rust, R3 |
| `RelayTests.test_set_mobile_delivery_request_validation` | `contracts::mobile_delivery_is_computer_scoped_and_capability_gated` | Rust, R3 |
| `FilesystemTests.test_private_database_and_no_parent_chmod` | `private_files::the_sqlite_store_is_private_and_never_changes_its_parent` (R5 deletes it with SQLite), `private_files::the_configuration_is_an_owner_only_regular_file` (the private file that remains) | Rust |

## `test_rust_relay.py`

R3 rewrites this module; its 21 borrowed `RelayTests` are mapped above.

| Python test | Replacement | Status |
|---|---|---|
| `RustHttpContracts.test_existing_python_receipt_identity` | R3's PostgreSQL SQL fixture with Python-computed `body_hash` values; the identity form: `contracts::canonical_python_identity`, `codec::goldens_match_python` | R3 |
| `RustHttpContracts.test_presence_alerts_http_contract` | `contracts::presence_holds_read_drops_and_lock_delivers_alert`, `contracts::heartbeats_store_presence_outside_the_snapshot` | Rust, R3 |

## `test_relay_peers.py` (`SQLitePeerTests`)

| Python test | Replacement | Status |
|---|---|---|
| `test_peer_creates_no_credential_and_workspace_isolation` | `registry_contracts::peers_create_no_credential_and_stay_in_their_workspace`, `contracts::one_hop_routes_freeze_and_withdraw` | Rust |
| `test_claim_requires_explicit_optin_and_pins_native_identity` | `registry_contracts::gateway_claims_need_opt_in_and_pin_the_native_identity` | Rust |
| `test_withdraw_then_readd_never_revives_old_request_or_receipt` | `registry_contracts::withdrawn_routes_never_revive_old_requests_or_receipts` | Rust |
| `test_withdraw_after_claim_is_uncertain_and_exact_result_cannot_resolve` | `registry_contracts::withdrawal_after_claim_is_uncertain_and_results_cannot_resolve_it`, `contracts::one_hop_routes_freeze_and_withdraw` | Rust |
| `test_gateway_freshness_never_refreshes_peer_snapshot` | `registry_contracts::gateway_liveness_never_refreshes_a_stale_peer` | Rust |
| `test_offline_peer_keeps_last_seen_without_becoming_fresh` | `contracts::offline_peer_route_reports_last_peer_heartbeat`, `contracts::postgres_cross_worker_offline_peer_last_seen` | Rust |
| `test_stale_frozen_route_cannot_reroute_to_fresh_direct_node` | `registry_contracts::a_stale_frozen_route_never_reroutes_to_a_fresh_direct_enrollment` | Rust |
| `test_unexposed_direct_enrollment_alias_attaches_existing_peer` | `registry_contracts::an_unexposed_direct_enrollment_becomes_an_alias_of_the_peer`, `administration::revoke_computer_by_alias` | Rust |
| `test_exposed_legacy_collision_ids_remain_visible_and_revoke_physical_machine` | `registry_contracts::exposed_legacy_ids_stay_listed_and_revoke_the_physical_machine` | Rust |
| `test_revoked_peer_manifest_does_not_disable_gateway_or_other_peers` | `registry_contracts::a_revoked_peer_leaves_the_gateway_and_other_peers_working` | Rust |
| `test_root_computer_revoke_preserves_gateway_credential_and_peer_routes` | `registry_contracts::root_revocation_keeps_the_gateway_credential_and_peer_routes`, `contracts::revoked_root_retains_only_authorized_peers` | Rust |
| `test_revoked_root_does_not_publish_new_local_snapshot_events_or_pushes` | `registry_contracts::a_revoked_root_publishes_no_local_snapshot_events_or_wakes` | Rust |
| `test_gateway_revoke_preserves_independent_direct_route` | `registry_contracts::gateway_revocation_keeps_an_independent_direct_route` | Rust |
| `test_immutable_root_binding_and_authorized_peer_metadata` | `registry_contracts::the_root_binding_is_immutable_and_peer_metadata_must_match` | Rust |
| `test_peer_events_use_target_computer_and_keep_event_accounting` | `registry_contracts::peer_events_name_the_target_computer_and_keep_event_accounting` | Rust |
| `test_revoke_using_attached_node_alias_blocks_existing_peer_machine` | `registry_contracts::revoking_an_attached_alias_blocks_the_peer_machine`, `administration::revoke_computer_by_alias` (through the command line) | Rust |
| `test_attached_direct_events_use_visible_computer_identity` | `registry_contracts::attached_direct_enrollments_report_events_as_the_visible_computer` | Rust |
| `test_catalog_rejects_selected_payload_aggregate_before_fetch` | `registry_contracts::the_catalog_refuses_the_selected_snapshot_aggregate_before_fetching` | Rust |
| `test_catalog_unicode_wire_cap_is_enforced_for_selected_peers` | `registry_contracts::the_catalog_wire_bound_counts_escaped_peer_names` | Rust |
| `test_registry_retention_ceiling_rolls_back_manifest` | `registry_contracts::the_registry_ceiling_rolls_back_the_whole_manifest` (the real 2,048-computer ceiling instead of a patched constant) | Rust |
| `test_strict_http_metadata_and_optin` | `registry_contracts::gateway_metadata_and_its_opt_in_are_strict_over_http` | Rust |

## `test_relay_resources.py` (`RelayResourceTests`)

| Python test | Replacement | Status |
|---|---|---|
| `test_proxy_clients_have_independent_pairing_quotas` | `http_contracts::trusted_proxy_clients_have_independent_pairing_quotas`, `contracts::proxy_trust_is_explicit` | Rust |
| `test_untrusted_peer_cannot_rotate_forwarding_headers` | `http_contracts::an_untrusted_peer_cannot_rotate_forwarding_headers` | Rust |
| `test_trusted_chain_uses_nearest_untrusted_hop` | `http_contracts::a_trusted_chain_names_the_nearest_untrusted_hop`, `contracts::proxy_trust_is_explicit` | Rust |
| `test_anonymous_abuse_preserves_authenticated_and_health_access` | `http_contracts::anonymous_abuse_keeps_authenticated_and_health_access` | Rust |
| `test_external_sqlite_writer_does_not_stall_health` | `http_contracts::health_answers_while_a_writer_holds_the_database` (an exclusive table lock on PostgreSQL) | Rust |
| `test_long_poll_overload_and_disconnect_release_capacity` | `http_contracts::poll_capacity_spans_credentials_and_returns_after_a_disconnect`, `contracts::poll_leases_have_separate_capacity_and_idempotent_release`, `contracts::http_end_to_end_and_disconnect_cleanup` | Rust |
| `test_workspace_poll_limit_spans_distinct_credentials` | `http_contracts::poll_capacity_spans_credentials_and_returns_after_a_disconnect` | Rust |
| `test_disconnected_large_upload_releases_capacity` | `contracts::http_rejects_bad_content_before_mutation_and_recovers_from_disconnect`, `contracts::http_streamed_body_over_limit_releases_lane_before_bounded_drain` | Rust |
| `test_many_small_requests_with_large_results_reach_workspace_budget` | `operation_contracts::many_large_results_reach_the_workspace_budget` | Rust |
| `test_canonical_result_expansion_is_bounded_and_receipt_remains_safe` | `http_contracts::results_are_bounded_in_their_escaped_form_before_acknowledgement` | Rust |
| `test_slow_pair_bodies_preserve_authenticated_capacity` | `http_contracts::anonymous_abuse_keeps_authenticated_and_health_access` (slow pairing bodies hold the anonymous lanes), `contracts::http_rate_and_lane_refusals_are_answered_after_bounded_drain` | Rust |
| `test_oversized_event_cursor_is_rejected` | `http_contracts::cursors_waits_and_routing_flags_are_strict` | Rust |
| `test_catalog_payload_limit_applies_before_snapshot_fetch` | `http_contracts::the_catalog_size_is_checked_before_snapshots_are_read` | Rust |
| `test_dense_and_deep_json_are_rejected_before_loads` | `codec::parse_limits_count_containers_strings_and_scalars`, `contracts::malformed_and_deep_json` | Rust |
| `test_cancelled_thread_keeps_capacity_until_actual_completion` | Python's `ingress.blocking` and `Capacity` helpers. `App::work` moves its permits into the `spawn_blocking` closure, so no cancellation can release them early and there is no helper to test. | Dropped |
| `test_cancelled_spool_thread_closes_discarded_result` | Python's spool thread and its file object. A Rust response owns its unlinked spool file and drops it with the stream. | Dropped |
| `test_streaming_encoder_bounds_a_single_large_json_string` | Python's chunked `_encoded` generator (its memory profile). The Rust encoder writes through `LimitWriter`; its output is `codec.rs`'s, its memory the 256 MiB envelope of `tests/load.py`. | Dropped |
| `test_maintenance_and_push_resume_after_transient_failure` | `http_contracts::background_maintenance_and_wakes_resume_after_failures` | Rust |
| `test_idle_polls_leave_ordinary_authenticated_admission_free` | `http_contracts::idle_polls_leave_ordinary_authenticated_admission_free` | Rust |
| `test_readiness_flood_coalesces_database_probes` | `http_contracts::readiness_probes_coalesce_while_the_database_is_held` | Rust |
| `test_cancelled_poll_keeps_slot_until_durable_cleanup_finishes` | `http_contracts::a_cancelled_poll_keeps_its_slot_until_durable_cleanup_finishes` | Rust |
| `test_byte_parser_preserves_stdlib_json_semantics` | Python's own byte parser against `json.loads`. The Rust relay decodes with serde_json under the same bounds; the same accepted and refused bodies: `codec::bodies_parse_as_python_json_loads_does` (lone surrogates differ, below), `codec::goldens_match_python`, `codec::python_default_form_matches_cpython`, `contracts::python_float_and_unicode_differential`; non-finite numbers: `http_contracts::cursors_waits_and_routing_flags_are_strict` | Dropped (parser), Rust (semantics) |
| `test_wide_json_tokens_and_aggregate_are_bounded_before_decoding` | The bounds: `codec::parse_limits_count_containers_strings_and_scalars`, `codec::a_request_at_the_relay_maximum_needs_the_batch_margins`; "before `json.loads` runs" is Python's decoder memory | Rust (bounds), Dropped (decoder) |
| `test_parser_releases_input_immediately_with_cyclic_gc_disabled` | CPython reference cycles and garbage collection | Dropped |
| `test_worker_error_releases_input_without_cyclic_collection` | CPython reference cycles and garbage collection | Dropped |
| `test_unicode_event_pages_fit_phone_cap_without_skipping` | `http_contracts::unicode_event_pages_fit_the_phone_bound_without_skipping` | Rust |
| `test_result_receipt_wire_limit_is_checked_before_acknowledgement` | `http_contracts::results_are_bounded_in_their_escaped_form_before_acknowledgement` | Rust |

## `test_store_resources.py`

| Python test | Replacement | Status |
|---|---|---|
| `NotificationTests.test_unrelated_notifications_do_not_spin_idle_topic` | Python's per-topic `Notifications` registry. Rust polls share one bounded broadcast channel, skip other topics in `App::wait` and reread durably every five seconds; a notification is only a hint. What a poll sees: `http_contracts::long_polls_wake_promptly_and_only_for_their_own_news` | Dropped (registry), Rust (wakes) |
| `NotificationTests.test_new_waiters_do_not_reuse_signalled_event` | The same registry's event objects; a poll that starts after earlier news: `http_contracts::long_polls_wake_promptly_and_only_for_their_own_news` | Dropped (registry), Rust (wakes) |
| `NotificationTests.test_idle_topic_cardinality_is_bounded` | The same registry's topic table; Rust keeps no per-topic state | Dropped |
| `SQLiteResourceTests.test_cancellation_drains_worker_and_retains_capacity` | Python's `SyncStoreAdapter` worker threads and slots. Rust uses SQLx's pool and RAII transaction permits (`db.rs`). | Dropped |
| `SQLiteResourceTests.test_worker_failures_release_payload_without_cyclic_gc` | CPython garbage collection | Dropped |
| `SQLiteResourceTests.test_expiry_and_revocation_release_reserve_charge_errors_exactly` | `operation_contracts::node_revocation_charges_errors_and_releases_reservations_exactly` | Rust |

## Relay classes in other modules

The connector classes of these modules stay until the Python connector goes
(E2); R6 deletes only the classes below and the imports they need.

| Python test | Replacement | Status |
|---|---|---|
| `test_mobile_attachments.AttachmentRelayTests.test_larger_than_one_mebibyte_upload_and_node_delivery` | `http_contracts::a_large_inline_upload_crosses_http_and_reaches_the_node_once` | Rust |
| `test_mobile_attachments.AttachmentRelayTests.test_ordinary_endpoint_cap_invalid_metadata_and_capabilities` | `operation_contracts::attachment_limits_are_advertised_and_paths_are_refused`, `contracts::http_oversized_declared_body_is_answered_after_bounded_drain` | Rust |
| `test_mobile_attachments.AttachmentRelayTests.test_retained_completed_bodies_budget_and_reserved_control_allowance` | `operation_contracts::retained_bodies_reach_the_budget_while_exact_retries_and_controls_fit` | Rust |
| `test_mobile_attachments.AttachmentRelayTests.test_connector_accepts_large_request_response_independently` | The connector half: `test_bb_send.test_attachments_at_the_native_limits_reach_native_unchanged`, `test_attachments_and_text_beyond_the_limits_never_reach_native` | Black-box |
| `test_mobile_archives.ArchiveRelayTests.test_relay_preserves_archive_id_and_rejects_malformed_or_mutation_payloads` | `operation_contracts::archive_ids_are_exact_on_reads_and_refused_on_ordinary_mutations` | Rust |
| `test_mobile_archives.ArchiveRelayTests.test_archived_name_collision_does_not_change_live_notification_baseline` | `operation_contracts::archived_rows_never_change_the_live_event_baseline` | Rust |
| `test_mobile_context.ContextRelayTests.test_relay_declares_ops_old_nodes_reject_controls_but_accept_ordinary_send` | `operation_contracts::context_commands_are_gated_per_operation_and_protocol_version` | Rust |
| `test_mobile_context.ContextRelayTests.test_valid_request_duplicate_receipt_survives_capability_change` | `operation_contracts::context_receipts_survive_a_capability_change` | Rust |
| `test_mobile_context.ContextRelayTests.test_node_declares_each_operation_and_protocol_explicitly` | `operation_contracts::context_commands_are_gated_per_operation_and_protocol_version` | Rust |
| `test_mobile_context.ContextRelayTests.test_relay_rejects_extra_archive_invalid_identity_and_compaction_id` | `operation_contracts::context_payloads_are_exact` | Rust |
| `test_mobile_controls.CoreRelayTests.test_extension_caps_old_clients_and_read_retention` | `operation_contracts::inspection_and_send_extensions_need_their_advertised_features`, `operation_contracts::reads_leave_after_an_hour_and_mutations_keep_their_receipts`; its `validate_request` calls: P3 | Rust, P3 |
| `test_mobile_terminal.TerminalRateBudget.test_four_frames_per_second_receipts_and_node_uplink_for_two_minutes` | `operation_contracts::terminal_frames_fit_each_credentials_rate_budget`, over HTTP so the limits are the ones the relay applies | Rust |
| `test_mobile_terminal.TerminalRetention.test_sustained_reads_and_abandoned_results_expire_without_mutation_eviction` | `operation_contracts::sustained_screens_and_abandoned_reads_expire_without_evicting_input`, `operation_contracts::terminal_input_needs_a_fresh_computer_and_expires_before_delivery`. Its Journal half is now `TerminalJournalRetention.test_sustained_screens_leave_the_journal_and_inputs_stay`, a connector test mapped in `BLACKBOX.md`. | Rust |

`ContextConnectorTests` and `ContextFixtureTests` need no relay; their module
imports `Store`, `Config` and `create_app` only for `ContextRelayTests`, and
`BLACKBOX.md` maps them.

## `validate_request` call sites

These connector tests also assert that the Python relay's envelope check
refuses what the connector refuses. They stay until E2 on P3's
`relay_oracle.py` copy.

| Python test | Rust contract on main | Status |
|---|---|---|
| `test_mobile_actions.LifecycleTransportTests.test_old_cli_and_invalid_payload_never_invoke_mutator` | `operation_contracts::history_launch_and_lifecycle_wires_refuse_what_the_connector_refuses`, `operation_contracts::archive_ids_are_exact_on_reads_and_refused_on_ordinary_mutations` | P3 |
| `test_mobile_directory_create.DirectoryCreate.test_invalid_wire_fields_paths_and_uuid_are_rejected_without_native` | `directory_create::strict_directory_wire_agrees_with_connector_and_is_never_a_read` | P3 |
| `test_mobile_history_recovery.HistoryRecovery.test_history_is_read_only_scoped_and_old_nodes_deny` | `operation_contracts::history_launch_and_lifecycle_wires_refuse_what_the_connector_refuses`, `operation_contracts::reads_leave_after_an_hour_and_mutations_keep_their_receipts` | P3 |
| `test_mobile_launch.LaunchTransport.test_preflight_old_cli_invalid_fields_and_wrong_provider_do_not_launch` | `operation_contracts::history_launch_and_lifecycle_wires_refuse_what_the_connector_refuses` | P3 |
| `test_mobile_launch.LaunchTransport.test_model_route_is_bounded_and_specific_to_native_opencode` | `contracts::opencode_launch_model_is_native_account_scoped_and_bounded`, `contracts::model_settings_match_launch_utf8_byte_bound` | P3 |
| `test_mobile_read.MarkReadValidation.test_relay_oracle_validates_the_same_envelope` | `contracts::mark_read_payload_is_exact_and_capability_gated` | P3 |
| `test_mobile_speech.SpeechValidationTests.test_all_computer_operations_have_exact_shapes` | `contracts::speech_wire_rejects_unbounded_or_ambiguous_audio` | P3 |
| `test_mobile_speech.SpeechValidationTests.test_invalid_audio_identity_and_extra_fields_are_rejected` | `contracts::speech_wire_rejects_unbounded_or_ambiguous_audio` | P3 |
| `test_mobile_speech.SpeechValidationTests.test_maximum_audio_has_bounded_valid_encoding` | `contracts::speech_wire_rejects_unbounded_or_ambiguous_audio` | P3 |
| `test_mobile_speech_config.SpeechConfigValidationTests.test_voice_routes_preserve_optional_exact_profile_revision` | `contracts::voice_profile_revision_is_optional_bounded_and_exact` | P3 |
| `test_mobile_speech_config.SpeechConfigValidationTests.test_all_operations_are_computer_scoped_and_exact` | `contracts::speech_config_receipts_erase_bodies_and_expire_reads`, `operation_contracts::speech_and_management_status_belong_to_the_original_phone_and_computer` | P3 |
| `test_mobile_speech_config.SpeechConfigValidationTests.test_plaintext_credentials_and_unbounded_envelopes_are_rejected` | `operation_contracts::speech_and_management_status_belong_to_the_original_phone_and_computer` (a plaintext key) | P3 |
| `test_mobile_terminal.TerminalTransport.test_payload_is_literal_bounded_and_rejects_hidden_control` | `operation_contracts::terminal_input_is_literal_bounded_and_keys_are_allowlisted` | P3 |
| `test_mobile_voice.VoiceValidationTests.test_operations_keep_session_and_exact_native_identity` | `contracts::voice_wire_requires_bounded_audio_and_stable_machine_route` | P3 |
| `test_mobile_voice.VoiceValidationTests.test_routes_are_machine_identities_not_caller_aliases` | `contracts::voice_wire_requires_bounded_audio_and_stable_machine_route` | P3 |
| `test_vectors.GoldenVectorTests.test_frozen_oracle_matches_the_reference_relay_while_it_exists` | `vectors.rs` (the relay column) | P3 |

`CoreRelayTests.test_extension_caps_old_clients_and_read_retention`, the
sixteenth call site, is listed with the relay classes.

## Suites served by the relay fixture

`relay_fixture.start_relay` runs the Rust relay on its own PostgreSQL database
when `ZERUS_RELAY_BINARY` is set, as in CI. Without it, these suites serve the
Python relay through `create_app`, and CI repeats `test_mobile_speech*.py` that
way. R6 deletes that branch and the repeat.

| Python test | Replacement | Status |
|---|---|---|
| `test_end_to_end.EndToEndTests.test_pair_inspect_send_once_question_alert_and_revoke` | The same test on the Rust relay | Served |
| `test_peer_end_to_end.PeerEndToEndTests` (2 tests) | The same tests on the Rust relay | Served |
| `test_relay_fixture.RelayFixtureTests.test_seeded_credentials_authenticate_until_revoked` | The same test on the Rust relay | Served |
| `test_mobile_speech.SpeechHttpServiceTests` (2 tests) | The same tests on the Rust relay; status ownership also `operation_contracts::speech_and_management_status_belong_to_the_original_phone_and_computer` | Served |
| `test_mobile_speech_config.SpeechConfigHttpTests` (3 tests) | The same tests on the Rust relay; also `contracts::speech_config_receipts_erase_bodies_and_expire_reads` and `operation_contracts::speech_and_management_status_belong_to_the_original_phone_and_computer` | Served |
| `test_computer_file_read.NativeFileHttp.test_ten_mib_original_snapshot_and_changed_evicted_copy` | Borrows `test_rust_relay.py`'s Python-seeded store; `contracts::computer_file_read_is_scoped_gated_and_short_lived` | R3 |

`test_relay_fixture.test_rust_relay_owns_one_private_database_until_cleanup`,
`test_presence_end_to_end` and `test_mobile_directory_create.RustDirectoryHttp`
already require the Rust relay and never serve Python.

## Command line and helpers

| Python test or file | Replacement | Status |
|---|---|---|
| `test_relay_cli.CompatibilityCliTests.test_administration_execs_the_rust_relay` | The `zerus-mobile` console script that R6 deletes (the image keeps the alias); the administration it delegates: `administration::revoke_computer_by_alias` | Dropped (shim), Rust |
| `test_relay_cli.CompatibilityCliTests.test_offline_migration_command_is_removed` | Checks that the same deleted shim has no `migrate-sqlite` | Dropped |
| `relay_fixture.ReferenceRelay` | The Python fallback above | R6 |
| `relay_load.run_probe` and its `--worker` | The Python relay load probe. `tests/load.py` imports only `write_fixture` and `memory_kib` from it, which R6 keeps or moves. | R6 |

## Differences found

None of these keeps the Python behaviour. The owner decided each contract
question on 2026-10-18; the Rust relay's rule is the contract:

- **DEL (U+007F)** is a control character in every identity the relay checks:
  `session`, `computer_id` and the `expected_run_id`/`expected_conversation_id`
  of a send, answer or interrupt are refused with 400, as for the C0 controls.
  Python's `text()` refused only C0 controls. The Rust connector's identity
  checks already counted DEL as a control; an answer's `question_id` and
  `expected_question_hash` stay unchecked by both relays, since native `hgs`
  verifies them (zerus-3rqx.6, `vectors.rs` `ORACLE_DIFFERENCES`).
- **A workspace over its own limits** gets 429 from both relays with the text
  that names the limit, which Android shows: "workspace queue is full",
  "workspace retained request limit reached" and "workspace retained payload
  budget is full". Only the relay's own capacity (its shared payload shards,
  lanes and rates) answers "relay capacity is busy" (zerus-3rqx.8,
  `operation_contracts::retained_bodies_reach_the_budget_while_exact_retries_and_controls_fit`,
  `contracts::retained_request_counts_name_their_own_limit`).
- **Rate limits:** Python's `RateLimits` also bounded authenticated requests
  globally and per IP address. The relay bounds them per credential (2,400 a
  minute) and by its admission lanes, and the Terminal budget fits both. Global
  and per-address limits belong to the reverse proxy, which a shared public
  relay needs before it opens (zerus-3rqx.9, `docs/mobile-deployment.md`).
- **Lone surrogates:** a body holding a lone UTF-16 surrogate escape, such as
  `"\ud800"`, is refused with 400, since the relay's strings must be valid
  Unicode (`codec::bodies_parse_as_python_json_loads_does`). Python's
  `json.loads` kept it wherever it did not check the text, for example in a
  heartbeat snapshot or in the answers of an `answer`. No released client sends
  one: `hgs` and serde_json never write one, the Python connector forwards
  `hgs` JSON, and Android's UTF-8 encoding replaces one with `?` (zerus-3rqx.10).
- Rust appends presence `alert` events beside ordinary events and queues wakes
  only for them; Python wakes phones on every ordinary event. The contracts
  that compare event lists skip `alert` rows. This is the reviewed presence
  alert design, not a new difference.
