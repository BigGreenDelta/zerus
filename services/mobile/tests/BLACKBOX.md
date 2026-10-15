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
| b: a result the relay rejects | `test_bb_requests.test_rejected_result_blocks_later_claims_in_python` | A 400, 413 or 422 upload refusal is retried before every poll, so it blocks every later claim. | Skipped; the retirement after three spaced attempts is planned for H2 with the `test-hooks` clock scale. |
| c: a peer `attachment_read` | `test_bb_peer_operations.test_peer_attachment_read_follows_divergence_c` | Fails with "connector could not record native outcome" before it asks the peer for the file. | Reads the file through the peer callback and serves the requested range. |
| d: a relay that refuses the extended heartbeat | `test_bb_gateway_downgrade` | Keeps gateway sharing off until restart. | The same within these tests; the re-probe after 300 s is planned for C6 with the `test-hooks` clock scale. |
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
| `test_cli_version_survives_enrichment_and_legacy_snapshots_remain_supported` | `hgs_version` forwarding in the heartbeat test; a listing without it: H2 | partial |
| `test_fixed_argv_and_exact_stdin_for_all_allowlisted_operations` | `test_bb_requests` (`inspect`, `send`, `interrupt`); `answer`: H2 | partial |
| `test_scrubs_native_identity_but_preserves_machine_paths` | `test_bb_requests.test_mutation_gets_exact_stdin_and_no_calling_agent_identity` | covered |
| `test_rejects_remote_alias_options_controls_and_unknown_session` | `test_bb_requests.test_unknown_session_fails_without_native_execution`; aliases, options and control characters: H2 | partial |
| `test_shell_syntax_in_exact_known_name_is_literal_argv` | H2 | planned |
| `test_rejects_invalid_request_and_payload_identity_before_spawn` | H2 (needs non-canonical identities the relay never queues) | planned |
| `test_completed_duplicate_replays_native_result_without_execution` | `test_bb_requests.test_duplicate_identity_replays_without_native_execution` | covered |
| `test_timeout_is_uncertain_for_mutation_and_never_retried` | H2 with the Rust `test-hooks` native timeout; Python's 45 s default is too slow for a black-box run | planned |
| `test_timeout_read_fails_without_marking_mutation` | same | planned |
| `test_output_bounds_and_safe_native_errors` | H2 (output cap through `test-hooks`; failing and invalid native output) | planned |
| `test_cancelled_mutation_is_durable_and_uncertain` | `test_bb_signals` covers the process-level stop (SIGTERM, divergence h), which in Python leaves the row running for restart recovery; the cancellation itself runs only on SIGINT, which awaits an owner decision | partial |
| `test_abrupt_process_exit_after_native_execution_cannot_repeat_write` | `test_bb_restart.test_crash_after_native_success_restarts_as_uncertain_without_repeating` | covered |
| `test_journal_excludes_second_connector` | `test_bb_journal.test_second_connector_on_the_same_journal_is_refused`, `test_first_start_creates_a_private_journal_bound_to_its_credentials` | covered |
| `test_outbox_survives_restart_and_retries_identical_result` | `test_bb_restart.test_undelivered_result_survives_restart_and_uploads_unchanged` | covered |
| `test_redirect_does_not_forward_token_or_repeat_result_elsewhere` | `test_bb_requests.test_redirects_are_never_followed` | covered |
| `test_result_conflict_is_retired_and_does_not_block_subsequent_commands` | `test_bb_requests.test_result_conflict_is_retired_without_blocking_later_requests` | covered |
| `test_result_too_large_is_failed_read_or_uncertain_write` | H2 | planned |
| `test_journal_refuses_config_switch_and_unsafe_files_without_chmod` | `test_bb_journal.test_other_credentials_never_take_over_a_journal`, `test_unsafe_state_directory_is_refused_without_repair`, `test_unsafe_journal_files_are_refused_without_repair` | covered |
| `test_attention_round_robin_budget_and_exact_identity_cache` | H3 | planned |
| `test_https_and_explicit_loopback_only_http` | `test_bb_startup.test_invalid_configuration_is_refused_before_any_state_or_network`, `test_loopback_http_requires_its_explicit_development_flag`; the full matrix: P2 `config.jsonl` | covered |

