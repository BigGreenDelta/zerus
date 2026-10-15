# Desktop keyboard shortcuts

Status: approved direction, implementation starting, 2026-10-15. Tracking: epic
`zerus-vq25`. Facts were checked against `8b55892`.

## Goal

A person can drive the desktop workspace from the keyboard with chords they
already know from browsers, Telegram, Slack and VS Code: switch sections,
sessions, projects and accounts, switch Activity, Terminal and Processes, push
the agent's native queue, and find any of these by name. Chords work while
Terminal has focus wherever that does not take a key an agent needs. Every
command can be rebound in Settings.

## Owner decisions (2026-10-15)

- **Ctrl+Tab cycles the session view tabs** (Activity, Terminal, Processes,
  Native UI), as in browsers and IDEs. Sessions use Alt+↑/↓ and Ctrl+PgUp/PgDn.
- **Ctrl+Shift+Enter is Send now for an existing native queue only.** It never
  sends the typed message.
- **A separate two-step chord, Ctrl+X Ctrl+S, sends the typed message and pushes
  it**, like Claude Code's own send-now chord.
- **Rebinding in Settings ships in the first release**, not later.

## Current state

- Existing shortcuts: Ctrl/⌘+F (search), Ctrl/⌘+R and F5 (refresh), Escape (clear
  search, then stop the selected turn), Enter and Shift+Enter in the message field,
  Tab (take the suggestion into an empty field), arrows/Enter in lists, Menu and
  Shift+F10 (context menu), Ctrl/⌘-click (multiple selection), Space and Escape
  (stop voice capture), Alt+↑/↓ (reorder projects in the Projects dialog). They
  are scattered `QShortcut`s and `keyPressEvent` overrides.
- `TerminalScreen::event` accepts every `ShortcutOverride` on Linux and Windows.
  While Terminal has focus, no window shortcut works, not even Ctrl+F, and the
  keyboard cannot leave Terminal: Tab and Escape also go to the agent. On macOS
  every ⌘ chord except ⌘C/⌘V reaches the window.
- Agents and their TUIs use Ctrl+letters and Alt chords: Ctrl+C, Ctrl+R, Ctrl+O,
  Ctrl+T, Ctrl+B, Ctrl+G, Claude's Ctrl+X Ctrl+S, Kimi's Ctrl+S, shell editing
  keys. A session can also run an untracked command such as `htop`, which uses
  function keys. Terminal must keep all of these.
- `QPlainTextEdit` and `QLineEdit` on X11 bind Ctrl+K (delete to end of line) and
  Ctrl+U; on macOS ⌥↑/↓ move by paragraph. Text fields accept these overrides
  before a window shortcut can fire.
- The native queue snapshot (`src/state/input_queue.rs`) already reports the
  queue text, `can_send_now` and the provider's send-now keys (Claude Ctrl+X Ctrl+S,
  Codex Escape "interrupt and send immediately", Kimi Ctrl+S). Activity's **Send
  now** button calls `SessionsWindow`'s `queueSendNowRequested` handler, which
  checks identity and `can_send_now` before `requestQueueSendNow`.

## Notation

- **Mod** is Ctrl on Linux and Windows and ⌘ on macOS. In Qt this is the `Ctrl`
  modifier, which Qt maps to ⌘ on macOS.
- **Control** is the physical Control key on every platform. In Qt it is `Ctrl`
  on Linux and Windows and `Meta` on macOS.
- **Alt** is Alt, or ⌥ on macOS.

## Default keymap

Scope **Window** means anywhere in the main window, subject to the Terminal and
text-field rules below. Scope **Composer** means only while the message field
(main or subagent) has focus.

