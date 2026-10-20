# Android Tasks Screen Implementation Plan (5c2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The Android app gets the Tasks screen of the spec's "Android interface" over the phone's replica:
- the entry with the In review badge, session chips, stage tabs, cards with the stage actions, the task settings sheet, the input, the cancel dialog and the project's task settings;
- Send now and Cancel through the session's computer, after the phone's edits reach it;
- Done from a computer in pages, with "Show older (N)" and opening a Done task.

**Architecture:**
- **Logic.**
  - `TaskActions` (`TaskSync.kt`) carries the relay calls under the project's lock:
    - Send now and Cancel: exchange first, then `task_send` or `task_cancel`, then exchange;
    - the Done page: `task_done`;
    - opening a Done task: an exchange that fetches it.
  - `TasksPresentation` is the pure model of what the screen shows: tabs, counts, default tab, cards, actions with their state and reasons, highlight, chips.
  - Both are JVM-tested.
- **UI.** The Compose screen is a full-screen `Dialog` + `Scaffold`, like Launch and Terminal. The ViewModel holds the open project, the chosen session, the listing and Done pages. It runs every `TaskStore` call on `Dispatchers.IO` and syncs the project after each local edit.

**Tech Stack:** Kotlin and Jetpack Compose (Material3 1.3.2), the UniFFI bindings of `zerus-replica-ffi`, JUnit4 with OkHttp MockWebServer.

**Spec:**
- `docs/superpowers/specs/2026-10-13-project-task-queue-design.md`: "Interface" (stage actions, rows, task settings), "Android interface" and "Failure cases".
- `docs/superpowers/specs/2026-10-18-task-replication-loro-design.md`: "Phone" (Send now and Cancel from the phone, Done on the phone) and "Done tasks".

## Global Constraints

- **Order and actions** (spec, verbatim):
  - Stage order: "In review, In progress, To do, Backlog, Done". Default tab: "In review when it has tasks, otherwise To do".
  - Actions: "Edit (pencil), Move to Backlog (clock), Move to top (double chevron) and Send now (play) in To do; Edit, Move to To do (list with a plus) and Send now in Backlog; Cancel (✕) … in In progress; Back to To do (list with a plus) and Accept (check) in In review; Back to To do in Done."
- **Phone layout** (spec, verbatim):
  - "Tasks opens on the session of the conversation it was opened from. A tap on a chip picks another session for Tasks only."
  - "There is no dragging on the phone: the session changes in the task settings, and To do is reordered with Move to the top."
- **Colours:** "bottom sheets #1A2225, dialogs #1F292C, snackbars #2A3538, sheet handles #354449. The existing Session details sheet should follow."
- **Notifications:** "Notifications: none for tasks."
- **Rename:** the Session details "Tasks" tab is renamed "Plan".
- **Send now on the phone** first exchanges with the session's computer, and is refused with the reason when that fails (owner decision, 2026-10-20). An uncertain receipt says the task may have been sent. A refusal shows hgs's reason in a snackbar.
- **Done** shows the last 7 days, then "Show older (N)", in pages of 50 from a computer (owner decision, 2026-10-20).
- **Code rules:**
  - `TaskStore` calls run on `Dispatchers.IO`, and stores and exchanges are closed;
  - `CancellationException` is always rethrown;
  - text is hard-coded English as elsewhere in the app;
  - fixtures use invented names (`orchard`, `codex/api`).
- **Check:** `cd mobile/android && JAVA_HOME=$HOME/.jdks/jdk-17.0.20.1+1 ANDROID_HOME=$HOME/Android/Sdk ./gradlew --no-daemon testDebugUnitTest lintRelease checkReplicaPackaging`. There is no emulator here: the screen is checked on a device by the owner.
- **Commits:**
  - No co-author lines.
  - Stage only these files.
  - Never push, merge, rebase or switch branches.

## Review Focus

1. **Send now never runs on stale edits, and never twice.**
   - The exchange with the session's computer precedes `task_send`, and a failed exchange sends nothing.
   - An uncertain receipt is reported as possibly sent, and the action stays disabled until the next exchange shows the stage.
   - Pinned by `aSendFirstBringsThePhonesEdits`, `aFailedExchangeSendsNothing` and `anUncertainSendSaysSo`.
2. **Routing.**
   - A task pinned to a session goes to the computer that runs it: native machine ID → relay computer.
   - An Any session task goes to the chosen chip's session.
   - An offline computer, or one without `task_send`, disables Send now with the reason.
   - Pinned by `TasksPresentationTest`.
3. **Done pages.**
   - "Show older (N)" shows N = total − recent and loads the next page from the cursor.
   - A failed page keeps the shown rows.
   - Opening a Done row fetches the task, then the settings open.
   - Pinned by `TaskActionsTest` and `TasksPresentationTest`.
4. **Edits sync.** Every local edit (add, edit, move, stage, target, comment, settings) triggers an exchange of that project, and the listing refreshes after each sync. Pinned by the ViewModel glue being thin over `TaskSync` (no Android unit tests), and by `TaskActionsTest.editsReachTheComputer`.
5. **Only this session.** It keeps the chosen session's tasks and the Any session tasks in To do and Backlog, and the counts follow. Pinned by `TasksPresentationTest`.

