# Presence-aware mobile notifications

Status: approved design, 2026-10-10. Tracker: Beads epic `zerus-rk5b`.

## Goal

Phone notifications must not duplicate what the owner already sees at a computer.
Like Slack, a phone alert is held while the owner is active on any Zerus desktop
and is dropped when the item is handled or read anywhere. It is delivered when
the owner is away and the item is still unread and unresolved.

Owner decisions (2026-10-10):

- Desktop presence and read state both suppress phone alerts.
- Read state is shared by all devices: reading on any desktop or phone marks the
  item read everywhere.
- One delivery setting exists on desktop and phone and stays synchronized.
- Approach A: `hgs` is the source of truth; the relay decides and holds pushes.

Non-goals: changing desktop notification timing or sounds, end-to-end
encryption, iOS, treating phone use as desktop presence.

## Current behavior

- The connector posts `hgs ls --json --local` snapshots of its machine and of its
  direct peers to the relay every few seconds.
- `registry::event_changes` derives `attention`, `error` and `completed` events
  from snapshot transitions. Each event immediately coalesces into one push job
  per device; the generic FCM wake makes the phone fetch events and decide alerts.
- Desktop read state (`attention/readReplies`, attention marks) is private to
  each GUI in `QSettings`. The relay and other desktops never see it.

## Sources of truth

### Read marks (per session, on the session's host)

`hgs` stores read marks next to session state on the machine that runs the
session. One record per session conversation:

```json
{"conversation_id": "...", "reply_id": "...", "attention_signature": "...", "at": 1760000000.0}
```

- `hgs [@host] read <session> --json` accepts `{conversation_id, reply_id?,
  attention_signature?}` from stdin. A field is applied only when it equals the
  session's current value; otherwise the command reports `stale` and changes
  nothing. Marks therefore only move forward and an old mark never hides a
  newer reply or question.
- Writers: a GUI on the same machine (local), a GUI on another machine
  (`hgs @host`), and the phone (new relay operation, executed by the connector).
- `hgs ls --json` adds per session:
  - `attention_signature`: a canonical digest of the current actionable state
    (run, conversation, phase when `approval`/`input`/`error`, attention and
    question identities, pending `mobile_attention`, and actionable subagent
    requests). An error without a native identity uses its turn start, so a
    second error is not mistaken for a read one. `null` when nothing needs
    attention. `hgs` reads pending Codex questions from the same index the
    connector publishes as `mobile_attention`. `hgs` is the only place that
    computes it; desktop and relay compare it opaquely.
  - `read`: the current mark or `null`.
- A session is unread when `reply_id` differs from `read.reply_id`, and its
  attention is unacknowledged when `attention_signature` is non-null and differs
  from `read.attention_signature`.

### Desktop presence (per machine)

- The tray agent (running even with the window closed) samples OS input idle
  time and screen lock every 30 s and on transitions, and calls
  `hgs presence set --json` with `{idle_seconds, locked}`. `hgs` writes
  `desktop-presence.json` atomically in its state directory with its local
  write time.
- `hgs ls --json --local` adds top-level `desktop_presence: {idle_seconds,
  locked, locked_seconds}`, aged to the moment of listing. It is omitted when the
  file is older than 120 s (GUI closed, crashed or machine asleep).
- Values are relative durations, so the relay converts them with its own receipt
  clock and machine clock skew does not matter. No content is included.
- Presence travels next to the snapshot, not inside it: the connector removes
  `desktop_presence` from each listing and sends it as heartbeat field
  `presence` (gateway and each direct peer). Changing idle time therefore never
  rewrites or re-hashes an unchanged snapshot.
- Platform sources:
  - macOS: `CGEventSourceSecondsSinceLastEventType` (no permission prompt) and
    `CGSessionCopyCurrentDictionary` for lock.
  - Linux: KIdleTime for idle time (Plasma on Wayland does not support
    `org.freedesktop.ScreenSaver.GetSessionIdleTime`), `org.freedesktop.ScreenSaver.GetActive`
    for lock; GNOME: `org.gnome.Mutter.IdleMonitor`.
  - Otherwise only input in Zerus windows counts, which can only cause extra
    phone alerts, never missed ones.

### Delivery setting (swarm-wide)

- New swarm catalog key `Preference { field: "mobile_delivery" }` with values
  `immediate | away | away_5 | away_10 | away_15 | away_30`; default `away`.
- `hgs swarm preference get|set` reads and writes it; a single machine without a
  swarm uses its local catalog the same way.
- Desktop Settings and the phone edit the same value. The connector includes
  `preferences.mobile_delivery` in its own machine snapshot.
- Older swarm members reject unknown catalog keys, so preference operations are
  not sent to them; project synchronization keeps working. Desktop Settings
  names members that are too old to receive the shared value.

## Relay decision rule

### Alert candidates

Each derived event becomes a candidate row: workspace, computer, session, kind,
`run_id`, `conversation_id`, `attention_signature` (attention/error) or
`reply_id` (completed), and creation time. Candidates replace immediate push
creation. They expire after 24 hours and are bounded per workspace like events.

### Evaluation

Candidates are re-evaluated on every accepted snapshot (which carries presence,
read marks and the preference) and on a maintenance tick about every 15 s. The
relay worker that accepted a changed snapshot or new presence evaluates that
workspace inline; an advisory-elected maintenance pass rechecks due candidates.
Several workers may evaluate the same candidate: deleting the candidate row in
the transaction that appends its `alert` event delivers it once. A computer
reachable directly and through a gateway is evaluated against the route the
catalog shows (available before stale, local before a gateway), never against
a snapshot from its other, possibly lagging, route.

