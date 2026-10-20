# Project Tasks on iOS Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The native iOS client gets the same project tasks as Android: the replica, the relay exchange, Send now, Cancel, Done and the Tasks screen.

**Architecture:**
- **The library.** `build-replica.sh` turns `zerus-replica-ffi` into an XCFramework plus UniFFI Swift bindings (`ZerusReplica`), which ZerusKit links as a binary target.
- **ZerusKit.** It ports Android's `TaskSync.kt` and `TasksPresentation.kt` under the same names, with XCTest ports of their tests.
- **The app.** `TasksModel` ports the Tasks section of `ZerusViewModel.kt`, and SwiftUI views port `TasksUi.kt`.

**Tech Stack:** Rust 1.99.1, UniFFI 0.32.2 (Swift), Swift 6 and SwiftUI on iOS 18, XCTest.

**Spec:**
- `docs/superpowers/specs/2026-10-20-task-tracker-ios-design.md`, which refers to `2026-10-13-project-task-queue-design.md` ("Interface", "Android interface", "Failure cases") and `2026-10-18-task-replication-loro-design.md` ("Phone", "Done tasks", "Actions with side effects").
- The Android code under `mobile/android/app/src/main/java/app/zerus/mobile/` (`TaskSync.kt`, `TasksPresentation.kt`, `TasksUi.kt`, the Tasks section of `ZerusViewModel.kt`) and its tests are the reference for every behaviour. Read them before porting.

## Global Constraints

- **Parity.** Behaviour, constants and names match Android:
  - sync every 15 s, the first after 6 s;
  - own catalog read when the catalog is older than 20 s;
  - retry after 60 s, exchange deadline 5 min;
  - exchange receipts polled 80 × 400 ms, actions 150 × 400 ms;
  - Done pages of 50;
  - notices shown 3 s.
- **Language and dependencies.** iOS 18, Swift 6 language mode. ZerusKit uses Foundation, CryptoKit, Security, UniformTypeIdentifiers and the new `ZerusReplica` only; no third-party Swift packages.
- **Store calls.** Every `TaskStore`/`TaskExchange` call runs through `TaskReplicas.run` (a dedicated `DispatchQueue`), never on the main actor or the cooperative pool. Every caught `CancellationError` is rethrown. Every store and exchange is released after use.
- **Storage.**
  - Replicas live in `Application Support/task-replicas-v1/<sha256(connection id)>/<sha256(swarm id)>` (lowercase hex).
  - The folder is excluded from backup and uses `.completeUntilFirstUserAuthentication`, without further encryption.
- **Rust build.**
  - Rust 1.99.1, targets `aarch64-apple-ios aarch64-apple-ios-sim x86_64-apple-ios aarch64-apple-darwin x86_64-apple-darwin`.
  - `cargo rustc --lib --release --crate-type staticlib`; the manifest's crate types stay as they are.
  - Outputs go to `mobile/ios/ZerusKit/Replica/` (git-ignored); the cargo target dir is `mobile/ios/ZerusKit/Replica/cargo`.
- **Fixtures** use invented names: project `orchard`, sessions `codex/api` and `claude/web`, computers `Studio` and `Laptop`.
- **Text.** UI text is English and hard-coded, as in the app. The task input has no microphone.
- **Commits.**
  - No co-author lines. Stage only this plan's files.
  - Never push, merge, rebase or switch branches.
  - Never build into `target/`, `tray/build` or `crates/replica-ffi/target`.
- **Linux bench (not committed).** No Mac is available yet. ZerusKit tests run on Linux with Swift 6.2.3 from `$CLAUDE_JOB_DIR/tmp/swift`, against the Linux build of `zerus-replica-ffi` (target dir `$CLAUDE_JOB_DIR/tmp/ffi-target`).
  - The bench script `$CLAUDE_JOB_DIR/tmp/kit-bench/run.sh [TestClass…]` compiles ZerusKit without `Attachments`, `ConversationCache`, `SpeechConfiguration`, `Keychain` and `SealedFileStore`. It adds a SHA-256 stand-in module named `CryptoKit`, the generated bindings, a Linux module map and `import FoundationNetworking`. It then builds the needed tests and runs them with XCTest.
  - "Bench: PASS" below means that script's run. App targets (SwiftUI) cannot build there. The owner builds them on a Mac.