| Command id | Title | Linux / Windows | macOS | Scope |
|---|---|---|---|---|
| `go.overview` | Go to Overview | Ctrl+1 | ⌘1 | Window |
| `go.sessions` | Go to Sessions | Ctrl+2 | ⌘2 | Window |
| `go.projects` | Go to Projects | Ctrl+3 | ⌘3 | Window |
| `go.machines` | Go to Machines | Ctrl+4 | ⌘4 | Window |
| `go.accounts` | Go to Accounts | Ctrl+5 | ⌘5 | Window |
| `go.settings` | Open Settings | Ctrl+, | ⌘, | Window |
| `go.anywhere` | Go to… | Ctrl+K | ⌘K | Window |
| `help.shortcuts` | Keyboard shortcuts | F1, Ctrl+/ | F1, ⌘/ | Window |
| `session.next` | Next session | Alt+↓, Ctrl+PgDown | ⌥↓, ⌘⌥↓ | Window |
| `session.previous` | Previous session | Alt+↑, Ctrl+PgUp | ⌥↑, ⌘⌥↑ | Window |
| `session.nextAttention` | Next session that needs attention | Alt+Shift+↓ | ⌥⇧↓ | Window |
| `session.previousAttention` | Previous session that needs attention | Alt+Shift+↑ | ⌥⇧↑ | Window |
| `session.new` | New session | Ctrl+N | ⌘N | Window |
| `session.rename` | Rename session | F2 | F2 | Window |
| `session.search` | Search sessions | Ctrl+F | ⌘F | Window |
| `view.refresh` | Refresh | Ctrl+R, F5 | ⌘R, F5 | Window |
| `tab.next` | Next tab | Ctrl+Tab | ⌃Tab | Window |
| `tab.previous` | Previous tab | Ctrl+Shift+Tab | ⌃⇧Tab | Window |
| `tab.terminal` | Switch between Activity and Terminal | Ctrl+` | ⌃` | Window |
| `panel.toggle` | Show or hide the side panel | Ctrl+I | ⌘I | Window |
| `focus.next` | Next area | F6 | F6 | Window |
| `focus.previous` | Previous area | Shift+F6 | ⇧F6 | Window |
| `message.send` | Send message | Ctrl+Enter | ⌘↩ | Composer |
| `queue.sendNow` | Send queued messages now | Ctrl+Shift+Enter | ⌘⇧↩ | Window |
| `message.push` | Send and push now | Ctrl+X, Ctrl+S | ⌃X, ⌃S | Composer |
| `zoom.in` | Increase content scale | Ctrl+=, Ctrl++ | ⌘=, ⌘+ | Window |
| `zoom.out` | Decrease content scale | Ctrl+- | ⌘- | Window |
| `zoom.reset` | Reset content scale | Ctrl+0 | ⌘0 | Window |