The poison pill of divergence b is pinned for Python by
`test_bb_requests.test_rejected_result_blocks_later_claims_in_python`; the Rust
retirement test is planned for H2, since its retry spacing needs the
`test-hooks` clock scale.

### `test_connector_retention.py`

| In-process test | Black-box coverage | Status |
|---|---|---|
| `JournalRetentionTests.test_delivered_inspect_expires_and_is_safe_to_read_again` | `test_bb_retention.test_delivered_read_expires_after_an_hour_and_may_execute_again` | covered |
| `JournalRetentionTests.test_mutation_content_scrubs_without_losing_durable_identity` | `test_bb_retention.test_delivered_mutation_is_scrubbed_after_seven_days_and_never_repeats` | covered |
| `JournalRetentionTests.test_pending_results_never_expire` | `test_bb_retention.test_undelivered_results_never_expire` | covered |
| `JournalRetentionTests.test_legacy_migration_is_conservative_and_preserves_outbox` | `test_bb_journal.test_legacy_journal_is_migrated_in_place_and_keeps_its_outbox` | covered |
| `JournalRetentionTests.test_retained_read_limit_does_not_consume_mutation_capacity` | P2 journal fixtures at the caps, then a seeded black-box run in H2 | planned |
| `ConnectorRetentionTests.test_changed_payload_never_executes_and_expired_result_is_explicit` | `test_bb_retention.test_delivered_mutation_is_scrubbed_after_seven_days_and_never_repeats` | covered |

### Other modules with connector cases

