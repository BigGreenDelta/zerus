# Desktop Keyboard Shortcuts Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Familiar, rebindable keyboard commands for the desktop workspace that also work from Terminal without taking keys agents need.

**Architecture:** A command registry (`Shortcuts`/`ShortcutRegistry`) owns ids, platform defaults and per-device overrides. Each command is a `QAction`; a `qApp` event filter (`ShortcutRouter`) lets bound chords win over Terminal and text fields under explicit rules. Window commands live in a separate `SessionsWindowShortcuts.cpp`; Go to, the cheat sheet and Settings → Keyboard are standalone widgets fed by the registry.

**Tech Stack:** C++17, Qt 6.10+ Widgets and QtTest, CMake/CTest.

**Spec:** `docs/superpowers/specs/2026-10-15-keyboard-shortcuts-design.md` (read it first; it is authoritative for behavior).

## Global Constraints

- C++17 (`CMAKE_CXX_STANDARD 17`); no `using enum`, no C++20 library features.
- Qt ≥ 6.10 on every GUI CI platform (Arch Qt 6.12, macOS Homebrew, Windows 6.10.3).
- Repository text is English. Code style matches the surrounding file (dense one-line members in headers like `SettingsPage.h`, `tr()` for every visible string).
- GUI tests are offscreen; build with `cmake --build .ci-build/gui --target <test> -j8`, run single binaries with `QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/<test>` and the suite with `ctest --test-dir .ci-build/gui --parallel 8`. Reconfigure with `cmake .ci-build/gui` after adding files.
- `python3 scripts/ci/check-source.py` must pass (privacy and source checks).
- Never restart or kill agents, tmux servers or installed GUIs; tests use fakes only.
- Commit messages end with the Co-Authored-By trailer the coordinator gives you. Do not push; the coordinator merges.
- Command ids, default chords and rules come from `tray/src/Shortcuts.cpp`; never hard-code a chord elsewhere. Use `ShortcutRegistry::instance()` in product code and a local `ShortcutRegistry(platform)` in tests.
- Keep command actions enabled; a command with nothing to do shows a notice. A disabled action would hand its chord back to a text field.

## Review Focus