The default chord strings are stored in one table in `tray/src/Shortcuts.cpp`, in
Qt portable notation with the platform choice made there (for example `Meta+Tab`
on macOS for Control+Tab, because ⌘Tab is the system application switcher and
⌘` the system window switcher).

### Fixed keys

These keep their current behavior, are listed in the cheat sheet and cannot be
rebound: Enter (send), Shift+Enter (new line), Escape, Tab and Shift+Tab focus
traversal, Tab suggestion in an empty message field, arrows/Home/End/Enter in
lists, Menu and Shift+F10, Ctrl/⌘-click, Space and Escape during voice capture,
Ctrl+Shift+C/V and Shift+PgUp/PgDn in Terminal (⌘C/⌘V on macOS), and Alt+↑/↓
inside the Projects dialog's list.

## Terminal rule

While a `TerminalScreen` in the main window has focus, a key goes to the window
only when it is bound to a Window-scope command **and** belongs to the
terminal-safe class. Everything else goes to the agent, bound or not.

- Linux and Windows: modifiers exactly Ctrl or Ctrl+Shift with a digit, Tab,
  Backtab, PgUp, PgDown, \` (QuoteLeft) or comma; and Ctrl+Shift+Enter/Return.
  Ctrl+Shift+C/V stay copy/paste. Ctrl+letters, Ctrl+Shift+letters, every Alt
  chord, Escape, Enter and function keys go to the agent.
- macOS: every chord with ⌘ except ⌘C/⌘V, as today. With physical Control
  (with or without Shift) only Tab, Backtab and \`. Every other Control chord and
  every ⌥ chord go to the agent.

So in Terminal the defaults that work are sections (Ctrl+1…5, Ctrl+,), session
switching via Ctrl+PgUp/PgDn, Ctrl+Tab, Ctrl+`, zoom reset (Ctrl+0) and
Ctrl+Shift+Enter; on macOS all ⌘ defaults plus ⌃Tab and ⌃`. Ctrl+` and Ctrl+Tab
are the keyboard way out of Terminal. Claude's own Ctrl+X Ctrl+S keeps working in
Terminal because Ctrl+letters always reach the agent.

## Text-field rule

While a text field (`QLineEdit`, `QPlainTextEdit`, `QTextEdit`) in the main window
has focus, a chord bound to a command in scope wins over the field's own binding
unless it is a protected editing key: `QKeySequence` standard keys Copy, Cut,
Paste, Undo, Redo and SelectAll; any chord without Ctrl, ⌘ or Alt (typing,
Shift+letters, plain navigation); and Ctrl/⌘ or Alt (⌥ on macOS) with
Left/Right/Home/End/Backspace/Delete, with or without Shift (word and line
movement). Function keys are not protected.

`message.push` starts with Ctrl+X (⌃X on macOS). On Linux and Windows Ctrl+X is
Cut. With a selection, Ctrl+X cuts as before. Without a selection, Cut has
nothing to do, so the composer lets the shortcut map take Ctrl+X as the first key
of the sequence. As with every Qt key sequence, a next key other than Ctrl+S
cancels it and is consumed; later keys are handled normally. On macOS ⌃X is not
an editing key, so there is no conflict. Copy may likewise start a rebound
sequence, since it also does nothing without a selection.

Escape and Enter keep their existing handling in the composer. `Ctrl+Enter`
(`message.send`) is an alias for Enter.

A focused combo box keeps Alt+↑, Alt+↓ and F4, which open its list: the router
claims these keys for it, so they never switch sessions there. A widget whose
own action uses a command's chord (the Projects list's Alt+↑/↓) keeps it while
it has focus: the window's command drops that chord until focus leaves, since
Qt would otherwise treat the two shortcuts as ambiguous and run neither.

## Routing

A `ShortcutRouter` event filter on `qApp` implements both rules. It acts only on
`QEvent::ShortcutOverride` whose focus widget belongs to the main window. If the
chord is bound to a command whose scope covers the focus widget, the router decides
with the Terminal or text-field rule. When the window should win, it ignores the
event and stops it there and at the focus widget's ancestors, so no widget can
claim it and Qt's shortcut map activates the command's `QAction`. Qt delivers
each override to the window object before the focus widget; the router decides
only at the focus widget and starts afresh for every event. Otherwise the event continues to the widget.
Dialogs and other windows are untouched. `TerminalScreen` needs no change: it
still accepts every override that reaches it.

