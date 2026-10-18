# Native Answer Channels Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Pending Claude questions become answerable from Activity and phones whatever the terminal size. Claude questions are answered through a native PermissionRequest hook channel (C). Screen delivery remains as the fallback and grows unviewed panes (A).

**Architecture:** P1 makes every disabled Submit explain itself and applies the shared unviewed-pane growth (`question_terminal::fit_unviewed`, from P0) to Claude and Kimi question panels. P2 adds `src/state/answer_channel.rs`. A long-running `hgs __state answer-channel` hook process binds to the exact pending card and waits for an answer handed over through atomic files. `hgs answer` and inspection prefer that channel when it is live. Otherwise they keep the verified screen path.

**Tech Stack:** Rust CLI (`src/state`), Qt 6 GUI (`tray/src`, QtTest), Python native tests against real Claude 2.1.296 with a localhost API fixture and isolated tmux sockets.

**Spec:** `docs/superpowers/specs/2026-10-17-native-answer-channels-design.md`

## Global Constraints

- Keep the `hgs` CLI, API v1, `hgs answer` JSON, `question_receipts/<request_id>.json` statuses (`in_progress`, `answered`, `submitted`, `rejected`, uncertain errors) and durable request identities compatible.
- Every answer reaches the agent at most once; unknown delivery reports `delivery uncertain` and is never retried or re-sent through another transport.
- Never resize or type into a pane that a client views; width never changes; growth cap `MAX_UNVIEWED_ROWS = 192` (P0 constant).
- `answer_unavailable_reason` is never empty when `can_answer` is false; a disabled Submit always shows a reason.
- Register the channel hook only for a Claude version read from the native CLI and qualified (`>= 2.1.296`); prune it before an older version starts.
- The channel process exits silently without output when `HGS_SESSION`/`HGS_RUN_ID` do not name a live tracked record.
- Native terminal changes need isolated real-agent coverage on pinned Claude 2.1.296; a skipped native suite is not compatibility evidence.
- Test tmux operations only with isolated socket directories; never touch live agents.
- Repository text in English; no absolute home paths in Beads or committed files.
- Rust 1.85 compatibility; no new crates.

## Review Focus

1. A person answers in Terminal while Zerus waits for the hook to claim: the channel exits, no late answer is printed, Zerus reports already answered. Covered in Task 8, `test_channel_terminal_answer_first`.
2. Two clients submit the same question at once: exactly one claim and one native answer. Covered in Task 5, `two_offers_claim_once`, and in Task 8, `test_channel_double_submit`.
3. The channel process dies or times out mid-wait: the next answer falls back to the screen after A, never both. Covered in Task 6, `dead_channel_uses_screen`, and in Task 8, `test_channel_killed_falls_back`.
4. An Other answer that contains `, ` or a multi-choice answer: the native result still matches the receipt. Covered in Task 5, `answer_text_matches_response_format`, and in Task 8, `test_channel_multi_and_other`.
5. A session ends or the run changes while a channel waits: the channel exits and inspection stops offering it. Covered in Task 4, `channel_exits_when_card_or_run_changes`.

---

## P1: A for questions, reasons never empty

### Task 1: Disabled Submit always explains itself

**Files:**
- Modify: `tray/src/QuestionCard.cpp` (`QuestionCard::updateControls`)
- Test: `tray/tests/test_questioncard.cpp`

**Interfaces:**
- Consumes: question JSON fields `can_answer`, `answer_unavailable_reason`.
- Produces: no API change. The status label is non-empty whenever `m_submit` is visible and disabled.

- [ ] **Step 1: Write the failing tests.** Add two cases to `test_questioncard.cpp`:
  - a card with `can_answer:false` and `answer_unavailable_reason:""` shows "Answer this request in Terminal.";
  - a card whose draft is still loading shows "Loading your saved answer…" while Submit is disabled.
- [ ] **Step 2: Run them and confirm they fail.** Run `ctest --test-dir <build> -R questioncard --output-on-failure`.
- [ ] **Step 3: Implement.** In `updateControls`:
  - replace `m_question.value("answer_unavailable_reason").toString(tr(...))` with a check that falls back when the string is empty;
  - add `if (notice.isEmpty() && m_loadingKeys.contains(m_key)) notice = tr("Loading your saved answer…");` before the generic notices.