---

### Task 1 (done, dfaa138c): fetching exchanges

`Exchange::fetch` in `zerus-replica` and `TaskStore.exchangeFetching(tasks)` in the bindings. Tests: `a_done_task_the_phone_asks_for_arrives_and_stays` and `two_stores_exchange_through_json`.

---

### Task 2: `TaskActions`

**Files:**
- Modify: `mobile/android/app/src/main/java/app/zerus/mobile/TaskSync.kt`.
- Test: `mobile/android/app/src/test/java/app/zerus/mobile/TaskActionsTest.kt` (new), against a MockWebServer whose computer answers `task_exchange` with `answerExchange`, and `task_send`, `task_cancel` and `task_done` with scripted receipts.

**Interfaces:**
- `TaskSync` gains:

  ```kotlin
  /** One exchange with this computer now, whatever the digests: the phone's edits reach it. */
  suspend fun exchangeNow(connection: Connection, key: ProjectKey, name: String, computerId: String, fetch: List<String> = emptyList())
  /** Send now: the exchange, then task_send to that computer, then the exchange that shows the result. */
  suspend fun send(connection: Connection, key: ProjectKey, name: String, computerId: String, task: String, session: String): TaskOutcome
  suspend fun cancel(connection: Connection, key: ProjectKey, name: String, computerId: String, task: String, reason: String): TaskOutcome
  /** One Done page from that computer. */
  suspend fun donePage(connection: Connection, key: ProjectKey, computerId: String, before: DoneCursor?, limit: Int = 50): DonePage
  ```

- `TaskOutcome` is `Done`, `Refused(reason)` or `Uncertain(reason)`. A failed first exchange is `Refused("The tasks could not reach <computer>: …")` and sends nothing.
- `DonePage(rows: List<DoneRow>, next: DoneCursor?, total: Int, recent: Int, damaged: Int)` and `DoneRow(uuid, id, text, session: SessionRef?, acceptedAt: Long)` are parsed from the answer.
- All of these hold the project's lock, as `sync` does.

- [ ] **Step 1: Write the failing tests**
  - `aSendFirstBringsThePhonesEdits`: the phone edits the prompt. The first relay request is `task_exchange` carrying the edit, then `task_send` with `{project, task, session}`, then `task_exchange`. The computer's store has the edited text before the send.
  - `aFailedExchangeSendsNothing`: the exchange receipt fails. No `task_send` is submitted, and the outcome is `Refused` with the reason.
  - `anUncertainSendSaysSo`: an `uncertain` receipt gives `Uncertain` with its error.
  - `aRefusedSendGivesHgsReason`: a `failed` receipt gives `Refused` with its error.
  - `aCancelCarriesItsReason`.
  - `donePagesParseAndContinue`: two pages, the cursor passed as `before`, and totals, recent and damaged parsed.
  - `openingADoneTaskFetchesIt`: `exchangeNow` with `fetch` brings a Done task into the phone's store.
  - `editsReachTheComputer`: after a local add, `exchangeNow` makes the computer hold it.
- [ ] **Step 2: Run them to verify they fail**
  - Run: `./gradlew --no-daemon :app:testDebugUnitTest --tests 'app.zerus.mobile.TaskActionsTest'`.
  - Expected: FAIL to compile.
- [ ] **Step 3: Implement**
- [ ] **Step 4: Run them to verify they pass**
- [ ] **Step 5: Commit** — "Carry the phone's Send now, Cancel, Done pages and fetches through the relay under the project's lock".

---

### Task 3: `TasksPresentation`

**Files:**
- Create: `mobile/android/app/src/main/java/app/zerus/mobile/TasksPresentation.kt`.
- Test: `mobile/android/app/src/test/java/app/zerus/mobile/TasksPresentationTest.kt`.

**Interfaces:**

```kotlin
enum class TaskTab(val label: String) { REVIEW("In review"), ACTIVE("In progress"), TODO("To do"), BACKLOG("Backlog"), DONE("Done") }
enum class TaskAction(val label: String) { EDIT("Edit"), TO_BACKLOG("Move to Backlog"), TO_TOP("Move to top"), SEND("Send now"),
    TO_TODO("Move to To do"), CANCEL("Cancel"), BACK_TO_TODO("Back to To do"), ACCEPT("Accept") }
data class ActionState(val action: TaskAction, val enabled: Boolean, val reason: String = "")
data class TaskCard(val uuid: String, val id: String, val text: String, val tab: TaskTab, val sessionLabel: String,
    val highlighted: Boolean, val note: String /* report or cancellation, In review only */, val actions: List<ActionState>)
data class ChosenSession(val name: String, val computerId: String, val machineId: String)
data class SendRoute(val computerId: String, val session: String)
object TasksPresentation {
    fun counts(tasks: List<TaskInfo>, chosen: ChosenSession?, onlyThisSession: Boolean): Map<TaskTab, Int>
    fun defaultTab(tasks: List<TaskInfo>): TaskTab
    fun cards(tasks: List<TaskInfo>, tab: TaskTab, chosen: ChosenSession?, onlyThisSession: Boolean,
              routes: (TaskInfo) -> RouteState): List<TaskCard>
    /** Where Send now goes, or why it cannot: the pinned session's computer, or the chosen session for Any session. */
    fun route(task: TaskInfo, chosen: ChosenSession?, machines: List<Machine>, connectionId: String,
              operations: (Machine) -> Set<String>, sessions: List<Session>): RouteState
    fun doneSplit(recentCount: Int, total: Int, shown: Int): String? // "Show older (N)" while older remain
}
```