1. Russian keyboard layout: Ctrl+К, Ctrl+Ё and Ctrl+Ч Ctrl+Ы must act as Ctrl+K, Ctrl+` and Ctrl+X Ctrl+S (Qt maps Latin fallbacks; manual check on Linux, Windows, macOS in Task 6).
2. A chord pressed while a modal dialog (New session, confirmations) is open must not act on the main window (`Qt::WindowShortcut` plus the router's window check; Task 2 test opens a dialog and presses Ctrl+2).
3. Session switching while a message is being typed keeps the draft of each session (Task 2 test types into the composer, presses Alt+↓, then Alt+↑ and checks the text).
4. Push after the session changes or the agent finishes without queueing must never send Send now to another session or queue (Task 3 tests: session change, run change, queue without the message, timeout).
5. Rebinding to a chord already used, or clearing all chords of a command, must keep the cheat sheet, tooltips and Terminal routing consistent without a restart (Task 4 test rebinds and checks `hint()`; Task 2 test rebinds `tab.next` and presses the new chord in Terminal).

---

### Task 1: Registry and router (done, `Shortcuts.h/.cpp`, `test_shortcuts.cpp`)

Implemented by the coordinator before the other tasks start.

**Files:**
- Create: `tray/src/Shortcuts.h`, `tray/src/Shortcuts.cpp` (in `hgs-tray-ui`)
- Create: `tray/tests/test_shortcuts.cpp`; register `shortcuts` in `tray/tests/CMakeLists.txt`

**Produces (exact API, use these names):**

```cpp
namespace Shortcuts {
enum class Platform { Linux, Windows, MacOS };
enum class Scope { Window, Composer };
struct Command { const char *id; const char *group; const char *title; Scope scope; };
Platform currentPlatform();
const QList<Command> &commands();                 // display order
const Command *command(const QString &id);
QString title(const Command &command);            // translated
QString group(const Command &command);            // translated
QList<QKeySequence> defaults(const QString &id, Platform platform = currentPlatform());
QKeyCombination combination(const QKeyEvent *event);
QKeyCombination normalized(QKeyCombination key);
bool assignable(const QKeySequence &sequence, Platform platform, QString *reason = nullptr);
bool terminalSafe(QKeyCombination key, Platform platform);
bool protectedInTextField(QKeyCombination key, Platform platform);
bool selectionKey(QKeyCombination key);
QString text(const QKeySequence &sequence);       // NativeText
}
class ShortcutRegistry : public QObject {         // signal: changed()
    explicit ShortcutRegistry(Shortcuts::Platform = currentPlatform(), QObject *parent = nullptr);
    static ShortcutRegistry &instance();
    Shortcuts::Platform platform() const;
    QList<QKeySequence> keys(const QString &id) const;
    bool customized(const QString &id) const;
    void setKeys(const QString &id, const QList<QKeySequence> &keys);
    void reset(const QString &id);
    void resetAll();
    QString hint(const QString &id) const;                          // first chord, native text
    QString tooltip(const QString &caption, const QString &id) const; // "Caption (Ctrl+K)"
    QString conflict(const QString &id, const QKeySequence &sequence) const;
};
class ShortcutRouter : public QObject {           // install with qApp->installEventFilter
    explicit ShortcutRouter(QWidget *window, Shortcuts::Platform = currentPlatform(), QObject *parent = nullptr);
};
```

Command ids: `go.overview go.sessions go.projects go.machines go.accounts go.settings go.anywhere help.shortcuts session.next session.previous session.nextAttention session.previousAttention session.new session.rename session.search view.refresh tab.next tab.previous tab.terminal panel.toggle focus.next focus.previous message.send queue.sendNow message.push zoom.in zoom.out zoom.reset`.

---

### Task 2: Window commands in `SessionsWindow`

**Files:**
- Create: `tray/src/SessionsWindowShortcuts.cpp` (add to `hgs-tray-views` in `tray/CMakeLists.txt`)
- Modify: `tray/src/SessionsWindow.h` (private members below), `tray/src/SessionsWindow.cpp` (call `setupShortcuts()` at the end of the constructor; remove the `searchShortcut`, `refreshShortcut` and `refreshR` `QShortcut`s at lines ~1342-1345, keep the Escape `QShortcut`; replace the body of the `queueSendNowRequested` lambda with a call to `sendQueuedNow(id)`)
- Modify: `tray/src/ActivityView.h/.cpp`: `void setQueueSendNowShortcut(const QString &chord);` appends the chord to the Send now button tooltip (`queue["hint"]` + `" (" + chord + ")"`)
- Modify: `tray/src/SettingsPage.h`: `void syncContentScale();` re-reads `ContentScale::factor()` into the slider without emitting `contentScaleChanged`
- Test: `tray/tests/test_sessionswindow.cpp`

**Interfaces:**
- Consumes: Task 1 API.
- Produces (private members of `SessionsWindow`, later tasks call them):

```cpp
void setupShortcuts();                       // creates one QAction per Window command on this window,
                                             // installs ShortcutRouter on qApp, connects registry changed()