- [ ] **Step 4: Run the questioncard tests until they pass,** then run the workspace question tests (`-R "questioncard|sessionswindow"`).
- [ ] **Step 5: Commit** with the message "Explain every disabled question Submit".

### Task 2: Claude question panels report clipping and completeness

**Files:**
- Modify: `src/state/claude_question.rs`
- Test fixture: `tests/fixtures/claude-question-clipped-80x24.txt`, a styled capture from the isolated 80x24 repro with the header scrolled off.

**Interfaces:**
- Produces `pub(super) fn clipped(screen: &str) -> bool`. True when the panel's footer hints are visible but no complete panel top is: no divider followed by a `☐`/`☒` tab row or a `←…→` tab bar.
- Produces `pub(super) fn complete(screen: &str) -> bool`. True when a divider plus a tab row and the footer hints are both visible.
- Both have the signature `fn(&str) -> bool` that `question_terminal::fit_unviewed` expects.

- [ ] **Step 1: Capture the fixture.** Use the scratch repro (`repro_size.py`, 80x24) with `capture-pane -p -e` and save the styled screen with the synthetic Cyrillic options.
- [ ] **Step 2: Write the failing unit tests.**
  - `clipped` is true for the new fixture;
  - `clipped` is false and `complete` is true for `claude-question-single.txt` and `claude-question-review.txt`;
  - both are false for a screen without any panel.
- [ ] **Step 3: Implement** both functions from `question_terminal::plain` rows. Reuse `divider()` and the same tab-row and footer-hint tests that `parse()` uses: concatenated footer rows contain `Entertoselect`, `Esctocancel` and `navigate`.
- [ ] **Step 4: Run `cargo test claude_question`** and confirm the tests pass.
- [ ] **Step 5: Commit** with the message "Recognize clipped Claude question panels".

### Task 3: Fit clipped question panels in unviewed panes

Depends on P0, PR #218 (`grow_unviewed`, `question_terminal::fit_unviewed`, `questions::answer_lock`, `MAX_UNVIEWED_ROWS`). Rebase on main after it merges. P0 grows only in the listing path (`questions::collect` without a deadline, through `claude_approval::listed`). `current()` and `available()` stay read-only. Questions follow the same rule.

**Files:**
- Modify: `src/state/claude_question.rs` (new `pub(super) fn fit(record: &Value, card: &Value)`), `src/state/question_terminal.rs` (Kimi `fit`, `clipped` and `complete`), `src/state/questions.rs` (`inspection` calls `fit` before `available` for recorded `hook` and `kimi_wire` cards)
- Test: `tests/test_claude_questions_native.py`; Rust unit tests on the P0 fake backend

**Interfaces:**
- Consumes `question_terminal::fit_unviewed(record, screen, capture, clipped, complete) -> String`, and `claude_question::clipped` / `complete` from Task 2.
- Produces `claude_question::fit(record, card)` and `question_terminal::fit(record, card)`. Each grows the unviewed pane of a pending recorded card when needed. `available()` stays unchanged and read-only.

- [ ] **Step 1: Write the failing native test** `test_single_tall_question_in_unviewed_80x24_pane`. Start Claude at 80x24 with the four long Cyrillic options. Assert that `inspect()` gives `can_answer: true` and that the pane height grew. Then answer through `hgs answer` and assert the transcript result.
- [ ] **Step 2: Run it** with `HGS_CLAUDE_TEST_BIN=/usr/bin/claude python3 tests/test_claude_questions_native.py NativeClaudeQuestions.test_single_tall_question_in_unviewed_80x24_pane` and confirm it fails with "not fully visible".
- [ ] **Step 3: Implement.**
  ```rust
  // claude_question.rs
  pub(super) fn fit(record: &Value, card: &Value) {
      if string(card, "source") != "hook" || record["pending_questions"].get(string(card, "tool_call_id")).is_none() { return; }
      if let Ok(screen) = Native::capture(record) {
          let _ = super::question_terminal::fit_unviewed(record, screen, Native::capture, clipped, complete);
      }
  }
  ```
  In `questions::inspection`, call `claude_question::fit(record, card)` before `claude_question::available`. Kimi gets `question_terminal::fit`:
  - `clipped`: option rows and the dialog's left border are visible, but the `question` heading or the bottom border is missing;
  - `complete`: both are visible.
  Check whether `codex_async` panels can be clipped at 80x24 (native Codex test at 80x24). Add detectors only if they can.