## Review Focus

1. **Blocking calls stay off the main actor.** A store call that waits for the project's file lock must not freeze the UI. Pinned by `TaskReplicasTests.testRunLeavesTheCallersThread`.
2. **A possibly-sent task is not sent twice.** An uncertain receipt or a lost answer holds Send now until an exchange with that computer completes; a sync that exchanges nothing keeps the hold. Pinned by `TaskActionsTests.testAnUncertainSendHoldsUntilAnExchangeWithThatComputerEnds` and `testALostAnswerToTheSendIsUncertainAndHoldsTheTask`.
3. **Two relays never share documents, and removing a connection removes its replicas once running exchanges end.** Pinned by `TaskSyncTests.testReplicasAreSeparatePerConnectionAndSwarm` and `testPurgeWaitsForARunningExchange`.
4. **Only well-formed digests of catalogued projects drive sync.** An uncatalogued project or a malformed `tasks_digest` never triggers an exchange. Pinned by `ProjectsTests.testTaskDigestsComeFromCataloguedProjectsOnly`.
5. **The replica never leaves the phone in a backup.** Pinned by `TaskReplicasTests.testReplicaFoldersAreExcludedFromBackup` (Apple platforms only).

---

### Task 1: The replica library reaches ZerusKit

**Files:**
- Create: `mobile/ios/scripts/build-replica.sh`, `mobile/ios/ZerusKit/Tests/ZerusKitTests/ReplicaBindingsTests.swift`.
- Modify:
  - `crates/replica-ffi/uniffi.toml` — add `[bindings.swift]` with `module_name = "ZerusReplica"`;
  - `mobile/ios/ZerusKit/Package.swift`;
  - `.gitignore`, adding `mobile/ios/ZerusKit/Replica/`.

**Interfaces:**
- `build-replica.sh [--bindings-only]`:
  - With no option, it builds the five targets, lipos the simulator and macOS pairs, and writes `Replica/zerus_replica_ffiFFI.xcframework` (headers and `module.modulemap` for module `zerus_replica_ffiFFI`) and `Replica/Swift/zerus_replica_ffi.swift`.
  - `--bindings-only` writes only the Swift file and the header, from a host build. The Linux bench uses this.
  - It skips the build when `Replica/stamp` equals the SHA-256 of `rustc --version`, `Cargo.lock`, `crates/replica-ffi/uniffi.toml` and every file under `crates/replica/src` and `crates/replica-ffi/src`.
  - It fails with a message that names `rustup target add …` when a target is missing.
- `Package.swift`:
  - `.binaryTarget(name: "zerus_replica_ffiFFI", path: "Replica/zerus_replica_ffiFFI.xcframework")`;
  - `.target(name: "ZerusReplica", dependencies: ["zerus_replica_ffiFFI"], path: "Replica/Swift")`;
  - `ZerusKit` depends on `ZerusReplica`.
- Swift API produced: module `ZerusReplica` with `TaskStore(root:project:projectName:)`, `TaskInfo`, `TaskStage`, `TaskTarget`, `SessionRef`, `TaskPosition`, `TaskSettings`, `TaskExchange`, `ExchangeProgress`, `ReplicaError`.

- [ ] **Step 1: Write the failing test** `ReplicaBindingsTests.testAStoreFilesATaskAndAnswersAnExchange`.
  - In a temporary folder, `TaskStore(root:project: "project-1", projectName: "orchard")` adds "Add rate limiting" to `.todo`, `.anySession`, `.end`.
  - It asserts `list().tasks.count == 1`, an ID matching `^T-[a-z0-9]{6}$` and a 64-hex `liveDigest()`.
  - A second store's `exchange()` against the first's `answerExchange` ends with equal digests.
