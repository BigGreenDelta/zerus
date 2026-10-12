# Overmind 1b Follow-ups Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close every open Overmind follow-up left by plan 1b before the desktop and phone
build on it: Beads `zerus-89h.10`, `.9`, `.3.1`, `.6`, `.7`, `.8` and `.2.4`.

**Architecture:** Small, independent fixes inside `src/overmind/` (store, sync, deliver,
remote, templates, fold) plus best-effort hooks in the session host (`src/state/`) like the
existing `overmind::session_renamed`. No new modules except where a task says so.

**Tech Stack:** Rust 2021 (`rusqlite`, `serde_json`, `uuid` v4, `base64`, `sha2`), Python
`unittest` with the isolated fixtures in `tests/test_overmind.py`, `tests/test_overmind_sync.py`
and `tests/test_input.py`.

**Spec:** `docs/superpowers/specs/2026-10-11-overmind-core-design.md` (binding). Plan 1b:
`docs/superpowers/plans/2026-10-12-overmind-1b-sync-and-send.md` (its Decisions still hold).

## Global Constraints

- Minimum Rust 1.85, edition 2021: no let-chains, no std APIs newer than 1.85. No new crates or crate features; `cargo build --locked`; 0 warnings.
- `src/overmind/` and every call into it are Unix-only (`#[cfg(unix)]` at call sites outside it); Windows CI runs `cargo +1.85.0 clippy --all-targets -- -D warnings`.
- Agent-facing text is English; journal markers are ASCII and frozen.
- Lock and transaction rules: never hold an Overmind SQLite write transaction across `crate::state::deliver_input` (state lock) or `crate::swarm::call` (SSH); never call `crate::swarm::local_node_id` while a `LockedStore` is held; hooks called from the session host never fail the host's command and wait at most about a second for Overmind.
- Tests never touch live state or the live tmux server: temporary `HOME`/`HGS_CONFIG_DIR`/`HGS_STATE_DIR`; `tmux` shims live in `$HOME/.local/bin` (hgs puts it first in PATH); `HGS_SSH`, `TMUX`, `TMUX_PANE`, `HGS_SESSION`, `HGS_RUN_ID` removed; never a bare `tmux kill-server`. Python tests run with `PYTHONPATH=tests`.
- Commit after each task on branch `overmind` with `git commit --only <own paths>`; plain sentence messages; no Claude attribution; `python3 scripts/ci/check-source.py` passes.
- Close each task's Beads issue (`bd close <id> --reason "..."`) after its commit; never run `bd export`.

## Review Focus

- Read-only commands (`status`, `board`, `show`, `rev`) and swarm serving never wait for another writer just because they opened the store. Test in Task 1 (`opening_a_store_never_takes_the_write_lock`).
- A store restored from an old copy that wrote before being refilled stops writing under the colliding origin and says so in `status`. Test in Task 1 (`a_restored_store_rotates_its_origin`).
- A forwarded command's failure after `hello` is always a JSON refusal with a code the GUI can act on. Test in Task 2 (`test_forward_failures_are_refusals`).
- Archiving or terminating a session flags its active tasks `orphaned` without ever failing the archive. Test in Task 3 (`test_archiving_a_session_orphans_its_tasks`).
- Two people editing different templates of one project at once keep both edits. Test in Task 4 (`concurrent_edits_of_different_templates_both_survive`).

---

### Task 1: Store opening and restored stores (`zerus-89h.10`, `zerus-89h.9`)

**Files:** `src/overmind/store.rs`, `src/overmind/sync.rs`, `src/overmind/deliver.rs` (forward receipt), tests in those modules.

- [ ] **Step 1: Opening never takes the write lock.** `Store::open` today runs `INSERT OR IGNORE INTO meta` on every open, which takes SQLite's write lock even when the origin exists. Read the origin with `SELECT value FROM meta WHERE key = 'origin'`; only when absent, `INSERT OR IGNORE` a fresh UUID v4 and read it back (two first openers still agree). Test `opening_a_store_never_takes_the_write_lock`: create a store, hold `BEGIN IMMEDIATE` on a second raw `rusqlite::Connection` to the same file, then `Store::open` with a short busy timeout succeeds and `load`/`version_vector` work.
- [ ] **Step 2: A forwarded result never fails after the host acted.** In `deliver.rs` (the routing wrapper that remembers a forwarded result): write the caller's receipt only when the request carries a `request_id`; if writing it fails, print one warning line to stderr (`hgs overmind: could not record the forwarded result: <error>`) and still return the host's result. Test with a request without `request_id` (no receipt row written).
- [ ] **Step 3: A restored store rotates its origin.** Add `Store::origin()` (exists or add) and `Store::rotate_origin(&self) -> Result<String>` (replace the `meta` origin with a fresh UUID v4; local events written afterwards use it). In `sync::rounds` (caller) and `sync::serve` (peer): when the other side's vector holds more events of *this store's current origin* than this store has, this store is an older copy that peers have outgrown: rotate the origin before any further local write, and record in the peer state `last_error`: "this store is older than its copy on <peer>; new events use a fresh origin; tasks changed here before the exchange may differ from the other machines". `serve` answers with an extra field `refused` (count of events it refused from this request); the caller adds it to its `last_error` when non-zero. Tests: `a_restored_store_rotates_its_origin` (exchange A↔B; copy B's database file aside; write more on B; restore the old copy as B; exchange: B's origin changed, B's later local events use the new origin, `last_error` on B names the restore) and `serve_reports_refused_events` (a request with a different id at a known position → response `refused == 1` and the caller's `last_error` mentions it).
- [ ] **Step 4: Verify and commit.** `cargo test --locked overmind`, `cargo build --locked 2>&1 | grep -c '^warning'` (0), `PYTHONPATH=tests HGS_TEST_BIN=$PWD/target/debug/hgs python3 -m unittest tests.test_overmind tests.test_overmind_sync`, `python3 scripts/ci/check-source.py | tail -2`. Commit: "Open Overmind stores without the write lock and give a restored store a fresh origin". Close `zerus-89h.10` and `zerus-89h.9`.