- [ ] **Step 4: Run the native test, all of `tests/test_claude_questions_native.py`, and `cargo test`.** The existing 120x45 and 87x45 cases must not grow.
- [ ] **Step 5: Commit** with the message "Fit clipped question panels in unviewed panes".

---

## P2: Claude question channel (C)

### Task 4: Channel rendezvous files

**Files:**
- Create: `src/state/answer_channel.rs` (register `mod answer_channel;` in `src/state/mod.rs`)
- Test: unit tests in the same file

**Interfaces:**
- Produces:
  ```rust
  pub(super) const PROTOCOL: u64 = 1;
  pub(super) fn directory(record: &Value, card: &Value) -> Result<PathBuf>;
  // root/native_answers/<sha256(run_id)>/<sha256([run, conversation, question_id, question_hash])>
  #[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
  pub(super) struct Waiting { protocol: u64, pid: u32, process_start: String, run_id: String,
      conversation_id: String, question_id: String, question_hash: String, started_at: f64, deadline: f64 }
  pub(super) fn live(record: &Value, card: &Value) -> Option<Waiting>;
  pub(super) fn offer(dir: &Path, offer: &Value) -> Result<()>;          // atomic answer.json
  pub(super) enum Taken { Claimed, Retracted }
  pub(super) fn await_claim(dir: &Path, within: Duration) -> Result<Taken>; // rename race
  fn claim(dir: &Path) -> Result<Option<Value>>;                          // answer.json -> claimed.json
  fn close(dir: &Path, reason: &str);                                     // closed.json
  ```
- `live()` returns `Some` only in this case:
  - `waiting.json` parses with `PROTOCOL` and the exact identity;
  - `process_start(pid) == process_start`;
  - none of `closed.json`, `claimed.json`, `emitted.json` or `answer.json` exists;
  - `now < deadline`.

- [ ] **Step 1: Write the failing tests.**
  - `live_requires_matching_identity_and_process`;
  - `two_offers_claim_once`: two threads, where one claims and one retracts the same `answer.json`, and exactly one rename wins over 200 iterations;
  - `retract_after_timeout_without_claim`;
  - `channel_exits_when_card_or_run_changes` uses the state check from Task 5 and lands in Task 5.
- [ ] **Step 2: Run `cargo test answer_channel`** and confirm it fails because the module is missing.
- [ ] **Step 3: Implement.**
  - Use `storage::atomic` for writes and `std::fs::rename` for claim and retract.
  - `await_claim` polls `claimed.json` every 50 ms. At the deadline it renames `answer.json` to `retracted.json`. If that rename fails with `NotFound` and `claimed.json` exists, the result is `Claimed`.
  - Directories are private (`storage::private_dir`).
- [ ] **Step 4: Run the tests until they pass.**
- [ ] **Step 5: Commit** with the message "Add native answer channel rendezvous".

### Task 5: The `__state answer-channel` hook process

**Files:**
- Modify: `src/state/answer_channel.rs`, `src/state/mod.rs` (dispatch `if command == "answer-channel" { return answer_channel::run(); }`), `src/state/questions.rs` (extract `pub(super) fn answer_text(question: &Value, answer: &Value) -> String` from `response_matches`)
- Test: unit tests

**Interfaces:**
- Consumes the Task 4 API, `storage::read_run`, `questions::normalize` and `process_alive`.
- Produces:
  - `pub(super) fn run() -> Result<i32>`;
  - `fn bind(record: &Value, event: &Value) -> Option<Value>`. It returns the single main-agent `source:"hook"` card whose `questions == normalize(event.tool_input.questions)`;
  - `fn decision(event: &Value, card: &Value, answers: &Value) -> Value`.
