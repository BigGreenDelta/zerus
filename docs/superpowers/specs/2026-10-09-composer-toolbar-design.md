# Composer toolbar

Status: approved design, 2026-10-09. Tracking: `zerus-sdr`.

## Goal

Give the Activity view as much height as possible and stop the message field from
moving. Everything that currently sits between the conversation and the message
field collapses into one toolbar row of fixed height directly above the field.
Notices become compact chips whose full text and actions open in a popover.
Attachments become a single chip.

## Non-goals

- Changing how cache, recovery, usage, compaction or attachment state is computed.
  Every chip reads the functions that drive today's widgets.
- Changing the composer's bottom row (attach button, hint, model settings, Stop,
  Send) or the `QuestionCard`.
- Changing draft storage or the attachment wire format. `ComposerDraftStore` and
  `MessageAttachment` stay as they are.
- Scaling the toolbar with the content scale. Like the bottom row, it keeps the
  workspace size.

## Current state

Between `ActivityView` and the field, top to bottom (`SessionsWindow.cpp`, activity
page; `MessageComposer.cpp`):

1. `QuestionCard`: an agent question or permission request. A required question
   hides the whole composer.
2. `m_usageWarning`: a red label with `AccountUsage::exhausted()`.
3. `RecoveryUi::Panel`: provider error or automatic retry, with Retry now, Cancel
   retry, Attempts…, Open Terminal, Refresh usage and Settings….
4. `m_cacheNotice`: `CacheStatus::warning()` text and a Clear context button (about
   50 px).
5. `activityFooter` (about 32 px): Mark as read, compaction indicator and Cancel send
   on the left; Jump to latest in the centre; `CacheStatus::Button` and
   `SessionUsage::ContextButton` on the right.
6. The composer: a 78 px attachment strip of 174×58 tiles, the 76 px field, the
   bottom row.

The subagent page has its own `MessageComposer` and a separate right-aligned
`m_subagentContextUsage` row above it.

## Layout

The toolbar is a free row without background or frame, 34 px high, directly above
the field's frame (option C of the visual review). It is visible whenever a session
is selected, also when it holds no chip, so the field never moves.

Order, left to right:

- Leading zone (message and session): attachments, Voice, Jump to latest,
  Mark as read, compaction.
- Stretch.
- Trailing zone (status by decreasing severity): usage limit, recovery, cache,
  context. The context counter is always rightmost.

Owner revision, 2026-10-14: Jump to latest is a compact double-down-arrow icon beside
Voice in the existing toolbar, outside the transcript. Its tooltip and accessible
name include the number of new events. The subagent toolbar owns its separate jump
action. The toolbar height stays fixed when either action appears or disappears.
Standalone `ActivityView` instances retain their overlay fallback.

`QuestionCard` stays above the toolbar.

## Components

### `ComposerToolbar.{h,cpp}` (new)

- `ToolbarChip : QPushButton`
  - Height 24 px, radius 6 px, font 11 px, optional leading `workspaceIcon`.
  - `Tone`: `Quiet`, `Neutral`, `Success`, `Warning`, `Danger`, `Accent` (a green pill, radius 11 px:
    text `#8bdfc0` / `#167357`, fill `#233a35` / `#e7f3ed`, border `#456e61` / `#a5c8b8`). Colours come from
    the existing palette (dark / light): Danger `#ff9ca8` / `#b52d48`, Warning
    `#efbd78` / `#91621a`, Success `#72cdb2` / `#237a62`, Quiet `#a1adbb` /
    `#647386`. Neutral uses the composer field colours. Warning and Danger chips
    have a tinted background and border; Quiet and Success are text only.
  - `setLabels(full, shortText)` and `setCompact(bool)`. `accessibleName` is always
    the full label; `accessibleDescription` and the tooltip carry the full notice.
  - `setPopoverContent(QWidget *)`. With content, a click, Enter or Space opens the
    popover; without content, the chip only emits `clicked`.
  - `flash()`: a one-second accent border, used after an attachment is added.
  - Focus policy `Qt::TabFocus`, so a mouse click does not take focus from the field.
- `ChipPopover : QFrame`
  - The `SettingsPopup` from `MessageComposer.cpp` moves here unchanged in mechanism:
    `Qt::Popup | Qt::FramelessWindowHint`, translucent, painted with
    `PE_Widget`. The model settings popup uses it as well.
  - Opens above its anchor chip, aligned to the chip's nearer edge and kept inside
    the window. Escape closes it. Closing returns focus to the widget that had it
    when the popover opened: the chip after keyboard activation, the message field
    after a mouse click (chips never take focus on click).
