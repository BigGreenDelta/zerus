# Project tasks on iOS — design

Owner direction (2026-10-20): "the same as on Android, only on iOS". For now the
task input has no microphone. The behaviour is specified in:

- `2026-10-13-project-task-queue-design.md`, sections "Interface", "Android interface" and "Failure cases";
- `2026-10-18-task-replication-loro-design.md`, sections "Phone", "Done tasks" and "Actions with side effects".

This document maps that behaviour onto the native iOS client (`mobile/ios`) and
names what differs only because the platform does.

## Intent

A person using Zerus on an iPhone files, edits, sends, cancels and accepts a
project's tasks exactly as on the Android phone. The two phones can sit in the
same project and see the same tasks: both keep a replica of the project's
tasks, built from the same Rust crate and exchanged through the same relay
operations.

## Goals

- **The same Tasks screen as Android.** Entry, session chips, stages, cards and
  their actions, the task settings, the input, Cancel, the project's task
  settings and Done. Each part is listed under Behaviour below.
- **The same replica and exchange.**
  - The crate is `zerus-replica` through `zerus-replica-ffi`, with UniFFI Swift
    bindings instead of Kotlin.
  - The operations are `task_exchange`, `task_send`, `task_cancel` and
    `task_done` through the relay.
  - The timing and safety rules are Android's:
    - one replica per connection and swarm;
    - while the app is active, a check every 15 seconds and an exchange after
      each edit;
    - a one-minute wait after a failed exchange, and five minutes at most per
      exchange;
    - Send now exchanges first and nothing is sent if that fails;
    - an uncertain or lost answer holds Send now until an exchange with that
      computer completes;
    - Cancel falls back to another online computer of the project when the
      session's computer is out of reach;
    - Done with Only this session lists and counts only the tasks sent to that
      session.
- **Port names.** Kotlin names are kept in Swift (`TaskSync`, `TaskReplicas`,
  `TaskSyncPlan`, `TasksPresentation`, `TaskTab`, `TaskAction`, `RouteState`,
  `TaskOutcome`, `DonePage`), as the iOS foundation design asks, so the two
  clients can be compared line by line.

## Non-goals

- Any change to hgs, the relay or the connector: they already serve the phone.
- A microphone in the task input (owner decision, 2026-10-20).
- Notifications for tasks.
- Background sync.
- Dragging.
- Layouts specific to the iPad.
- The legacy Python connector.

## Anti-goals

- The iPhone and the Android phone disagree on what a task is, where Send now
  goes, or what a stage action does.
- A task reaches an agent from the input.
- A prompt is pasted twice because the phone retried an uncertain Send now.
- Blocking store calls run on the main actor.

## Behaviour on iOS

- **Entry.**
  - `SessionHeaderTools` gets a Tasks button after Terminal. Its badge counts the
    tasks In review.
  - The button shows only for catalogued projects, and not for child agents or
    archived sessions.
  - It opens the screen as a `fullScreenCover`, as Terminal does. Back returns
    to the conversation.
  - Tasks closes when that conversation leaves the screen.
- **Screen.**
  - A `NavigationStack` titled "Tasks", with the project's name under it.
  - The toolbar holds Back, the Only this session filter (funnel, mint when on)
    and the gear for the project's task settings.
- **Session chips.**
  - A horizontal row of capsules, one per live session of the project.
  - Each capsule has a status dot: Working mint, Needs input amber, Ready muted,
    from the same session filters as Android.
  - A last ⋯ capsule opens a sheet with the sessions of every catalogued
    project. Picking one of another project shows that project's tasks; the
    selected tab stays.
- **Stages.**
  - A horizontally scrolling tab strip in the order In review, In progress,
    To do, Backlog, Done, each tab with its count.
  - The selected tab is underlined in mint and scrolled into view.
  - The default tab is In review when it has tasks, otherwise To do.
- **Cards.**
  - Surface `#1A2225`, corner radius 14. The chosen session's tasks get a
    1.5 pt mint border.
  - The header line is 44 pt high and holds the stage symbol, the ID, the
    session label ("any session" when unpinned; mint when highlighted) and the
    stage's action icons, always visible.
  - The text below is limited to two lines.
  - In progress cards show the session's status chip. In review cards show the
    report, or "Cancelled: reason".
  - A tap opens the task settings. A tap on a disabled action shows its reason.