- [ ] **Step 2: Run it to see it fail.**
  - Bench: compile error, no module `ZerusReplica`.
- [ ] **Step 3: Write `build-replica.sh`, the package targets, `uniffi.toml` and `.gitignore`.** Then create the bench in `$CLAUDE_JOB_DIR/tmp/kit-bench/`; it calls `build-replica.sh --bindings-only`.
- [ ] **Step 4: Run it to see it pass.**
  - Run: `$CLAUDE_JOB_DIR/tmp/kit-bench/run.sh ReplicaBindingsTests` → PASS.
  - Run: `bash -n mobile/ios/scripts/build-replica.sh`, and `shellcheck` when it is installed → clean.
- [ ] **Step 5: Commit** — "Build the task replica for iOS as an XCFramework with Swift bindings that ZerusKit links".

### Task 2: Every iOS build makes the library first

**Files:**
- Modify:
  - `mobile/ios/scripts/dev-install.sh` and `testflight-upload.sh` — call `build-replica.sh` before `xcodebuild`;
  - `mobile/ios/scripts/remote-dev-install.sh` — also rsync `crates/replica`, `crates/replica-ffi`, `Cargo.toml`, `Cargo.lock` and `rust-toolchain*` if present, keeping the repo layout;
  - `.github/workflows/checks.yml` `ios` job, adding before the ZerusKit tests:

    ```yaml
    - run: rustup toolchain install 1.99.1 --profile minimal --target aarch64-apple-ios,aarch64-apple-ios-sim,x86_64-apple-ios,aarch64-apple-darwin,x86_64-apple-darwin
    - run: RUSTUP_TOOLCHAIN=1.99.1 bash mobile/ios/scripts/build-replica.sh
    ```

  - `.github/workflows/ios-testflight.yml` — the same two steps before the upload script;
  - `tests/test_ci_security.py` — only if it pins the job steps.

- [ ] **Step 1: Update the scripts and workflows.**
- [ ] **Step 2: Verify.**
  - Run: `python3 -m unittest tests.test_ci_security`, `python3 scripts/ci/check-source.py`, and `bash -n` on each changed script → all pass.
- [ ] **Step 3: Commit** — "Build the task replica before every iOS build, locally, remotely, in CI and for TestFlight".

### Task 3: Task digests in the project catalog

**Files:**
- Modify: `mobile/ios/ZerusKit/Sources/ZerusKit/Sessions.swift` (`ProjectCatalog`, `ProjectParser.parse`).
- Test: `mobile/ios/ZerusKit/Tests/ZerusKitTests/ProjectsTests.swift`.

**Interfaces:** `ProjectCatalog.taskDigests: [ProjectKey: [String: String]]`, mapping a project to its computer IDs and their digests, as Android's `ProjectCatalog.taskDigests`.

- [ ] **Step 1: Write the failing test** `testTaskDigestsComeFromCataloguedProjectsOnly`.
  - Two computers list `orchard` with digests `"a"×64` and `"b"×64`. The parse gives `taskDigests[key] == ["computer-1": "aaaa…", "computer-2": "bbbb…"]`.
  - A digest `"xyz"`, an upper-case one and a missing one are left out.
  - A folder-derived (uncatalogued) group has no entry.
- [ ] **Step 2: Bench: FAIL.**
- [ ] **Step 3: Implement.** Parse in the existing `projects` loop with `^[0-9a-f]{64}$`.
- [ ] **Step 4: Bench: PASS** (`ProjectsTests`).
- [ ] **Step 5: Commit** — "Read each computer's tasks digest into the iOS project catalog".

### Task 4: `TaskReplicas`, `TaskSyncPlan` and `TaskSync.sync`

