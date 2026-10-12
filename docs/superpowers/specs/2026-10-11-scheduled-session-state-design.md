# Scheduled session state

Task: zerus-rqfc. Owner decision, 2026-10-11: an idle agent whose own scheduled
jobs are pending is neither Ready nor Working. It is Scheduled, and it counts as
Active.

## Problem

Kimi `CronCreate` keeps a conversation doing work in the background: a
recurring job opens a turn every few minutes, the agent checks something and
goes idle again. Between ticks the session reports `activity: idle`, so every
client shows Ready and "Waiting for your next message" although the work is
still running and needs nothing from the person.

## State

`hgs ls` reports `schedules` for Kimi sessions:

```json
{"count": 1, "recurring": 1, "next_at": 1791744000.0}
```

- `count`: pending jobs, one-shot and recurring.
- `recurring`: pending recurring jobs. Reply notifications use this count:
  routine ticks are not new replies while it is positive.
- `next_at`: the earliest next run in seconds since the epoch, or `null` when
  no expression can be evaluated.

The object is `null` while the native cron journal cannot be read, and absent
for other providers. It replaces the unreleased `recurring_schedules` field.

A session is **Scheduled** when it is live and running, idle, needs no action
and `schedules.count > 0`. Precedence: needs action (approval, input, error) >
Working > Scheduled > Ready. An offline, paused or exited session is never
Scheduled: its jobs do not run.

## Next run

hgs folds `cron.add` and `cron.delete` from the main Kimi wire, keeping each
job's expression, recurrence, creation time and `cron.cursor` last fire. A
job's next run is the first local-time minute matching its five-field
expression after its last fire, or after its creation before the first fire,
as Kimi computes it. Kimi adds a small jitter, so the time is approximate. The
value depends only on the journal, never on the current time, so an unchanged
session keeps an unchanged snapshot. A time in the past means the run is due.

## Presentation

Desktop and Android show a Scheduled badge in its own color with a clock mark
and the next run time instead of an elapsed timer: `Scheduled · 22:00`, or
`Scheduled · due` when overdue. The desktop card detail reads "Next run at
22:00" or "Next run due"; Android cards keep their message preview. Scheduled
sessions do not pulse, do not count as working for the tray icon and add
nothing to attention counts or badges.

## Filters

Active includes Scheduled sessions; Needs attention does not. The shared
`session-filter-contract.json` covers scheduled, offline scheduled and unknown
schedule cases for both clients.

## Out of scope

Claude Code loops (`CronCreate`, `/loop`) can feed the same field later through
their own adapter. Phone notifications are unchanged.