- **Symbols.**
  - Stages: In review `tray`, In progress `play.fill`, To do `text.badge.plus`,
    Backlog `clock`, Done `checkmark`.
  - Actions: Edit `pencil`, Move to Backlog `clock`, Move to top `chevron.up.2`,
    Send now `play.fill`, Move to To do and Back to To do `text.badge.plus`,
    Cancel `xmark`, Accept `checkmark`.
- **Task settings.** A sheet at the large detent with:
  - the prompt, a `TextEditor` editable in To do and Backlog;
  - Session, a sheet with Any session and the project's sessions;
  - the stage actions except Edit, with the reasons for disabled ones;
  - Save once something changed (an action applies unsaved changes first);
  - the history in the desktop's wording;
  - a comment field with Add.
- **Cancel.** A system alert with:
  - the task text, and what will happen: the session gets the cancel message,
    or, on the fallback route, no message;
  - a Reason text field;
  - Keep running (cancel role) and Cancel task (destructive).
- **Project task settings.** A sheet with:
  - the two templates and the placeholder hint;
  - the toggle for agents acting on other sessions;
  - the hourly limit, from 0 to 1000.
  - Save writes only the changed fields onto the replica's current settings.
- **Input.**
  - A compact field (a vertical `TextField` of up to six lines, on the surface
    colour, radius 20) with one round 40 pt button.
  - The button adds the task to To do, or to Backlog while Backlog is open,
    pinned to the chosen session. The placeholder names the stage.
  - When the chosen session's computer reports no native ID, the button shows
    an info glyph and a tap shows why.
  - No attach button, no microphone.