void applyShortcuts();                       // action->setShortcuts(registry.keys(id)); refreshes tooltips
void runShortcut(const QString &id);         // the command body; Go to's command items call it
QAction *shortcutAction(const QString &id) const;
bool sendQueuedNow(const QString &queueId = {}); // empty id = current snapshot; false + notice when nothing can be sent
void openGoTo();                             // Task 6 replaces the body; Task 2: showNotice(tr("Go to is not available yet."))
void openShortcutHelp();                     // Task 6 replaces the body; Task 2: show Settings
QHash<QString, QAction *> m_shortcutActions;
ShortcutRouter *m_shortcutRouter = nullptr;
```

Behavior (spec "Command behavior"):

- Sections: reuse the rail handlers: `m_brand->click()` for Overview, `m_sessionsNav` logic *without* the second-click filter reset (call `showSessionList()`), `showProjects(selectedHostTarget())`, `m_machinesNav->click()`, `m_accountsNav->click()`, `showWorkspaceSettings()`.
- `session.next/previous`: operate on the visible list widget (`m_listStack->currentIndex()==1` → search results list, else `m_sessions`). Starting at `currentRow()` step ±1 until a row that is not hidden, not a header (`SessionRoles::Header` false) and has no `SessionRoles::ChildId`. No wrap. If not on page 0, call `showSessionList()` first. Use `setCurrentRow` + `scrollToItem` so ordinary selection (`selectSession`) runs.
- `session.nextAttention/previousAttention`: iterate rows in list order starting after/before the current row, wrapping once; candidates are session rows whose `Entry` satisfies `entryNeedsAttention(entry)`; include rows inside collapsed groups by calling `revealSession(key)` before selecting. None → `showNotice(tr("No sessions need attention"))`.
- `session.new`: same call as the rail **+** (`showNewSession({}, {}, selectedHostTarget())`).
- `session.rename`: `renameSession()` when a session is selected.
- `session.search`/`view.refresh`: today's lambdas.
- `tab.next/previous`: over `m_detailTabs` indices with `isTabVisible(i) && isTabEnabled(i)`, wrapping; no selection → nothing. Set focus into the new tab's widget.
- `tab.terminal`: if current widget is not `m_terminal` → `setCurrentWidget(m_terminal); m_terminal->connectSession(); m_terminal->setFocus()`; else `setCurrentIndex(0)` and focus the composer when `m_composer` is visible and enabled.
- `panel.toggle`: `m_inspectorToggle->toggle()`.
- `focus.next/previous`: only on page 0; ordered areas: visible session list, current tab widget, `m_composer` (if visible+enabled), `m_inspector` (if visible); move to the next area that does not contain the focus widget, wrapping; `setFocus(Qt::TabFocusReason)` on the area's `focusProxy()` or first focusable child.
- `queue.sendNow`: `sendQueuedNow()`.
- `zoom.in/out/reset`: `QSettings().setValue("workspace/contentScale", ContentScale::clamp(old ± 0.1))` (reset → `ContentScale::Default`), `applyContentScale()`, `m_settingsPage->syncContentScale()`, `showNotice(tr("Content scale %1%").arg(qRound(scale*100)))`.
- Tooltips via `ShortcutRegistry::instance().tooltip(caption, id)`: rail buttons (Overview/brand, Sessions, Projects, Machines, Accounts, Settings), **+** (`session.new`), `m_inspectorToggle` (`panel.toggle`), Activity's Send now (`m_activityView->setQueueSendNowShortcut(hint("queue.sendNow"))`). Re-apply in `applyShortcuts()`.
- `message.send`, `message.push`: Composer scope, not created here (Task 3).

- [ ] **Step 1: Write failing tests** in `test_sessionswindow.cpp` (follow the file's existing fixture helpers for building a window with fake sessions and `QTest::qWaitForWindowActive`). Cover, each as its own test function:
  - `shortcutsSwitchSections`: Ctrl+2/3/4/5/1 and Ctrl+, set `m_pages` to 0/1/2/4/3/5 and the matching rail checked state (query via `findChild<QPushButton*>` object names already used in the file).
  - `shortcutsMoveBetweenSessions`: with a project header, two sessions and a subagent row, Alt+Down moves from session 1 to session 2 skipping header/subagent; at the end Alt+Down stays; Ctrl+PgUp moves back; draft typed into the composer for session 1 is still there after returning.
  - `shortcutsFindSessionsNeedingAttention`: three sessions, the 1st and 3rd need attention; from the 3rd, Alt+Shift+Down wraps to the 1st; with none, the notice text appears.
  - `shortcutsCycleSessionTabs`: Ctrl+Tab from Activity selects Terminal, then Processes only if enabled, skipping the hidden Native UI, then wraps; Ctrl+Shift+Tab goes back.
  - `terminalChordReturnsToActivity`: focus Terminal's screen, Ctrl+` shows Activity and focuses the composer; Ctrl+` again shows Terminal; Ctrl+C and Escape pressed in Terminal do not change the tab.
  - `shortcutsInModalDialogsStayLocal`: open a `QDialog` child modal over the window, press Ctrl+2 in it, page unchanged.
  - `queueChordUsesTheExistingSendNowPath`: with an `input_queue` snapshot `{id:"q1", can_send_now:true}` Ctrl+Shift+Enter issues one `requestQueueSendNow` with `"q1"` (use the fake CLI log the file already uses for queue tests); with `can_send_now:false` it shows "Nothing queued can be sent now".
  - `zoomChordsChangeContentScale`: Ctrl+= then Ctrl+0 change `workspace/contentScale` to 1.1 then 1.0.
  - `reboundChordsApplyAtOnce`: `ShortcutRegistry::instance().setKeys("tab.next", {QKeySequence("Ctrl+9")})`; Ctrl+9 in Terminal switches tab; Ctrl+Tab no longer does; tooltip of the rail Projects button contains the Ctrl+3 native text. Reset the registry in `cleanup()`.
- [ ] **Step 2: Run** `cmake --build .ci-build/gui --target test_sessionswindow -j8 && QT_QPA_PLATFORM=offscreen .ci-build/gui/tests/test_sessionswindow <testname>` for each; expect FAIL.
- [ ] **Step 3: Implement** `SessionsWindowShortcuts.cpp` and the header/ActivityView/SettingsPage hooks above. Skeleton:

```cpp
#include "SessionsWindow.h"
#include "Shortcuts.h"
// ... includes used below