---

### Task 2: Forwarded commands for the desktop and the phone (`zerus-89h.3.1`)

**Files:** `src/overmind/remote.rs`, `src/overmind/deliver.rs`, `tests/test_overmind_sync.py`.

- [ ] **Step 1: Every failure after `hello` is a JSON refusal.** In `remote::forward`: the identity check (the alias now leads to another machine) → `Refusal::field("host_changed", "session", "invalid", ...)`; a failed exchange before delivery → `host_sync_failed` (nothing was attempted on the host: say so); a failed blob push → `host_sync_failed`; a failed `overmind-deliver` call → `delivery_unknown` (already); an unknown-command answer from the host → `host_outdated`. A failed pull after a successful delivery stays a success.
- [ ] **Step 2: Messages and fields read from the caller's side.** The host's `remote_session` refusal says "the session is not on <host machine id>; no further forwarding". For `rework-send` and `answer` (no `session` in the request), refusals about the receiving session use field `task`, not `session`.
- [ ] **Step 3: The returned task is this machine's view.** After the pull, rebuild `result["task"]` from the local store (`load` + `view` with the local `reference`), so its `ref` is valid here; if the pull failed, keep the host's view.
- [ ] **Step 4: End-to-end tests** in the `RemoteDelivery` class (live fake agent on `b`, caller `a`): `review-send` to the session on `b` (task in Review assigned to another session, set up with the SQLite `append` helper on `b` then synced); `rework-send` and `answer` whose assignee is the live session on `b`; a host refusal round trip (the agent on `b` busy → `send` from `a` refuses `agent_busy` with field `confirm`; with `confirm: ["agent_busy"]` it is `sent`); `test_forward_failures_are_refusals` (identity change by swapping `b`'s node id in its catalog → `host_changed`; host without `overmind-deliver` → `host_outdated`); and stronger assertions in the existing send test: exactly one `[claim]`, and the `[sent]` entry's actor machine is `b`.
- [ ] **Step 5: Verify and commit** (the Task 1 commands plus `tests.test_input`). Commit: "Return structured refusals and this machine's task view for forwarded Overmind commands". Close `zerus-89h.3.1`.

---

### Task 3: Orphaned tasks (`zerus-89h.6`)

**Files:** `src/overmind/sessions.rs`, `src/overmind/fold.rs`, `src/state/` (the archive, terminate, forget and kill paths), tests.

- [ ] **Step 1: The hook.** Add `pub fn session_ended(name: &str)` to `sessions.rs`, best effort exactly like `session_renamed` (no store → nothing; 1 s busy timeout; errors printed, never returned): for every task whose `assignee` is `(this machine, name)` and whose stage role is active or needs-human, append `flag_set {flag: "orphaned", value: true}` as `Actor::human(machine)` with the task's project. Export it from `mod.rs`.
- [ ] **Step 2: Call it** wherever a tracked session on this machine stops being able to take input for good: manual and scoped archive (`src/state/archive.rs`), terminate (`src/state/terminate.rs`), forget of a native DeepSeek session (`src/state/dsh.rs` forget paths), and `hgs kill` of a live session (`src/cli.rs` `kill`, after the session is gone). Call it after the host's own lock is released when the code makes that easy (as `rename.rs` does), otherwise accept the 1 s bound. Pausing is not an end.
- [ ] **Step 3: The fold clears it.** A transition that changes the assignee (`claim`, `unclaim`, `return`, `decompose`) removes the `orphaned` flag. This changes how existing kinds fold; the event version stays 1 because nothing is released yet (note this in the spec's Compatibility section).
- [ ] **Step 4: Tests.** Rust: the fold clears `orphaned` on unclaim and on a new claim. Python (`tests/test_overmind.py` `AgentSession`, live fake agent): a task claimed for the live session, then `hgs archive <session>` (or terminate) → `show` has the `orphaned` flag and the archive succeeded; `move` back to Queue clears it. Also a machine without an Overmind store archives without creating one.
- [ ] **Step 5: Docs and commit.** docs/reference.md and spec Part 8 ("Assignee session terminated or archived") describe the flag and how it clears. Commit: "Flag Overmind tasks whose session was archived or terminated as orphaned". Close `zerus-89h.6`.

---

### Task 4: Templates per name (`zerus-89h.7` and the template items of `zerus-89h.8`)

**Files:** `src/overmind/templates.rs`, tests.

- [ ] **Step 1: One setting per template.** `templates set` writes one `settings_set` event per changed name: payload `{"key": "template", "name": <name>, "value": <text or null>}`. `custom()` takes, for each name, the latest such event (total order, people only, `v <= EVENT_VERSION`); `null` means built-in. The old whole-map `{"key": "templates"}` events are ignored (nothing is released). A name whose text does not change writes nothing; a request that changes nothing writes nothing and returns the listing.
- [ ] **Step 2: Tests.** `concurrent_edits_of_different_templates_both_survive` (two stores; A sets `dispatch`, B sets `review`, exchange both ways, both see both); the control-character refusal of `check_rendered`; an agent's or a newer-version `settings_set` is ignored by `custom()`; a no-op `set` appends no event.
- [ ] **Step 3: Verify, docs (spec Part 4 Templates: stored per template), commit** "Store Overmind templates one per name so concurrent edits of different templates both survive". Close `zerus-89h.7`.

---

### Task 5: Delivery follow-ups and renamed pins (`zerus-89h.8`, `zerus-89h.2.4`)

**Files:** `src/state/input.rs`, `src/overmind/deliver.rs`, `src/overmind/remote.rs`, `src/overmind/commands.rs`, tests.

- [ ] **Step 1: Preflight matches `submit`.** `submit` refuses through `effort::apply_pending` before typing; `input::preflight` does not, so a stale pending-settings record writes a claim and an unclaim instead of nothing. Expose a read-only check in `effort` (the conditions `apply_pending` refuses on, without applying anything) and call it from `preflight`. Test with a record whose pending settings `apply_pending` would refuse: `send` refuses with nothing written.
- [ ] **Step 2: Session checks before image fetches.** In `send`, `review-send` and `rework-send`, run `local_target`/`check_session` (fresh requests only, read-only) before `remote::ensure_blobs`, so a send to a stopped session makes no SSH fetch and is not reported as `attachment_unavailable`.
- [ ] **Step 3: Cheaper and safer image serving.** `serve_blob` reads only the requested range (`File::seek` + bounded `read_exact`/`take`), taking `size` from the file's metadata. `serve_blobs` and `fetch` reject a base64 string whose encoded length already exceeds the size limit before decoding it.
- [ ] **Step 4: `active_tasks` folds once.** Fold all events once and look up each project's workflow once per project (a small map), instead of per task.
- [ ] **Step 5: `pin` takes a session without `machine`,** like `send` (empty or missing = this machine).
- [ ] **Step 6: Renamed pins (`zerus-89h.2.4`).** Since 1b, assignees and reviewers are written by the session's host, which always uses the current name; only pins are written on any machine and can name a session that its host renamed concurrently. On the host, `send`'s `pinned_elsewhere` gate resolves the pin's name through this machine's own `session_renamed` events (oldest first, following `old → new` while no tracked session of the current name exists here) before comparing; `show`/`board` add `pinned_current` (the resolved name) when the task is pinned to a session of this machine and the name changed. Record in the spec (Part 8, Session renamed) that other machines show the pin as written until the host resolves it.
- [ ] **Step 7: Tests** for steps 1–6 that the 1b reviews listed as missing: the DeepSeek refusal (`dsh/...` name with a native binding file → `session_unavailable`), `unknown_session`, a stopped session (pane gone) and a paused session refuse with nothing written, `unpin`, the lenient render path on a resumed request (a template edited after typing that no longer renders → `uncertain`, not a refusal), the `not_delivered` hint naming the request_id for `answer`, a renamed pin (pin `codex/demo/old`, rename it to `codex/demo/new` on the host after a concurrent pin event, `send` to `new` passes without `pinned_elsewhere`).
- [ ] **Step 8: Verify and commit** (all suites of Task 2). Commit: "Close the Overmind delivery follow-ups and resolve renamed pins on the session's host". Close `zerus-89h.8` and `zerus-89h.2.4`.

---

### Task 6: Documentation sweep

- [ ] **Step 1:** docs/reference.md "Overmind tasks" and the spec reflect Tasks 1–5 (fresh origin on restore, refusal codes `host_changed`/`host_sync_failed`/`host_outdated`/`delivery_unknown`, `orphaned`, templates per name, `pinned_current`). Keep the help in `src/overmind/cli.rs` in sync.
- [ ] **Step 2:** `python3 scripts/ci/check-source.py | tail -2`. Commit "Document the Overmind follow-ups".