**Files:**
- Create: `mobile/ios/ZerusKit/Sources/ZerusKit/TaskSync.swift`.
- Test: `mobile/ios/ZerusKit/Tests/ZerusKitTests/TaskSyncTests.swift`, `TaskReplicasTests.swift`, and a `FakeComputer` helper in `TaskFakes.swift`. The fake is a `StubProtocol` responder that answers `task_exchange` with a real `TaskStore.answerExchange`, records envelopes, and can fail exchanges or return raw replies.

**Interfaces** (`Sendable`, ported from `TaskSync.kt`):

```swift
public final class TaskReplicas: Sendable {
    public init(root: URL)
    public static func live() throws -> TaskReplicas   // Application Support/task-replicas-v1
    public func run<T: Sendable>(_ body: @escaping @Sendable () throws -> T) async throws -> T
    public func store(_ key: ProjectKey, name: String) async throws -> TaskStore
    public func purge(connectionId: String) async throws
}
public struct TaskPeer: Hashable, Sendable { public let computerId: String; public let digest: String }
public enum TaskSyncPlan {
    public static let operation = "task_exchange"
    public static func peers(_ digests: [ProjectKey: [String: String]], relayOperations: [String: Set<String>],
                             machineOperations: [MachineKey: Set<String>], machines: [Machine]) -> [ProjectKey: [TaskPeer]]
}
public actor TaskSync {
    public init(client: RelayClient, replicas: TaskReplicas, exchangeTimeout: Duration = .seconds(300),
                clock: @escaping @Sendable () -> Date = Date.init)
    /// The failures by computer ID.
    public func sync(_ connection: Connection, _ key: ProjectKey, name: String, peers: [TaskPeer]) async -> [String: String]
    public func purge(connectionId: String) async
}
```

- [ ] **Step 1: Write the failing tests.** Port every test in Android `TaskSyncTest.kt`, keeping their names in XCTest style, plus:
  - `TaskReplicasTests.testRunLeavesTheCallersThread` — from `@MainActor`, the body sees `!Thread.isMainThread`;
  - `testReplicaFoldersAreExcludedFromBackup` — under `#if !os(Linux)`;
  - `TaskSyncTests.testReplicasAreSeparatePerConnectionAndSwarm`;
  - `testPurgeWaitsForARunningExchange` — the fake computer blocks one answer; `purge` returns only after it and the folder is gone.
- [ ] **Step 2: Bench: FAIL** (missing types).
- [ ] **Step 3: Implement** per the Interfaces and `TaskSync.kt`:
  - a lock per `ProjectKey` (an async mutex inside the actor);
  - `waiting` backoff of 60 s, and the `quiet` triple;
  - one whole exchange loop: `nextRequest` → `post` → `waitForReceipt(maxPolls: 80, pollDelay: .milliseconds(400))` → `accept`;
  - the deadline.
- [ ] **Step 4: Bench: PASS** (`TaskSyncTests TaskReplicasTests`).
- [ ] **Step 5: Commit** — "Keep and exchange the iPhone's task replicas per connection and swarm".

### Task 5: Send now, Cancel, Done pages, fetches and held sends

**Files:**
- Modify: `TaskSync.swift`.
- Test: `mobile/ios/ZerusKit/Tests/ZerusKitTests/TaskActionsTests.swift`.

**Interfaces** (ported from `TaskSync.kt`):