`RouteState` is `Ready(SendRoute)` or `Blocked(reason)`. The reasons are:
- "<session> no longer exists — choose another session";
- "<computer> is offline";
- "<computer> needs an update to send tasks";
- "Choose a session for this task".

Cancel routes the same way, from the task's recorded `session`.

- [ ] **Step 1: Write the failing tests**
  - Tab order and the default tab.
  - The actions of each stage, in the spec's order.
  - Counts with and without Only this session.
  - Highlight:
    - pinned to the chosen session in To do and Backlog;
    - sent to it in the other stages.
  - Session labels: the session's name, or "any session".
  - The In review note: the report, or "Cancelled: <reason>".
  - Routes:
    - pinned on another computer → that computer;
    - Any session → the chosen session;
    - an offline computer, a computer without `task_send`, and a missing session → blocked with the reason.
  - `doneSplit`.
- [ ] **Step 2: Run them to verify they fail**
- [ ] **Step 3: Implement**
- [ ] **Step 4: Run them to verify they pass**
- [ ] **Step 5: Commit** — "Model what the phone's Tasks screen shows: tabs, counts, cards, actions and where Send now goes".

---

### Task 4: The screen

**Files:**
- Create: `mobile/android/app/src/main/java/app/zerus/mobile/TasksUi.kt`, holding:
  - `TasksScreen`;
  - `TaskCardView`;
  - `TaskSettingsSheet`;
  - `TaskSessionSheet` (Any session and the project's sessions; from the ⋯ chip, every project's sessions);
  - `CancelTaskDialog`;
  - `ProjectTaskSettingsSheet`;
  - `TasksColors`: sheets #1A2225, dialogs #1F292C, snackbars #2A3538, handles #354449.
- Modify:
  - `ZerusViewModel.kt`: the Tasks state and actions (open/close, project key and name, chosen session, listing, tab, Only this session, Done pages, snackbar message, busy), each `TaskStore` call on IO followed by `syncProject(key)` and a refreshed listing, and `tasksInReview(projectKey)` for the badge.
  - `SessionDetailsUi.kt`:
    - a Tasks `IconButton` after Terminal, with a `BadgedBox` showing the In review count, only for catalogued projects;
    - the Session details sheet's colours;
    - the "Tasks" tab renamed "Plan".
- Behaviour, from the spec's "Android interface":
  - Top bar: Back, the title "Tasks" with the project's name, the Only this session filter icon, and the gear.
  - Session chips with status dots: Working green, Needs input amber, Ready muted, from `SessionFilters`. The last ⋯ chip opens the session sheet.
  - `PrimaryScrollableTabRow` with counts.
  - Cards in the session card style:
    - the header holds the stage glyph, the ID, the session label and the action icons, always visible;
    - In progress cards show the session badge, and In review cards the note;
    - the chosen session's cards get an accent border;
    - a tap opens the task settings.
  - Task settings sheet:
    - the prompt, editable in To do and Backlog;
    - the Session row, which opens the session sheet;
    - the actions, the history and a comment field;
    - Save appears once something changed, and an action first saves.
    - Send now there is disabled with its reason. On the card, a tap on a disabled Send now shows the reason in the snackbar.
  - Input: `CompactMessageInput` with `leading = {}`. Its action is a round button that adds the task to To do, or to Backlog while Backlog is open. The placeholder names the stage. It is pinned to the chosen session.
  - Cancel dialog: the task text, what will happen, an optional Reason, and the buttons Keep running and Cancel task.
  - Done tab: the last 7 days, then "Show older (N)". A tap on a Done row fetches the task, then opens its settings.
  - Errors and outcomes appear in the screen's `SnackbarHost`.

- [ ] **Step 1: Wire it**

  Wire it as described. The glue needs Android, so the logic it calls is Tasks 2 and 3.

- [ ] **Step 2: Build and test**

  Run: the full check.

  Expected: BUILD SUCCESSFUL, with no new lint errors.

- [ ] **Step 3: Commit** — "Give the phone the Tasks screen: chips, stages, cards, the task settings, the input, Cancel and Done".

---

### Task 5: Docs and the whole check

- Modify: `mobile/android/README.md` (Project tasks), and the spec if anything was decided.
- Run:
  - the full Android check;
  - `tests.test_ci_security`;
  - `scripts/ci/check-source.py`.
- Commit — "Document the phone's Tasks screen".