Each command is a `QAction` on the main window (Window scope, `Qt::WindowShortcut`)
or on each composer editor (Composer scope, `Qt::WidgetWithChildrenShortcut`).
The router finds the action Qt would consider by walking the focus widget's
ancestors and their actions, and compares normalized keys (no keypad flag;
Backtab as Shift+Tab, which Qt's shortcut map also tries). Commands keep their
actions enabled and report "nothing to do" with a notice, so a disabled action
never hands its chord back to a text field.

## Command behavior

- **Sections** show the page exactly as the rail button does, including the rail
  checked state. `go.sessions` does not reset filters (the rail button's second
  click does).
- **`session.next` / `session.previous`** move the current row in the visible
  session list (the search results while they are shown) to the next or previous
  session row. Project headers, collapsed rows and subagent rows are skipped.
  Navigation does not wrap. From another page, they first show Sessions and move
  relative to the current selection. The draft and selection rules of ordinary
  selection apply.
- **`session.nextAttention` / `session.previousAttention`** move to the next or
  previous session for which the Needs attention filter predicate holds, in the
  order of the visible list (the search results while they are shown, where the
  current session's other result rows are skipped), within the current filter.
  They wrap around and, in the session list, reveal a collapsed project. With
  none, a notice says "No sessions need attention".
- **`go.anywhere`** opens the Go to popup (below).
- **`session.new`** does what the rail's **+** does.
- **`session.rename`** starts renaming the selected session while the Sessions
  page is shown and renaming applies, otherwise does nothing.
- **`session.search`** and **`view.refresh`** keep today's behavior.
- **`tab.next` / `tab.previous`** cycle the visible and enabled tabs of the
  session view and wrap. With no session selected they do nothing.
- **`tab.terminal`** shows Terminal and focuses it; from Terminal it shows Activity
  and focuses the message field when it is enabled.
- **`panel.toggle`** toggles the side panel like its header button.
- **`focus.next` / `focus.previous`** cycle focus on the Sessions page through:
  session list, the current view (Activity, Terminal, Processes or Native UI),
  the message field, the side panel. Hidden or disabled areas are skipped.
  On other pages they do nothing.
- **`message.send`** submits like Enter.
- **`queue.sendNow`** runs the existing Send now path for the selected session's
  current queue snapshot when `can_send_now` is true. Otherwise it shows a notice:
  "Nothing queued can be sent now".
- **`message.push`**: with an empty message field and no attachments, it behaves
  like `queue.sendNow`. Otherwise it sends the message through the ordinary send
  path and then arms a push for that session key, run and conversation. When the
  send is confirmed, the next inspection whose queue snapshot has
  `can_send_now` and an item that begins with the message start (the first 24
  non-space characters of its first text, whitespace-normalized) triggers the
  Send now path for that snapshot. Attachment references before that text must
  begin the item in the same order, each optionally followed by the location the
  CLI pastes after it (`[File #1] (Read the attached file at "/path")`, or
  `Inspect the attached image at` for an image). An item is a row that begins
  with the agent's marker (❯, ↳ or ›) with the indented rows that continue it;
  header rows such as Codex's "• Messages to be submitted after next tool call
  …" never hold a message.
  While another send-now request runs, a matching snapshot leaves the push
  armed for a later one. The push is dropped with a notice if the session, run
  or conversation changes, the send fails or is uncertain, or 20 seconds pass
  without such a snapshot (the agent may already have taken the message).
  Selecting another session drops it at once with a notice naming that session
  ("The message to *title* was not pushed: another session was selected"). At
  the deadline the notice says the message will be sent without push only while
  its request is in flight or the message field sends it by itself after a
  start or compaction; while a dialog such as the cold cache warning still holds
  it, the notice says it was not pushed because it was not sent within 20
  seconds. A new push replaces a pending one with a notice. A push never resends
  text. A message with attachments but no text is sent without a push, since
  the queue cannot be matched without text.
- **Zoom** changes `workspace/contentScale` in 10% steps within its bounds, applies
  it like the Settings slider, keeps the slider in sync and shows the new value
  in a notice.

Tooltips of the rail buttons, **+**, the side panel toggle and Activity's **Send
now** button include the current chord (`Shortcuts::hint(id)`), and update after
rebinding.

## Go to

Mod+K opens a popup at the top of the main window: a search field and a list.
Items are sessions (live and saved, not archived; title, project, machine and
status), projects, accounts (label, provider, machine), machines, the sections and
every command (with its chord). With an empty query the list shows recent
sessions of this run first, then the other sessions in the order of the session
list, and after them those the list does not show. Matching is a
case-insensitive subsequence match ranked by prefix, word-start and contiguous
matches. A leading `>` limits the list to sections and commands. ↑/↓ move,
Enter activates, Escape or the current `go.anywhere` chord closes.

Activation: a session opens like a notification does (`showSession`), but keeps
the current filter when the session is visible under it. A project reveals its
group and selects its first session; a project without sessions opens Projects
with that project selected.
An account opens Accounts on it (`showAccount`). A machine opens Machines on it
(`selectMachine`). A section shows it. A command runs.

The popup takes its items from a provider callback so it can be tested without
the window.

## Rebinding

Settings gains a **Keyboard** section, inserted before About so the existing
section indices stay valid. It lists every command by group with up to two chords
each and marks chords that also work in Terminal. Editing uses
`QKeySequenceEdit` with at most two keys per chord. Validation:

- Fixed and protected editing keys cannot be assigned; the dialog says why.
- A chord that conflicts with chords of other commands with an overlapping
  scope (Window overlaps both scopes), being equal to one or a prefix either
  way, asks once "Already used by *titles*. Reassign?". Reassigning removes the
  conflicting chords from all of them, and the edited command's other slot
  drops a conflicting chord too.
- **Reset** restores one command's defaults; **Reset all** asks first.

Overrides live in `QSettings` group `shortcuts`, key = command id, value = a list
of portable chord strings. A missing key means defaults; an empty list means no
chord. Preferences are per device, like other workspace preferences. Changes apply
at once: the registry emits `changed()`, actions and tooltips update.

## Cheat sheet

F1 or Mod+/ opens a non-modal **Keyboard shortcuts** dialog: commands by group
with their current chords for this platform, the fixed keys, one line on the
Terminal rule, a filter field and **Customize…**, which opens Settings → Keyboard.

## Components

- `tray/src/Shortcuts.h/.cpp` (hgs-tray-ui): command table, platform defaults,
  `ShortcutRegistry` (overrides, conflicts, `hint(id)`, `changed()`), protection
  and terminal-safe classification, and `ShortcutRouter`, the `qApp` event
  filter. Platform is a parameter so tests cover all three.
- `tray/src/SessionsWindowShortcuts.cpp` (hgs-tray-views): `SessionsWindow`
  members that create the actions and implement the commands, so most of the
  wiring stays out of `SessionsWindow.cpp`.
- `tray/src/GoToDialog.h/.cpp` (hgs-tray-ui): popup, ranking, provider interface.
- `tray/src/KeyboardSettings.h/.cpp` (hgs-tray-ui): the Settings section.
- `tray/src/ShortcutsDialog.h/.cpp` (hgs-tray-ui): the cheat sheet.
- `MessageComposer`: Composer-scope actions and the `pushRequested` and
  `sendNowRequested` signals. The Ctrl+X prefix rule lives in `ShortcutRouter`.

## Documentation

`docs/reference.md` replaces the "Common shortcuts" sentence with a Keyboard
section: the default table for both platform families, the Terminal and
text-field rules, Go to, the cheat sheet and rebinding.

## Testing

- `test_shortcuts`: defaults per platform, unique chords per scope, Backtab
  aliases, persistence round trip and empty lists, conflict detection and
  reassignment, protected keys, terminal-safe classification for each platform,
  router decisions for a terminal-like widget, a `QPlainTextEdit`, a `QLineEdit`
  and a plain widget, and windows other than the main one.
- `test_sessionswindow`: section chords, session next/previous skipping headers
  and subagents without wrapping, attention navigation with wrap and notice, tab
  cycling over visible tabs, Ctrl+` toggle and focus, panel toggle, F6 order,
  zoom, `queue.sendNow` request identity, push arming, confirmation, mismatch,
  timeout and session change, and Terminal focus: Ctrl+Tab and Ctrl+1 reach the
  window, Ctrl+C, Ctrl+R, Alt+↑ and Escape reach the terminal.
- `test_messagecomposer`: Ctrl+Enter sends; Ctrl+X Ctrl+S without a selection
  emits `pushRequested`; with a selection Ctrl+X cuts.
- Go to: ranking, `>` filter, activation per kind. Keyboard settings: rebinding,
  conflicts, protected keys, reset, persistence. Cheat sheet: lists current
  chords.
- Manual: Russian layout on Linux (X11 and Wayland), Windows and macOS (Ctrl+К
  must act as Ctrl+K, Ctrl+Ё as Ctrl+`); macOS ⌃Tab, ⌃` and ⌃X ⌃S.

## Non-goals

- Android hardware-keyboard parity.
- A tmux-style leader key.
- Global (system-wide) hotkeys outside the Zerus window.
- Rebinding the fixed keys.