void SessionsWindow::setupShortcuts()
{
    for (const auto &command : Shortcuts::commands()) {
        if (command.scope != Shortcuts::Scope::Window) continue;
        const QString id = QLatin1String(command.id);
        auto *action = new QAction(Shortcuts::title(command), this);
        action->setObjectName(QStringLiteral("shortcut:") + id);
        action->setShortcutContext(Qt::WindowShortcut);
        connect(action, &QAction::triggered, this, [this, id] { runShortcut(id); });
        addAction(action); m_shortcutActions.insert(id, action);
    }
    m_shortcutRouter = new ShortcutRouter(this, Shortcuts::currentPlatform(), this);
    qApp->installEventFilter(m_shortcutRouter);
    connect(&ShortcutRegistry::instance(), &ShortcutRegistry::changed, this, &SessionsWindow::applyShortcuts);
    applyShortcuts();
}
```

  Remove the router in the destructor path if `SessionsWindow` can be destroyed while `qApp` lives (`qApp->removeEventFilter(m_shortcutRouter)` in `~SessionsWindow` or rely on QObject parenting: an event filter that is deleted is removed automatically by Qt; parenting to `this` is enough).
- [ ] **Step 4: Run** the new tests and the whole `sessionswindow` suite (`ctest --test-dir .ci-build/gui -R sessionswindow --parallel 8`); expect PASS.
- [ ] **Step 5: Commit** `git add -A tray && git commit -m "Add keyboard commands for the workspace window"`.

---

### Task 3: Composer commands and push (`message.send`, `message.push`)

Starts after Task 2 is merged (uses `sendQueuedNow`).

**Files:**
- Modify: `tray/src/MessageComposer.h/.cpp`
- Modify: `tray/src/SessionsWindow.h`, `tray/src/SessionsWindowShortcuts.cpp` (push state), `tray/src/SessionsWindow.cpp` (connect signals; call `pushInspected(...)` from `acceptInspection`, `pushSendFinished(...)` from `finishMessage`)
- Test: `tray/tests/test_messagecomposer.cpp`, `tray/tests/test_sessionswindow.cpp`

**Interfaces:**
- Consumes: Task 1 API; Task 2 `sendQueuedNow(const QString &queueId)`.
- Produces:

```cpp
// MessageComposer
void setPushEnabled(bool enabled);           // main composer true, subagent composer false (default false)
signals:
void pushRequested(const QString &sessionKey);      // emitted immediately before sendRequested for a push
void sendNowRequested(const QString &sessionKey);   // push with an empty field and no attachments
// SessionsWindow (private)
struct PendingPush { QString key, run, conversation, text; quint64 request = 0; bool confirmed = false; QDeadlineTimer deadline; };
std::optional<PendingPush> m_push;
void armPush(const QString &key);
void pushSendFinished(quint64 request, bool ok, bool uncertain);
void pushInspected(const QString &key, const QJsonObject &details);
```

Behavior (spec `message.push`):
- `MessageComposer` adds two `QAction`s to its editor (`Qt::WidgetWithChildrenShortcut`), ids `message.send` (calls the same submit path as Enter) and, when push is enabled, `message.push`; shortcuts from `ShortcutRegistry::instance().keys(id)`, refreshed on `changed()`. Push with empty text and no attachments → `emit sendNowRequested(key)`. Otherwise set an internal flag so the next `send()` emits `pushRequested(key)` right before `sendRequested`; clear the flag on any failure before emission. While `m_composing` (IME) both actions do nothing.
- `SessionsWindow`: `sendNowRequested` → `sendQueuedNow()`. `pushRequested(key)` → `armPush(key)` recording key, `m_details["run_id"]`, `m_details["conversation_id"]`, the message text, deadline 20 s; the request id is captured in `sendMessage` when it calls `requestSendMessage` for an armed key without a request. `pushSendFinished`: failure or uncertain → drop with `showNotice(tr("The message was not pushed: sending did not complete"), true)`; success → `confirmed = true`. `pushInspected(key, details)` (call from `acceptInspection` after `m_details` is updated): ignore other keys; drop with notice if run or conversation differ; if confirmed and `input_queue.can_send_now` and the whitespace-normalized queue text contains the first 24 non-space characters of the message (whitespace-normalized) → drop the push, `sendQueuedNow(queue.id)`; if the deadline passed → drop with `showNotice(tr("The message was sent without push: it did not appear in the agent's queue"))`. The push belongs to its session key: inspections of other sessions are ignored, and a 1 s `QTimer` checks the deadline. Selecting another session stops the inspections the push needs, so it ends at its deadline with that notice.

- [ ] **Step 1: Failing tests** in `test_messagecomposer.cpp`: `ctrlEnterSendsLikeEnter`; `pushWithTextEmitsPushThenSend` (QSignalSpy order: `pushRequested` then `sendRequested`); `pushWithEmptyFieldRequestsSendNow`; `pushIsOffForSubagentComposer`; `ctrlXWithSelectionStillCuts`. In `test_sessionswindow.cpp`: `pushSendsNowWhenTheMessageIsQueued` (fake send confirmed, next inspection queue text contains the message, expect one queue-send-now request with that id); `pushIgnoresQueuesWithoutTheMessage`; `pushDropsAfterRunChange`; `pushTimesOut` (shorten via a test hook `setPushTimeoutForTests(int ms)`); `pushDropsWhenSendFails`.
- [ ] **Step 2: Run** them; expect FAIL.
- [ ] **Step 3: Implement** as above.
- [ ] **Step 4: Run** `messagecomposer` and `sessionswindow` suites; expect PASS.
- [ ] **Step 5: Commit** `"Add Send and push now for the message field"`.

---

### Task 4: Settings → Keyboard and the cheat sheet

Independent of Tasks 2, 3 and 5.

**Files:**
- Create: `tray/src/KeyboardSettings.h/.cpp`, `tray/src/ShortcutsDialog.h/.cpp` (add both `.cpp` to `hgs-tray-ui`)
- Modify: `tray/src/SettingsPage.h`: insert a `tr("Keyboard")` nav row and page immediately before About (after Voice input) so rows 0-6 keep their indices; add `void openKeyboard();`
- Test: create `tray/tests/test_keyboardsettings.cpp` (register like `test_shortcuts`)

**Interfaces:**
- Consumes: Task 1 API.
- Produces:

```cpp
class KeyboardSettings : public QWidget {    // objectName "keyboardSettings"
    explicit KeyboardSettings(ShortcutRegistry &registry, QWidget *parent = nullptr);
    // child objectNames: "keyboardCommands" (QTreeWidget: group items, command items with
    // columns Command | Shortcut | Alternate; Qt::UserRole on column 0 = command id),
    // "keyboardResetCommand", "keyboardResetAll", "keyboardTerminalNote" (QLabel)
};
class ShortcutsDialog : public QDialog {     // objectName "shortcutsDialog", non-modal
    explicit ShortcutsDialog(ShortcutRegistry &registry, QWidget *parent = nullptr);
signals:
    void customizeRequested();
    // children: "shortcutsFilter" (QLineEdit), "shortcutsList" (QTreeWidget), "shortcutsCustomize" (QPushButton)
};
```

Behavior (spec "Rebinding" and "Cheat sheet"):
- `KeyboardSettings`: tree grouped by `Shortcuts::group`, titles by `Shortcuts::title`, chords by `Shortcuts::text`. A marker "Works in Terminal" (tooltip, and a ✓ suffix in the chord cell) when the chord's first key is `terminalSafe` for the registry's platform. Double-click or Enter on a chord cell opens an edit dialog with a `QKeySequenceEdit` (`setMaximumSequenceLength(2)`), **Clear** and **Cancel/OK**. On OK: if `!Shortcuts::assignable(seq, platform, &reason)` show `reason` inline and keep the dialog open; if `registry.conflict(id, seq)` names another command, `QMessageBox::question(tr("Already used by %1. Reassign?"))`; on yes remove that sequence from the other command's keys (`setKeys(other, keys without it)`), then `setKeys(id, ...)` with the edited slot. **Reset** (current command) and **Reset all** (asks first). Rebuilds itself on `registry.changed()`. Text under the tree explains the Terminal rule (one paragraph from the spec).
- `ShortcutsDialog`: same tree read-only plus a "Fixed keys" group listing the spec's fixed keys with their native text; filter field filters by title or chord text (case-insensitive); **Customize…** emits `customizeRequested()`; Escape closes. Rebuilds on `changed()`.

- [ ] **Step 1: Failing tests** `test_keyboardsettings.cpp` (use an isolated `QSettings` path as in `test_shortcuts.cpp`, and a local `ShortcutRegistry registry(Shortcuts::Platform::Linux)`): lists every command once; editing `go.anywhere` to Ctrl+J persists (`registry.keys`); Return is refused with a visible reason; Ctrl+N for `go.anywhere` asks and, when accepted (auto-answer with a `QTimer::singleShot` that clicks the message box's Yes button), moves it from `session.new`; Reset all asks then clears; Terminal marker present for Ctrl+Tab and absent for Ctrl+K; cheat sheet filter "tab" shows only tab commands; Customize emits; cheat sheet updates after `setKeys`.
- [ ] **Step 2: Run**; expect FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the new test and `ctest -R settings`; expect PASS.
- [ ] **Step 5: Commit** `"Add Keyboard settings and the shortcut cheat sheet"`.

---

### Task 5: Go to popup

Independent of Tasks 2, 3 and 4.

**Files:**
- Create: `tray/src/GoToDialog.h/.cpp` (add to `hgs-tray-ui`)
- Test: create `tray/tests/test_gotodialog.cpp`

**Interfaces:**
- Produces:

```cpp
struct GoToItem {
    enum Kind { Session, Project, Account, Machine, Section, Command };
    Kind kind = Session;
    QString id;       // session key, project id, account id, machine host, page id or command id
    QString host;     // machine for sessions and accounts
    QString title;    // primary text
    QString detail;   // secondary text: project · machine · status, provider · machine, …
    QString chord;    // native chord text for commands, else empty
    int recency = -1; // 0 = most recent session of this run, -1 = none
};
namespace GoTo {
int score(const QString &query, const QString &text);           // <0 = no match
QList<GoToItem> rank(const QList<GoToItem> &items, const QString &query);
}
class GoToDialog : public QFrame {           // objectName "goToDialog", Qt::Popup
    Q_OBJECT
public:
    explicit GoToDialog(std::function<QList<GoToItem>()> provider, QWidget *parent);
    void popup();                             // fetch items, clear query, show at the top center of parent
signals:
    void activated(const GoToItem &item);
    // children: "goToQuery" (QLineEdit), "goToResults" (QListWidget)
};
```

Behavior (spec "Go to"):
- `score`: case-insensitive subsequence match over `title + " " + detail`; base points per matched char; bonuses: match at title start (+100), at a word start (+20 per char), contiguous run (+10 per char after the first); penalty by gap length. Empty query → 0 for all.
- `rank`: query starting with `>` keeps only `Command` items and matches the rest of the query. Empty query: sessions with `recency >= 0` ascending first, then the remaining items in input order. Otherwise sort by score desc, then kind order (Session, Project, Account, Machine, Section, Command), then input order; drop non-matches.
- UI: `QLineEdit` + `QListWidget` (title bold, detail muted, chord right-aligned); ↑/↓/PgUp/PgDn move in the list while typing; Enter emits `activated(item)` and hides; Escape hides; clicking an item activates. Width `min(640, parent width - 32)`; max 12 visible rows.

- [ ] **Step 1: Failing tests** `test_gotodialog.cpp`: score ordering (`"ses"` ranks "Sessions" above "Close session"); subsequence (`"zrs"` matches "Zerus") and non-match; `>` filter; empty query recency order; Enter emits the highlighted item; arrow keys move the highlight; Escape hides without emitting.
- [ ] **Step 2: Run**; expect FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run**; expect PASS.
- [ ] **Step 5: Commit** `"Add the Go to popup"`.

---

### Task 6: Integration, documentation and verification (coordinator)

**Files:**
- Modify: `tray/src/SessionsWindowShortcuts.cpp`: `openGoTo()` builds items (sessions from `m_fleet` entries excluding archived, with `recency` from an in-memory MRU list updated in `selectSession`; projects from `m_organization`; accounts from the Accounts page catalog; machines from the fleet peers; the six sections; every command with `hint`), shows a lazily created `GoToDialog`, and handles `activated` per spec "Go to" activation. `openShortcutHelp()` shows a lazily created `ShortcutsDialog`; `customizeRequested` → `showWorkspaceSettings(); m_settingsPage->openKeyboard();`.
- Modify: `docs/reference.md`: replace the "Common shortcuts" paragraph with a `### Keyboard` section (defaults table for Linux/Windows and macOS, Terminal rule, text-field rule, Go to, cheat sheet, rebinding).
- Test: `test_sessionswindow.cpp`: Ctrl+K opens Go to; activating a session item selects it; activating a command item runs it; F1 opens the cheat sheet and Customize opens Settings → Keyboard.

- [ ] Steps: failing tests → implement → full `ctest --test-dir .ci-build/gui --parallel 8` with `env -u QT_QPA_PLATFORM` → `check-source.py` → commit `"Wire Go to and the cheat sheet into the workspace"` → push to main → watch CI → close beads.
- [ ] Manual (owner or coordinator on a real desktop): Russian layout chords, macOS ⌃Tab/⌃`/⌃X ⌃S.