- `ComposerToolbar : QWidget`
  - Fixed height 34 px; never wraps.
  - `add(Slot, QWidget *)`. A fixed slot table owns each item's zone, visual order,
    priority and hideability, so the order does not depend on which code adds an
    item first:

    | Slot | Zone | Priority (lower gives way first) | Hideable |
    |---|---|---|---|
    | `Attachments` | leading | 20 | no |
    | `MarkRead` | leading | 10 | yes |
    | `Compaction` | leading | never shortened | no |
    | `CompactionCancel` | leading | never shortened | no |
    | `UsageLimit` | trailing | 50 | no |
    | `Recovery` | trailing | 40 | no |
    | `Cache` | trailing | 30 | no |
    | `Context` | trailing | never shortened | no |

    Items can be any widget; `ToolbarChip` items take part in compaction. Owners show
    and hide chips only through `ToolbarChip::setActive()`, because the toolbar may
    additionally hide a hideable chip that does not fit.
  - Refits on resize and whenever an item's visibility or label changes.

### `MessageComposer`

- Removes `m_attachmentScroll`, `m_attachmentList`, `m_attachmentsLayout`, the tiles
  and the tile code in `rebuildAttachments()`.
- Owns a `ComposerToolbar` above the field: `toolbar()` returns it.
- Adds an attachments chip to the leading zone: Neutral, `attachment` icon, labels
  "%n attached" / "%n", hidden when the draft has no attachments, `flash()` on every
  successful `addAttachment()`. Its popover lists each attachment: a thumbnail
  (images) or file icon, the reference (`[Image #2]`), the name elided in the middle,
  the size in KiB, and a remove button. Hovering a row shows a large preview of an
  image; activating a row stores the bytes with `AttachmentFiles::store()` and opens
  them with the desktop's default application. Remove keeps today's behaviour,
  including removing the reference from the text, and is disabled while sending.
- Adds `setInputVisible(bool)`: hides or shows the field and the bottom row (and the
  narrow-mode feedback row); the toolbar stays.

### `RecoveryUi::Panel`

- No longer calls `setVisible()` on itself. It exposes `bool active() const` (today's
  visibility condition) and a `std::function<void()> summaryChanged` callback.
- `summary()` returns the chip labels and tone:
  - waiting, dispatching, retrying: Warning, full label `RecoveryUi::status()`
    (e.g. "Retry in 42 s"), short label the remaining seconds ("42 s") or "…".
  - cancelled: Warning, full label `RecoveryUi::status()`, short label "Off".
  - provider failure, exhausted, blocked, uncertain: Danger, full label
    "Provider error" for a failure and `RecoveryUi::status()` otherwise, short label
    "Error".
- The existing one-second timer calls `summaryChanged`, so the chip ticks with the
  panel. All buttons and their actions are unchanged; the panel becomes the chip's
  popover content.

### `CacheStatus`

`CacheStatus::Button` is replaced by `CacheStatus::Chip : ToolbarChip`, which keeps
the one-second countdown timer. The labels come from a pure
`CacheStatus::chipState(data)`, testable without widgets:

| State | Tone | Full / short label |
|---|---|---|
| `expired()` | Danger | "Cold cache" / "Cold" |
| `cache_hint.status == "saving_hint"` | Warning | "Clear suggested" / "/clear" |
| warm with remaining time | Success | "Cache ~%1m" / "~%1m" |
| otherwise | hidden | |