1. Drop the candidate when:
   - the session is gone or archived, or its run or conversation changed;
   - attention/error: `read.attention_signature` equals the candidate's or the
     current `attention_signature`, the current signature is absent, or it
     differs and the native need for the candidate's kind is gone. A different
     signature alone does not drop it: `hgs` signatures also change on details
     the relay never announces (subagent requests, request start times,
     unindexed or removed questions), while a genuine replacement is a new
     event whose candidate replaces this one;
   - completed: `read.reply_id` equals the candidate reply, a newer reply exists,
     or the session is busy again.
   - A candidate without an attention signature (for example recovery-only
     attention) is dropped only by resolution, never by a read mark.
2. Deliver when the preference is `immediate`, or when the owner is away long
   enough:
   - A machine is present when its latest snapshot is at most 120 s old and
     carries `desktop_presence`.
   - The owner is active when some present machine is unlocked with
     `idle_seconds < 120`.
   - Away since T: the end of the last known activity, i.e. the latest
     `received_at - idle_seconds + 120` over all machines with remembered
     presence (stale ones included), or the lock time when earlier. Stale
     machines cannot make the owner active but still bound T, so closing a
     laptop starts the `away_N` countdown. Without any presence ever received
     the owner has been away indefinitely.
   - `away` delivers as soon as the owner is not active; `away_N` delivers when
     `now >= T + N minutes`. A candidate created while already away for long
     enough is delivered immediately.
3. Otherwise keep waiting. Active and unread candidates wait until handled,
   read, away or expired, as in Slack.

When information is missing (machine not reachable by the connector, GUI
closed, stale snapshot), the owner counts as away: an extra alert is preferred
over a missed one. A connector without the new fields therefore behaves like
today with `away` and nobody present.

### Delivery

- A delivered candidate appends an event of kind `alert` with `alert_kind`
  (`attention | error | completed`), computer and session, then coalesces the
  existing generic FCM wake for every registered device.
- Capabilities advertise feature `presence_alerts`.
- Clients that know the feature notify only on `alert` events; ordinary events
  only refresh state. The live connection therefore follows the same rule.
- API v1 changes are additive: new event kind, new feature, optional snapshot
  fields and two operations (below). Older clients ignore unknown event kinds.

## Desktop

- `FleetState::markSessionRead`, `markReplyRead` and `markRepliesRead` send
  `hgs [@host] read` instead of writing `QSettings`. The UI applies the mark
  immediately; failed writes stay queued and retry without reverting the UI.
- Unread and acknowledged state come from snapshot `read` and
  `attention_signature`. A read on one desktop reaches the others on their next
  poll (peers are polled once a minute); `AttentionTracker` withdraws their
  system notifications through its existing clear path.
- One-time migration: a new GUI sends its existing `QSettings` marks to the
  sessions' hosts and records that migration finished. Old keys are kept.
- Settings adds "When I'm not at a computer, send notifications to my phone":
  Immediately, even if I'm at a computer / As soon as I'm away (default) /
  After 5, 10, 15 or 30 minutes away.
- Desktop notifications keep their current timing and sounds.

## Phone

- Machines → Notifications (where the app's notification settings live) shows the same six choices with the shared value
  from the latest snapshot. Changing it sends operation `set_mobile_delivery`
  to the gateway computer, executed by the connector as
  `hgs swarm preference set`, with the usual submitted/confirmed states and no
  automatic replay.
- Viewing a session's latest reply sends operation `mark_read` with exact
  computer, session, run, conversation, `reply_id` and `attention_signature`.
  It is idempotent; stale marks are ignored by `hgs`.
- With `presence_alerts`, notifications are created only from `alert` events.
  An `attention` or `error` alert announces the session's input or error card,
  whichever the phone shows. A posted input or error card follows detail
  changes of the same run silently; a need the relay announced without an
  alert waits for its alert. Per-type switches, card replacement, privacy and
  tap targets stay unchanged. Without the feature the app keeps today's
  behavior.

## Failure handling

- Missing or stale presence: away.
- Read write cannot reach the host: the desktop queue or the phone's durable
  request applies it later; an alert may be delivered meanwhile.
- Stale marks never suppress a newer reply or question.
- Relay restart: candidates are durable rows; evaluation resumes on the next tick.
- Storage: candidates expire after 24 h and are capped per workspace; only the
  latest presence per machine is kept (inside snapshots).

## Rollout

Each step is independently safe:

1. `hgs` CLI and desktop on all machines: read marks, presence, preference and
   snapshot fields.
2. Android: shared setting, `mark_read` and alert-only notifications, active
   only when the relay advertises `presence_alerts`. Older apps keep only the
   newest event per session, so they must not meet `alert` events first.
3. Relay: candidates and `alert` events; unchanged behavior without new
   fields. Replace all relay workers together.

Every condition the phone alerts on (including recovery-state errors and
questions known only by count) must produce a relay event, so alert-only mode
never loses one.

Implementation phases: (1) shared read state across `hgs`, desktop and phone,
(2) presence and the shared setting, (3) relay alerts and phone alert display.

## Testing

- `hgs`: forward-only marks and stale rejection; signature stability across
  status-only changes; presence aging and expiry; swarm preference merge and
  default; CLI integration through an isolated state directory.
- Relay: table-driven rule tests (preference × presence × read × time);
  `alert` event contract; one alert across two PostgreSQL workers; push
  coalescing; expiry and bounds.
- Desktop: mocked idle/lock providers; read writes through a fixture `hgs`;
  `QSettings` migration; cross-desktop read propagation; Settings choices.
- Android: alert-only notifications, fallback without the feature, setting sync
  and `mark_read` requests.
- End to end: active on Mac → no phone alert; read on Mac → candidate dropped;
  two minutes idle or locked → alert delivered.