- **Done.**
  - The first page loads when Tasks opens and on every opening of the Done tab:
    pages of 50 from a computer of the project (the chosen session's first).
  - The last 7 days come first, then "Show older (N)".
  - A tap on a Done card fetches the task into the replica, then opens it.
- **Notices.**
  - The screen has its own bottom capsule, because the app's notice sits under
    the full-screen cover.
  - It uses `#2A3538` and stays 3 seconds. It shows refusals, uncertain
    outcomes and failures.
- **Colours.**
  - Sheets use `.presentationBackground(#1A2225)` with the drag indicator
    visible.
  - System alerts keep the system appearance, since iOS draws them (the
    builder's call).
- **Session details.** The "Tasks" segment of `SessionDetailsSheet` is renamed
  "Plan", and its empty text names the plan.

## Architecture

### The library

- **The build script.** `mobile/ios/scripts/build-replica.sh` builds
  `zerus-replica-ffi` as a static library with the pinned Rust (1.99.1, as CI):
  - It runs `cargo rustc --lib --release --crate-type staticlib` for
    `aarch64-apple-ios`, `aarch64-apple-ios-sim`, `x86_64-apple-ios`,
    `aarch64-apple-darwin` and `x86_64-apple-darwin`. The manifest's crate types
    do not change.
  - It joins the simulator pair and the macOS pair with `lipo`.
  - It makes `ZerusReplicaFFI.xcframework` with the UniFFI header and its
    module map (the configured module name gives the FFI module its name).
  - It generates the Swift bindings with the crate's own `uniffi-bindgen`, from
    a debug build for the host with `--features bindgen`, as the Android build
    does: the release profile strips the UniFFI metadata.
- **Outputs.**
  - Both outputs go to `mobile/ios/ZerusKit/Replica/`, which git ignores.
  - A stamp of the crate sources, the crate's own `Cargo.lock` and the
    toolchain skips a build that would produce the same files.
  - `crates/replica-ffi/uniffi.toml` gets `[bindings.swift]`, so the module
    is named `ZerusReplica`.
- **The package.** `ZerusKit/Package.swift` gains a `binaryTarget` for the
  XCFramework and a `ZerusReplica` target for the generated Swift. ZerusKit
  depends on it.
- **Who runs the script.** Xcode resolves packages before any build phase, so
  the script runs first:
  - `dev-install.sh`;
  - `remote-dev-install.sh`, which also copies `crates/replica` and
    `crates/replica-ffi` to the Mac in the repository's layout, and needs Rust
    with those targets there;
  - the TestFlight workflow, in a step of its own before any signing secret is
    in the environment, since cargo runs the dependencies' build scripts;
  - the CI `ios` job, after
    `rustup toolchain install 1.99.1 --profile minimal --target …`;
  - and once by hand before Xcode opens the project (README).
- **Size.** The app grows by the static library. It is measured on a Mac and
  reported in the plan.

### ZerusKit

- **Projects.** `ProjectCatalog` gains `taskDigests`, parsed from
  `mobile_projects.projects[].tasks_digest` (64 hex characters) for catalogued
  projects, as Android's `Projects.kt` does.
- **`TaskSync.swift`** ports `TaskSync.kt`:
  - `TaskReplicas` (folders and purge), `TaskPeer`, `TaskSyncPlan.peers`;
  - the `TaskSync` actor: a lock per project, the quiet triple, the backoff, the
    exchange deadline, held sends, `exchangeNow`, `send`, `cancel`, `donePage`,
    `settleSends`, `purge`;
  - `TaskOutcome`, `DoneCursor`, `DoneRow`, `DonePage`.
- **Store calls.** Every `TaskStore` call runs on a dedicated dispatch queue
  behind an async bridge, never on the main actor or the cooperative pool:
  calls may wait up to five seconds for the project's file lock.
- **Relay.** It uses the existing `RelayClient` (`post`, `waitForReceipt`) and
  its `RelayError`:
  - a rejected status before anything was queued is a refusal;
  - any other error after the submit is uncertain.
- **`TasksPresentation.swift`** ports `TasksPresentation.kt`: tabs, counts, the
  default tab, cards and their actions, highlight, Only this session, routes
  for Send now and Cancel, chips, the reasons and "Show older (N)".

### The app

- **`TasksModel`.** An `@MainActor @Observable` feature controller owned by
  `AppModel`, like the others. It holds the open project, the chosen session,
  the listing, the tab, the filter, the Done pages, busy state, notices, held
  sends and the In review counts. It ports the Tasks section of
  `ZerusViewModel.kt`.
- **The sync loop.**
  - It runs from `RootView` in a `.task(id: scenePhase)` loop while the scene is
    active: first after 6 seconds, then every 15.
  - It uses the latest catalog, or its own `/v1/computers` read when the
    catalog is older than 20 seconds.
  - Leaving `.active` stops the loop; moving to the background also cancels
    the exchanges that edits started, as Android does when the app stops.
    Removing a connection stops the running exchanges before it purges.
- **Purge.** `AppModel.disconnect` purges the connection's replicas once their
  running exchanges end.
- **Storage.**
  - The replicas live under `Application Support/task-replicas-v1/`, in
    `sha256(connection)/sha256(swarm)`.
  - The folder is excluded from backup, with file protection until first
    unlock.
  - There is no further encryption, as on Android (owner decision,
    2026-10-19): Loro writes its files itself.
  - `PrivacyInfo.xcprivacy` already declares file timestamps (C617.1), which
    the replica's inventory cache reads.
- **Views.** `Zerus/Tasks/` holds:
  - `TasksView` (screen, chips, tabs, cards, input, notice);
  - `TaskSettingsSheet`, `TaskSessionSheet`, `AllSessionsSheet`;
  - the cancel alert;
  - `ProjectTaskSettingsSheet`.

## Testing

- **ZerusKit XCTest** ports the Kotlin tests:
  - `TasksPresentationTests`;
  - `TaskSyncTests`;
  - `TaskActionsTests`, through `StubProtocol`, whose fake computer answers
    `task_exchange` from a real `TaskStore` with `answerExchange`;
  - the project parser's task digests.
- **Linux.** During development these run on Linux against the Linux build of
  the library: a Swift toolchain, ZerusKit without its Apple-only files, and a
  SHA-256 stand-in for CryptoKit. That bench is not part of the repository.
- **On a Mac and the device**, the owner checks:
  - `swift test` and the simulator build;
  - the views, which use SwiftUI.

## Left to the builder

- Spacing, fonts and the exact layout of sheets within the app's existing
  style.
- The stamp's format and the script's options.
- How the XCTest fakes are shaped.