```swift
public enum TaskOutcome: Equatable, Sendable { case done, refused(String), uncertain(String) }
public struct DoneCursor: Equatable, Sendable { public let at: Int64; public let uuid: String }
public struct DoneRow: Equatable, Sendable { public let uuid, id, text: String; public let session: SessionRef?; public let acceptedAt: Int64 }
public struct DonePage: Equatable, Sendable { public let rows: [DoneRow]; public let next: DoneCursor?; public let total, recent, damaged: Int }
extension TaskSync {
    public func exchangeNow(_ connection: Connection, _ key: ProjectKey, name: String, computerId: String, fetch: [String] = []) async throws
    public func send(_ connection: Connection, _ key: ProjectKey, name: String, computerId: String, task: String,
                     session: String, computerName: String? = nil) async -> TaskOutcome
    public func cancel(_ connection: Connection, _ key: ProjectKey, name: String, computerId: String, task: String,
                       reason: String, computerName: String? = nil) async -> TaskOutcome
    public func donePage(_ connection: Connection, _ key: ProjectKey, computerId: String, before: DoneCursor?,
                         limit: Int = 50, session: SessionRef? = nil) async throws -> DonePage
    public func held(_ task: String) -> Bool
    public func heldTasks() -> Set<String>
    public func settleSends(_ connection: Connection, names: @Sendable (ProjectKey) -> String) async
}
```

- [ ] **Step 1: Write the failing tests.** Port every test in Android `TaskActionsTest.kt` (12), with the same payload and outcome assertions:
  - `RelayError` with status 400 → `.refused(message)`;
  - a dropped connection after the request → `.uncertain` and held;
  - the failed first exchange reason starts with "The tasks could not reach Studio";
  - the fetch request's inventory names the fetched UUID.
- [ ] **Step 2: Bench: FAIL.**
- [ ] **Step 3: Implement.**
  - `act` exchanges, then posts with `maxPolls: 150`, then exchanges again.
  - A rejected `RelayError` (`status` in 400…499 except 408) is a refusal; any other error after the submit is uncertain.
  - `send` holds the task on `.uncertain`.
  - `settleSends` runs `exchangeNow` per held (project, computer) of the connection and releases on success.
- [ ] **Step 4: Bench: PASS** (`TaskActionsTests TaskSyncTests`).
- [ ] **Step 5: Commit** — "Carry the iPhone's Send now, Cancel, Done pages and fetches through the relay, holding a possibly sent task".

### Task 6: `TasksPresentation`

**Files:**
- Create: `mobile/ios/ZerusKit/Sources/ZerusKit/TasksPresentation.swift`.
- Test: `mobile/ios/ZerusKit/Tests/ZerusKitTests/TasksPresentationTests.swift`.

**Interfaces:** port `TasksPresentation.kt` one-to-one:
- `TaskTab` (with `label`), `TaskAction` (with `label`), `ActionState`, `TaskCard`, `ChosenSession`, `ChipDot`, `SessionChip`;
- `SendRoute(computerId, session, direct = true)` and `RouteState { ready(SendRoute), blocked(String) }`;
- `enum TasksPresentation` with `tab`, `actions`, `highlighted`, `visible`, `counts`, `defaultTab`, `sessionLabel`, `note`, `cards`, `addReason`, `route`, `cancelRoute`, `chips`, `olderLabel`.
- Same reasons, word for word.

- [ ] **Step 1: Write the failing tests:** every test in Android `TasksPresentationTest.kt` (10), with the same values.
- [ ] **Step 2: Bench: FAIL.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Bench: PASS** (`TasksPresentationTests`), then the whole bench run → PASS.
- [ ] **Step 5: Commit** — "Model what the iPhone's Tasks screen shows, as on Android".

### Task 7: `TasksModel` and the app's sync

**Files:**
- Create: `mobile/ios/Zerus/Tasks/TasksModel.swift`.
- Modify:
  - `mobile/ios/Zerus/App/AppModel.swift`: own `tasks: TasksModel`; keep `taskDigests` from `loadCatalog`; add `tasks.purge(connection)` in `disconnect`; stop task sync on `.background` in `scenePhaseChanged`;
  - `mobile/ios/Zerus/App/RootView.swift`: a `.task(id: scenePhase)` loop that, while `.active`, waits 6 s and then calls `model.tasks.syncTasks()` every 15 s.