| Module | Planned coverage |
|---|---|
| `test_mobile_actions.py`, `test_mobile_archives.py`, `test_mobile_child_send.py`, `test_mobile_context.py`, `test_mobile_controls.py`, `test_mobile_delivery.py`, `test_mobile_read.py`, `test_mobile_terminal.py`, `test_mobile_history.py`, `test_mobile_history_recovery.py`, `test_mobile_voice.py` | H2, session operations |
| `test_mobile_attachments.py`, `test_mobile_attachment_read.py`, `test_mobile_launch.py`, `test_mobile_project_launch.py`, `test_mobile_projects.py`, `test_mobile_directory_create.py`, `test_mobile_worktrees.py`, `test_mobile_accounts.py`, `test_mobile_speech.py`, `test_mobile_speech_config.py` | H3, computer, read and media operations |
| `test_connector_peers.py`, `test_peer_end_to_end.py`, the peer classes of `test_mobile_directory_create.py` and `test_mobile_worktrees.py` | Mapped under [Peers and the one-hop gateway](#peers-and-the-one-hop-gateway) |
| `test_managed_relay.py` | P2 `identities.json` |
| `test_end_to_end.py`, `test_presence_end_to_end.py` | Real-relay end-to-end tests on PostgreSQL |

Connector halves inside relay classes (for example
`AttachmentRelayTests.test_connector_accepts_large_request_response_independently`
and `TerminalRetention`) need rows here before the Python reference relay is
deleted.

### Peers and the one-hop gateway

Seven modules cover discovery, routes, claims, peer operations and peer
heartbeats: `test_bb_fleet`, `test_bb_peer_routes`, `test_bb_gateway`,
`test_bb_peer_operations`, `test_bb_peer_heartbeats`,
`test_bb_gateway_downgrade` and `test_bb_peer_transport`. Their 32 tests took
8-34 s per module and 156 s in all on a Linux workstation against the Python
connector (2026-10-15).

`gateway_harness.py` scripts a gateway's `hgs` as a fleet: `swarm mobile-peers
--json` answers with a synthetic inventory, `swarm mobile-peer --json` with
each online peer's own listing, help, accounts and session (matched on the
envelope's `via`, target machine and inner argv through the scripted hgs's
`stdin_fields`), and `swarm __mobile-peer-local --json` with the gateway's
own guarded machine. Route IDs, `MAX_PEERS`, the 1 MiB peer snapshot bound,
help markers and receipt texts come from the golden vectors. `FakeRelay`
answers 404 to a peer heartbeat whose route is not in the last manifest it
accepted, as the relay does. Only `test_bb_peer_transport` runs the built
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

The vectors pin the receipts a black-box run cannot reach: "guarded native
routing is unavailable" (`receipts.json`, `execute:gateway-route-unavailable`;
the relay hands routed claims only to one-hop polls, which a connector makes
only while it can route them) and the directory route mismatch below.

#### Peer cases in other modules

| In-process test | Black-box coverage | Status |
|---|---|---|
| `test_mobile_directory_create.PeerDirectoryCreate.test_exact_peer_and_gateway_creation_cannot_cross_machines` | `test_bb_peer_operations.test_folders_and_worktrees_are_created_only_on_the_selected_machine` | covered |
| `test_mobile_directory_create.PeerDirectoryCreate.test_foreign_computer_nested_route_and_unavailable_peer_fail_without_creation` | `test_bb_gateway.test_routes_outside_the_current_inventory_fail_before_native_work`, `test_bb_peer_routes.test_rebound_and_withdrawn_routes_fail_before_native_work`; a claim whose `computer_id` differs from its route: `projections.jsonl` (`directory-create-route-mismatch`), since no relay delivers `computer_id` | covered |
| `test_mobile_directory_create.PeerDirectoryCreate.test_old_peer_capability_does_not_inherit_new_gateway_support` | `test_bb_peer_operations.test_folders_and_worktrees_are_created_only_on_the_selected_machine` | covered |
| `test_mobile_directory_create.PeerDirectoryCreate.test_local_native_identity_reset_after_preflight_cannot_create` | `test_bb_gateway.test_native_guard_refusal_after_dispatch_is_uncertain_and_never_repeated` | covered |
| `test_mobile_worktrees.PeerWorktrees.test_worktree_create_keeps_selected_native_context_and_exact_guard` | `test_bb_peer_operations.test_folders_and_worktrees_are_created_only_on_the_selected_machine` | covered |
| `test_mobile_worktrees.PeerWorktrees.test_rebound_peer_cannot_create_on_local_or_new_identity` | `test_bb_peer_routes.test_rebound_and_withdrawn_routes_fail_before_native_work` | covered |
| `test_mobile_delivery.MobileDeliveryFixtureTests.test_only_the_gateway_advertises_and_its_snapshot_reports_the_value` | `test_bb_peer_operations.test_the_delivery_setting_belongs_to_the_gateway`; the listing's `preferences` reach the heartbeat as `test_bb_heartbeat` checks | covered |
| `test_peer_end_to_end.PeerEndToEndTests.test_phone_reaches_peer_only_through_enrolled_gateway` | The connector half: `test_bb_fleet`, `test_bb_gateway.test_equal_session_names_reach_only_their_own_machine`, `test_mutations_forward_exact_payloads_and_upload_their_results`, `test_route_is_part_of_the_request_identity`; the relay half: real-relay end-to-end tests on PostgreSQL | partial |
| `test_peer_end_to_end.PeerEndToEndTests.test_unreachable_peer_stays_listed_offline_under_its_gateway` | The connector half: `test_bb_peer_routes.test_unreachable_verified_peer_is_shared_offline_and_never_contacted`; the relay half: real-relay end-to-end tests on PostgreSQL | partial |
| `tests/test_mobile_peers.py`: `MobilePeers.test_connector_timeout_and_cancellation_reap_owned_transports` | `test_bb_peer_transport.test_peer_commands_reach_the_peer_and_a_deadline_reaps_the_owned_transport` (the deadline); the cancellation runs only on SIGINT (divergence h) | partial |