- Hook output when it answers:
  ```json
  {"hookSpecificOutput":{"hookEventName":"PermissionRequest","decision":{"behavior":"allow","updatedInput":{"questions":<event.tool_input.questions>,"answers":{"<question text>":"<answer_text>"}}}}}
  ```

- [ ] **Step 1: Write the failing tests.**
  - `bind_needs_exactly_one_matching_card`: zero matches, two matches, a child agent and another tool all give `None`;
  - `answer_text_matches_response_format`: labels in option order plus Other joined with `", "`, and `response_matches` accepts the result;
  - `decision_passes_original_questions`;
  - `channel_exits_when_card_or_run_changes`: `should_close()` returns a reason when the record's run changes, the card leaves `pending_questions` or the process is gone.
- [ ] **Step 2: Run `cargo test answer_channel questions::`** and confirm the tests fail.
- [ ] **Step 3: Implement `run()`.**
  1. Without `HGS_SESSION` and `HGS_RUN_ID`, exit with `Ok(0)` and no output.
  2. Read the event from stdin. Skip it unless it is a main-agent `PermissionRequest` for `AskUserQuestion`.
  3. Bind with retries for 5 s at 100 ms. The card may arrive with the PreToolUse journal.
  4. Write `waiting.json` with a deadline of `started_at + 86000`.
  5. Loop every 150 ms:
     - if `should_close` gives a reason, call `close(reason)` and return 0;
     - if `claim()` gives an answer, re-run `bind` and `should_close`. On failure, write `closed.json`, rename `claimed.json` to `rejected-claim.json` and return 0 with no output.
  6. Write `emitted.json` with the request id, print the decision, return 0.
- [ ] **Step 4: Run the tests until they pass.**
- [ ] **Step 5: Commit** with the message "Answer Claude questions through a waiting hook".

### Task 6: Route inspection and `hgs answer` through a live channel

**Files:**
- Modify: `src/state/questions.rs` (`inspection`, `dispatch`)
- Test: unit tests in `questions.rs`, using a fake channel that writes `waiting.json` with the current process identity and claims from a thread

**Interfaces:**
- Consumes `answer_channel::{live, directory, offer, await_claim, Taken}` and `answer_text`.
- Produces inspection fields `can_answer:true` and `answer_transport:"claude_hook"` when `live()` is `Some`, and the receipt field `"transport":"claude_hook"|"claude_tui"`.

- [ ] **Step 1: Write the failing tests.**
  - `inspection_prefers_live_channel`;
  - `answer_uses_claimed_channel`: the receipt becomes answered after a fake transcript result;
  - `unclaimed_offer_retracts_then_uses_screen`;
  - `dead_channel_uses_screen`;
  - `claimed_without_confirmation_is_uncertain`.
- [ ] **Step 2: Run `cargo test questions::`** and confirm the tests fail.
- [ ] **Step 3: Implement.**
  - In `inspection`, for a Claude `hook` card, check `answer_channel::live` before `claude_question::available`.
  - In `dispatch`, take the channel route when `live()` is `Some` at preparation:
    - skip the screen availability check;
    - after the `in_progress` receipt, offer `{request_id, answers:{question: answer_text}}`;
    - on `Taken::Claimed`, go to the existing confirmation loop. Its 6 s budget becomes `uncertain`;
    - on `Taken::Retracted`, run `claude_question::available` and then `answer` (the screen path).
- [ ] **Step 4: Run `cargo test`** and confirm everything passes.
- [ ] **Step 5: Commit** with the message "Prefer a live Claude answer channel".

### Task 7: Register the channel hook for qualified Claude versions

**Files:**
- Modify: `src/state/hooks.rs`
- Test: unit tests in `hooks.rs`, following `claude_model_switch_is_registered_only_for_versions_that_have_it`

**Interfaces:**
- Produces:
  - `const CLAUDE_ANSWER_CHANNEL: Version = (2, 1, 296);`
  - `const CHANNEL_ARGS: [&str; 2] = ["__state", "answer-channel"];`
  - `fn owns_channel(hook: &Value, windows: bool) -> bool`
  - the group `{"matcher":"AskUserQuestion","hooks":[{"type":"command","command":"<hgs> __state answer-channel","timeout":86400}]}` on `PermissionRequest`. On Windows it uses the exec form `args`, like `Entry`.