**Interfaces:** `@MainActor @Observable final class TasksModel` ports the Tasks section of `ZerusViewModel.kt`, with the same member names in Swift style:
- state: `tasksKey`, `tasksChosen`, `taskListing`, `taskTab`, `tasksOnlyThisSession`, `taskNotice`, `taskBusy`, `doneRows`, `doneTotal`, `doneRecent`, `doneNext`, `doneExpanded`, `doneLoading`, `doneLoaded`, `doneError`, `reviewCounts`;
- methods: `syncTasks()`, `stopTaskSync()`, `openTasks(_ session: Session)`, `closeTasks()`, `chooseTaskSession(_:_:)`, `setOnlyThisSession(_:)`, `selectTaskTab(_:)`, `refreshTasks(selectDefault:)`, `refreshReviewCount(_:)`, `addTask(_:)`, `saveTask(_:text:target:)`, `commentTask(_:_:)`, `saveTaskSettings(original:edited:)`, `taskSettings()`, `saveThen(_:text:target:action:)`, `taskAction(_:_:)`, `cancelTask(_:reason:)`, `loadDone(more:keepExpanded:)`, `openDoneTask(_:opened:)`, `taskProject(_:)`, `taskChips(_:)`, `projectSessions(_:)`, `sendRoute(_:)`, `cancelRoute(_:)`, `taskProjectName(_:)`, `purge(_ connection:)`.
- It reads connections, machines, sessions, projects, operations and digests from its `AppModel` (weak).

- [ ] **Step 1: Write `TasksModel` and the `AppModel`/`RootView` wiring,** following `ZerusViewModel.kt` line by line, including the held-send mirror, `doneRequest` generation, `launchProjectSync` tracking and the computer-name refusals.
- [ ] **Step 2: Bench: PASS** — ZerusKit is unchanged by this task, so the run guards against regressions. Owner check on a Mac: `xcodebuild … build` succeeds.
- [ ] **Step 3: Commit** — "Give the iPhone app its task replicas: sync while active, edits, actions and Done pages".

### Task 8: The Tasks screen

**Files:**
- Create in `mobile/ios/Zerus/Tasks/`:
  - `TasksView.swift` — screen, chips, tab strip, cards, input, notice capsule;
  - `TaskSettingsSheet.swift` — task settings, session picker, all-sessions sheet;
  - `ProjectTaskSettingsSheet.swift`.
- Modify:
  - `mobile/ios/Zerus/Conversation/SessionHeader.swift` — the Tasks button after Terminal, with a badge of `reviewCounts`, shown only per the design;
  - `mobile/ios/Zerus/Conversation/ConversationView.swift` — `.fullScreenCover` for Tasks; close it when the conversation's target changes or leaves;
  - `mobile/ios/Zerus/Conversation/SessionSheets.swift` — "Tasks" becomes "Plan", with the empty text "No plan reported yet. It appears when the agent updates its plan."

- [ ] **Step 1: Write the views** per the design's "Behaviour on iOS": its symbols, colours (`#1A2225` sheets, `#2A3538` notice, mint accent), the cancel alert's two wordings, Save only when changed, and the input's info glyph.
- [ ] **Step 2: Bench: PASS.** `python3 scripts/ci/check-source.py` → pass. Owner check on a Mac and an iPhone: the simulator build and the screen.
- [ ] **Step 3: Commit** — "Give the iPhone the Tasks screen: chips, stages, cards, the task settings, the input, Cancel and Done".

### Task 9: Docs

- Modify:
  - `mobile/ios/README.md`: a "Project tasks" section, plus `build-replica.sh` and the Rust targets under Build;
  - `docs/superpowers/specs/2026-10-20-task-tracker-ios-design.md`, for anything decided while building.
- Run: `python3 scripts/ci/check-source.py`, `python3 -m unittest tests.test_ci_security` and the whole bench → pass.
- Commit — "Document the iPhone's project tasks and how its replica library is built".