Its popover shows `warning()` (if any), the detail lines of today's tooltip, and two
buttons: Usage details (today's click: open the inspector usage tab) and Clear
context (only when `warning()` is non-empty; enabled under today's `m_cacheClear`
condition; runs `SessionsWindow::clearContext()`).

### `AccountUsage`

Adds `exhaustedSummary(data)`: "Limit reached · %1" with the time until the nearest
reset among exhausted current windows, or "Limit reached" when unknown. The chip is
Danger with the `attention` icon, short label "Limit". Its popover shows
`exhausted()` and a Refresh usage button that calls the existing refresh path.

### `SessionUsage::ContextButton`

Loses `setFixedSize(220, 24)`: fixed height, width from its content. It stays a plain
button that opens the inspector.

### `SessionsWindow`

- Removes `activityFooter` and `m_cacheNotice`. `m_cacheWarning` and `m_cacheClear`
  move into the cache popover, `m_usageWarning` into the usage-limit popover, and
  `m_recovery` leaves the activity layout for the recovery popover.
- Adds to `m_composer->toolbar()`:
  - leading: Mark as read (Accent: the former green pill, `read-all` icon, "Mark as read" / icon only,
    hideable, lowest priority), then `ActivityView::compactionIndicator()` and
    `m_compactCancel` (shown as an underlined link) as two adjacent items;
  - trailing: usage limit, recovery (`refresh` icon), cache (`context-warning` icon
    when Warning or Danger), `m_contextUsage`.
- `renderDetails()` and `renderAccountUsage()` set chip labels, tones and visibility
  where they set widget state today.
- A required question calls `m_composer->setInputVisible(false)` instead of
  `setVisible(false)`; focus handling after the answer is unchanged.
- The recovery focus rule in `renderDetails()` (return focus to the field when the
  panel that had focus disappears) applies to the popover: when recovery becomes
  inactive while its popover is open, the popover closes and focus returns to the
  field.
- Subagent page: `m_subagentContextUsage` moves into the subagent composer's
  toolbar. The subagent composer is always shown on that page; where it is hidden
  today (`send_supported` false, reset), it calls `setInputVisible(false)` instead.

## Narrow widths

On every relayout the toolbar fits its items into the available width:

1. Measure all visible items with full labels.
2. While they do not fit, switch the lowest-priority chip that still has a full
   label to its short label. Priority, lowest first: Mark as read, attachments,
   cache, recovery, usage limit.
3. If they still do not fit, hide hideable items (Mark as read).
4. As a last resort, elide the longest chip text. Notices and the context counter
   are never hidden; the row never wraps and its height never changes.

## Keyboard and accessibility

- Chips are reachable with Tab and Shift+Tab; Enter or Space opens a popover, Escape
  closes it and returns focus to the chip. Popover buttons are keyboard operable.
- Attachment rows in the popover are focusable; Enter or Space opens the attachment.
- Tone is conveyed by icon and text, never by colour alone.
- Escape in the field still interrupts a working turn; chips do not change that.

## Removed object names

Tests and styles that refer to `messageAttachments`, `attachmentTile`,
`activityCacheNotice`, `activityCacheWarning`, `clearContextFromCache`,
`activityCache` and the footer placement of `activityJumpLatest` move to the new
names: `composerToolbar`, `messageInputArea`, `attachmentsChip`,
`attachmentsPopover`, `attachmentRow`, `attachmentPreview`, `usageLimitChip`,
`usageLimitPopover`, `usageLimitRefresh`, `recoveryChip`, `cacheChip`,
`cachePopover`, `cacheWarning`, `cacheDetail`, `cacheUsageDetails`,
`cacheClearContext`, `chipPopover`.

These keep their names: `activityMarkRead` (now a chip), `activityUsageWarning`
(now the text inside the usage-limit popover), `recoveryPanel`, `recoveryNow`,
`recoveryCancel`, `recoveryHistory`, `providerErrorTerminal`,
`providerErrorRefresh`, `cancelCompactSend`, `activityCompaction`,
`activityContext`, `subagentContext`, `activityJumpLatest`, `sessionSettingsPopup`
and `removeAttachment` (now the remove button of a popover row).

## Testing

- New `tray/tests/test_composertoolbar.cpp`:
  - the row height is constant with zero, one and all items, visible or hidden;
  - compaction at 900, 640, 420 and 320 px: order of short labels, Mark as read
    hidden last, notices and context never hidden, no wrap;
  - `accessibleName` stays the full label when compact;
  - the popover opens on click, Enter and Space; Escape closes it and restores
    focus; clicking a chip leaves focus in the field.
- `test_messagecomposer.cpp`: tile-based tests move to the chip and its popover.
  The count follows paste, drop, file dialog and removal; removing from the popover
  removes the reference from the text; attachments survive session switches and
  draft restoration; `setInputVisible(false)` keeps the toolbar visible.
- `test_sessionswindow.cpp`: the tests that look up the removed object names check
  the same behaviour through chips: Clear context from the cache popover keeps the
  draft; Retry now and Cancel retry work from the recovery popover; the recovery chip
  ticks each second; a required question keeps the toolbar visible; Jump to latest
  shares the fixed toolbar without covering messages; the subagent context counter
  sits in the subagent toolbar.
- An optional preview test (skipped unless `HGS_COMPOSER_PREVIEW` names a folder),
  following `HGS_DASHBOARD_PREVIEW` in `test_dashboard.cpp`: `grab()` of the
  composer in dark and light themes, wide and narrow, for the seven reviewed states
  (normal, attachment added with unread, attachments popover, compaction, recovery
  waiting with popover, usage limit, narrow). The screenshots are compared with the
  reviewed mockups before handoff.
- Checks: `bash scripts/ci/gui.sh` and `python3 scripts/ci/check-source.py`.

## Compatibility

No change to the `hgs` CLI, `HGS_*` variables, state or config paths, draft files, or
the attachment protocol. The GUI is not restarted as part of the change; live agents
and tmux servers are not touched.