- [ ] **Step 1: Write the failing tests.**
  - `answer_channel_is_registered_only_for_qualified_claude`;
  - `answer_channel_entry_goes_for_an_older_version`;
  - `answer_channel_keeps_user_hooks_and_bytes_when_present`;
  - `codex_and_kimi_get_no_answer_channel`.
- [ ] **Step 2: Run `cargo test hooks::`** and confirm the tests fail.
- [ ] **Step 3: Implement.**
  - Derive the channel entry from the hgs hook `Entry` by replacing the final `hook` argument.
  - Add the group in `merge_json` when the agent is `claude` and `version() >= CLAUDE_ANSWER_CHANNEL`. Remove it in `prune_json` for an older version that can be read.
- [ ] **Step 4: Run the tests until they pass.**
- [ ] **Step 5: Commit** with the message "Register the Claude answer channel hook".

### Task 8: Native qualification on real Claude 2.1.296

**Files:**
- Modify: `tests/test_claude_questions_native.py`. The fixture API waits for a `ready` event before returning the tool use. The record is written first, and Claude's settings get the real `__state hook` journal entries plus the channel entry with `HGS_SESSION`, `HGS_RUN_ID` and `HGS_STATE_DIR` in its environment.

**Interfaces:**
- Consumes the installed test binary (`HGS_TEST_BIN`) and the Task 5 to 7 behavior.

- [ ] **Step 1: Write the tests.**
  - `test_channel_answers_tall_question_at_80x24_without_growth`: pane height stays 24 and the receipt `transport` is `claude_hook`.
  - `test_channel_multi_and_other`
  - `test_channel_several_questions`
  - `test_channel_terminal_answer_first`: a keyboard answer in the pane; the channel process exits within 2 s; a later `hgs answer` is rejected.
  - `test_channel_killed_falls_back`: SIGKILL the channel pid from `waiting.json`; the answer goes through the screen.
  - `test_channel_double_submit`: two concurrent `hgs answer` calls with different request ids; one answered, one rejected.
  - `test_channel_deadline_leaves_panel`: an environment override shortens the deadline; the panel remains and is answerable through the screen.
- [ ] **Step 2: Run them** with `HGS_CLAUDE_TEST_BIN=/usr/bin/claude HGS_TEST_BIN=target/debug/hgs python3 tests/test_claude_questions_native.py`, then the full native Claude suites (`test_claude_*_native.py`).
- [ ] **Step 3: Fix any failures in the owning task's code,** with the test first.
- [ ] **Step 4: Commit** with the message "Qualify the Claude answer channel natively".

### Task 9: Documentation, tracker and rollout

**Files:**
- Modify: `docs/reference.md` (questions section: channels, transports, reasons, unviewed growth), the spec's status line
- Tracker: close `zerus-0yrj` after merge; file follow-ups

- [ ] **Step 1: Document** the user-visible behavior and the `claude_hook` transport.
- [ ] **Step 2: Run the full local checks:** `cargo test`, the GUI ctest, the native Claude suites and `python3 scripts/check-source-privacy.py`.
- [ ] **Step 3: Update Beads.**
  - File follow-ups:
    - P4 Codex channel spike;
    - P4 Kimi channel spike;
    - P5 Claude's own questions (`refusal_fallback_preference`) through the channel;
    - P5 host and Windows parity for `grow_unviewed`, if the agent-approval session has not filed it.
  - Run `bd export -o .beads/issues.jsonl`.
- [ ] **Step 4: Open a PR to main** and wait for the CI gate. Merge when green. Roll the GUI and CLI out with the documented installers, checking running builds and coordinating with other sessions first.

## Later phases (separate plans after their spikes)

- P3 Claude approvals through the channel: owned by the agent-approval session. It reuses Task 4 and Task 5 with `behavior: allow/deny`.
- P4 Codex and Kimi: spike first, as in the Claude spike. Adapters implement `bind`, `available`, `deliver`, `confirm` and `close` and plug into Task 6 routing.
- P5 Claude's own questions through the channel (bind by `PermissionRequest` without PreToolUse), and host and Windows `grow_unviewed`.
