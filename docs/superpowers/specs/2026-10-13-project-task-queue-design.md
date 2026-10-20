# Project task queue

Status: draft, 2026-10-17. This spec covers the interface, the agent commands
and the task logic; the clickable prototype is `design/task-tracker/index.html`.
Storage, replication (to be built on Loro), the mobile relay, phases and
testing are being redesigned from a clean sheet and will get their own spec.

## Goal

Tasks are a convenient way to keep prompts and send them to agents by hand. A
person or an agent files a task. It waits in Backlog or in the ordered To do
list until the person sends it to a session with Send now, or an agent starts
it. The session gets the task prompt, which names the command to report with;
the agent's report moves the task to In review, and the person or an agent
accepts it into Done or sends it back to To do. People and agents comment on
tasks.

A task is a light object. It does not follow the agent or the session after
the send: Zerus does not check that the prompt arrived, does not watch the
session, and does not change the task when the session ends. Everything
between the send and the report is the work of the agent and the operator.

A task is not a conversation. If an agent needs something from the person while
it works on a task, it asks in its session as it always does (Activity, a
question, the Needs input status). The task stays In progress.

When the person tells an agent inside Zerus "поставь таск" (or "add a task",
"put it in the queue", "for later"), the agent must create a Zerus task, not an
entry in its own todo list.

## Out of scope

- Any automatic sending: dispatching To do tasks to free sessions, and queues
  of tasks that wait for a session.
- Delivery confirmation, retries, and session states caused by tasks. A
  session that needs attention is the operator's concern, not the task's.
- Questions and answers through tasks. Agents ask in their sessions.
- Task dependencies and LLM coordinators. Agents may start tasks in other
  sessions of the project (see Agent interface), but Zerus plans nothing for
  them.
- Sending tasks to sessions of other projects.
- Syncing tasks with Beads or any other issue tracker. Zerus tasks live only in
  Zerus.

## Model

A task belongs to one project. The project is the catalog's project,
identified by its stable project ID; its name and colour can change. A task's
ID is `T-` plus six base-36 characters, such as `T-a1b2c3`, unique across
computers without coordination (see the replication spec); the full reference
shows the project's current name, `<project>/T-a1b2c3`. Task IDs are never
session names and are never presented as session IDs. The examples below use
short numbers for readability.

| Field | Meaning |
|---|---|
| `text` | The prompt itself. A task has no title; the text is editable while the task is in Backlog or To do |
| `state` | `backlog`, `todo`, `active`, `review`, `done` |
| `target` | One pinned session, or `any` session of the project |
| `author` | Who created the task: the person, or an agent with its session. Set when the task is created; it never changes |
| `created_at` | When the task was created |
| `session` | The session the task was last sent to |
| `summary` | The agent's latest report |
| `log` | Comments, sends, reports, cancellations and state changes, oldest first |

The interface is in English, like the rest of the GUI, and names the states
Backlog, To do, In progress, In review and Done. The order of To do is explicit;
the person and agents change it (see Agent interface).

```
backlog ⇄ todo ─(Send now, hgs task send)→ active ─(report, Cancel)→ review
review ─(Accept)→ done
review ─(Back to To do)→ todo
done ─(Back to To do)→ todo
```

Done is reached only through In review. A task leaves In progress in two ways:
the agent's report (`hgs task report`), or a Cancel; both lead to In review, where the
person decides. Every step is open to the person in the UI and to agents
through `hgs task`.

## Sending

A task is sent by the person with Send now, or by an agent with `hgs task send`.
It goes to the task's pinned session, or to the open session (the caller's
own, for an agent) when the target is Any session.

Zerus renders the task prompt (see Task prompt) and pastes it through hgs's
normal input path, the one the composer uses. If hgs refuses the input, for
example because the agent is not running or a question is open, nothing
changes and the reason is shown; Send now is disabled with the same reason
while hgs reports it, and `hgs task send` returns it as an error. Otherwise the
task becomes In progress in that session and the send is logged. Zerus does
not check that the agent received the prompt.

A working agent receives the prompt in its own input queue and reads it when
its turn ends, as with any message; so an agent may start a task in its own
session. A session may have several tasks In progress.

A computer never sends a task that its own copy already shows In progress:
Send now and `hgs task send` refuse it there ("T-41 is already In progress in
api"). Two computers that send the same task before their copies meet both
send it; each agent works on it and reports, and the second report is added to
the history (see Report).

After the send the task does not follow the session. If the session ends,
starts a new conversation, is archived or is forgotten, the task stays where
it is; the operator cancels it or lets it be. A pinned task in To do or
Backlog whose session no longer exists keeps its pin; Send now says "api no
longer exists — choose another session".

## Task prompt

The prompt carries the task's text; everything else about the task the agent
reads itself with `hgs task get`: earlier sends and their sessions, reports and
comments. The prompt is a template, editable per project. Placeholders:
`{ref}`, `{author}`, `{prompt}`, `{tools}`, `{history}`. Default:

```
[Zerus task {ref}]
Written by {author}.
{prompt}
{history}
When you finish, or find that it cannot be done, report with
{tools}.task_report: one to three sentences on what you changed, how you
checked it, and what is left undone or why it cannot be done.
```

`{author}` is the task's `author` field: "the person" or "the agent of
codex/api", naming the agent's session. `{history}` is empty the first time the task is sent; later it
is one line:

```
This task was sent before; read its history with {tools}.task_get
orchard/T-41 before you start.
```

`{tools}` names the `hgs task` commands.

The rendered prompt follows hgs's input limits: at most 64 KiB, and it never
starts with `/` or `!`. The task input, `hgs task add` and `hgs task edit` refuse a
text that would not fit; a template must start with its `[Zerus task …]` line,
and the settings do not save one that does not.

## Report

`hgs task report REF` records the agent's summary and moves the task
from In progress to In review, both when the work is done and when the agent
found it cannot be done; the summary says which. A report for a task that is
not In progress, for example one already cancelled or reported, is added to
its history and leaves the stage as it is. The In review row shows the latest
report.

## Cancel

Only an In progress task can be cancelled. The person presses Cancel (✕); a
confirmation dialog shows the task text and what will happen: "T-41 moves to
In review, and api gets the cancel message." An optional Reason field is saved
as a comment on the task; it is never sent to the agent. The buttons are Keep
running and Cancel task.

On confirmation:

1. The task moves to In review with the log entry "Cancelled by the person"
   (or by the agent that cancelled it) and the reason, if any. The In review
   row shows the cancellation instead of a report.
2. Zerus pastes the cancel message into the session the task was sent to,
   through hgs's normal input path. It does not stop the agent's turn and does
   not check that the message arrived; a working agent reads it when its turn
   ends. If hgs refuses the paste, the reason is shown and logged; the task is
   cancelled all the same.

The cancel message is a per-project template like the task prompt:

```
[Zerus task {ref} cancelled]
The person cancelled this task. Stop working on it. Do not continue and do
not revert anything; list what you changed and wait for the person.
```

A report that arrives after the cancel is added to the task's history (see
Report).

## In review and Done

From In review the person, or an agent the person asked:

- presses **Accept**, which moves the task to Done;
- or presses **Back to To do**, which returns it to To do to be edited or sent
  again.

Back to To do also reopens a Done task.

A task that changes list — Backlog to To do, To do to Backlog, or Back to To
do — goes to the top of the list it joins. New tasks go to the end unless a
place is named.

Nothing changes a task's target on its own: a send, a report, a cancel and
Back to To do leave it as it was. Only the person (dragging, the settings) and
an agent (`hgs task target`, or `hgs task send` with an explicit session) change it.
The task's history names the session of every send, so another session knows
the earlier changes are not in its working tree.

## Agent interface

### Commands for agents

Agents use `hgs task` in their own shell, the commands the desktop uses. There
is no MCP server (owner decision, 2026-10-19; every session runs with bypassed
permissions). A command run inside an agent session acts as that session only
when it can prove it, as `hgs self` does: the run is live on this computer and
the command descends from its agent. The proof follows a rename to the
session's current name. A command whose environment names a session it cannot
prove is refused with the reason. Commands work on the session's catalog
project unless a reference (`PROJECT/T-xxxxxx`) or `--project` names another; an
agent of a session in no project names one. Texts (prompts, comments,
reports) are read from stdin.

| Command | Purpose |
|---|---|
| `hgs task add [--backlog] [--position N]` | File a task for the calling agent's own session; it goes to the end of To do (or Backlog) unless a position is given |
| `hgs task move REF --position N` (or `--top`, `--before REF`, `--after REF`) | Change the priority of any task in To do or Backlog |
| `hgs task edit REF` | Edit the prompt of a task in To do or Backlog |
| `hgs task stage REF todo\|backlog\|done` | Move a task between To do and Backlog, Accept it (In review → Done) or send it Back to To do (from In review or Done) |
| `hgs task target REF SESSION\|any [--host ALIAS]` | Change the session of a task in To do or Backlog |
| `hgs task cancel REF [--reason TEXT]` | Cancel an In progress task of another session, as the person's Cancel does; the reason becomes a comment. An agent cannot cancel a task it was sent itself |
| `hgs task send REF [--session SESSION [--host ALIAS]]` | Send a task as Send now does: to the session if given (the task is then pinned to it), else to its pinned session, else, for an Any session task, to the caller's own session |
| `hgs task list [--stage S]`, `hgs task get REF` | Read tasks as JSON; `get` returns the text, author, target, every send with its session, the reports, the comments and the log |
| `hgs task comment REF` | Add a comment; it is never pasted into a session |
| `hgs task report REF` | Report the task, done or found impossible; it moves to In review either way. The summary is one to three sentences on what changed, how it was checked, and what is left undone or why it cannot be done. This is the only way an agent moves a task into In review |

A task filed by an agent is always pinned to that agent's session; `add` takes
no target from an agent.

Agents act on any task of the project through these commands, as the person does in the
UI, without extra confirmation: priority, stage changes including Cancel and,
when the person asked for it, Accept, starting tasks in any session, editing
the prompt and the session. An agent cannot cancel a task it was sent itself;
if the task cannot be done, it reports with what is left undone. Every agent
action is recorded in the task's history with the agent's name; nothing else
marks it.

Two project settings bound this:

- **Agents may start and cancel tasks in other sessions**, on by default. When
  it is off, `send` and `cancel` work only for the caller's own session, and
  the commands say so.
- **Limit on tasks started by agents**, off by default: at most N agent starts
  per hour in the project; beyond it `send` is refused with the reason.

Sessions that run without bypassed permissions are outside this design: Claude
Code asks before the first `hgs task` command, and Codex's sandbox keeps the
command from writing the task store and from reaching other computers.

### Answers

Read commands print JSON. A change an agent makes prints one line that says
what happened and what to do next; `--json` prints the task instead. The person's
commands print JSON as before.

| Command | Answer |
|---|---|
| `report` accepted | "orchard/T-41 is In review." |
| `report` for a task not In progress | "orchard/T-41 is In review (cancelled at 14:06); your report was added to its history." |
| `send` refused (error) | "orchard/web cannot take it now: <hgs reason>. Nothing was sent." |
| `send` sent | "orchard/T-44 sent to orchard/web." |
| `cancel` | "orchard/T-41 is In review; orchard/web got the cancel message." |
| `cancel` of a task sent to the caller (error) | "You cannot cancel orchard/T-41, a task you were sent. If it cannot be done, report what is left undone." |
| `add` | "Filed orchard/T-51 in To do, position 5, for orchard/api." |
| `move`, `edit`, `stage`, `target`, `comment` | "orchard/T-41 is position 2 in To do.", "orchard/T-41 has the new prompt.", "orchard/T-41 is in Backlog.", "orchard/T-41 now goes to orchard/web.", "Added your comment to orchard/T-41." |

A session on another computer is named with it ("orchard/T-44 sent to
orchard/web on mac."), and a send or cancel whose copy has not come back says
"This computer's copy follows with the next sync." A refusal that the session's
computer gave reads as above; a connection that failed on the way keeps its
message, since the prompt may have been pasted. An agent's empty text is refused
with where it goes: "the report is empty; give it on
stdin, for example: hgs task report REF <<'EOF'".

### Instructions at session start

Zerus's SessionStart hook gives Claude Code and Codex sessions these
instructions each time the agent runs its SessionStart hooks for the session's
own conversation (not a nested agent CLI or a run Zerus did not record): at the
start, on resume and after a clear, and in Claude Code also after a compaction. Other
agents get them once their hooks are reviewed. `hgs guide tasks` lists the
commands.

```
Zerus tasks are this project's shared task list. Run `hgs task` in your shell:
it acts as your session, on your session's project; `hgs guide tasks` lists
the commands. Texts go on stdin, for example
hgs task report <ref> <<'EOF' ... EOF.

Filing. When the person says "поставь таск", "добавь задачу", "запиши задачу",
"в очередь", "на потом", "file a task", "add to the queue" or "for later",
file it with `hgs task add`. Never put these into TodoWrite or your own plan.
New tasks go to the end of To do unless the person names a place (--backlog,
--position N).

Working. A message starting "[Zerus task <ref>]" is a task for you; do it like
any request from the person. When the prompt says the task was sent before,
read its history with `hgs task get <ref>` before you start. If you need the
person, ask in this conversation as usual. When the work is done, or you find
that it cannot be done, report with `hgs task report <ref>`: what you changed,
how you checked it, and what is left undone or why it cannot be done.

Cancelled. "[Zerus task <ref> cancelled]" means stop that task now and follow
the message.

Managing. `hgs task list` and `hgs task get` show tasks and their history;
move, stage, target, edit, send and cancel change them as the person can.
Accept a task (`hgs task stage <ref> done`) only when the person asks you to.

Comments. `hgs task comment <ref>` adds a note for people and agents; it is
never pasted into a session.
```

## Desktop interface

Tasks live in a Tasks tab beside Activity and Terminal, in the central area. The
tab shows the project of the selected session; its label counts the tasks in
In review and in To do.

- **Stage tiles**: five compact one-line tiles, one per state (In review, In
  progress, To do, Backlog, Done), each with its icon, name and count. A
  stage's icon is the icon of the action that moves a task there: play for In
  progress, a list for To do, a clock for Backlog, a check for Done, and an
  inbox for In review, where the agents' reports arrive. The last tile is a
  square framed gear that opens the project's task settings; there is no
  header line above the tiles and no stage title or hint under them. The tiles
  stay in place; only the task list scrolls. A click selects the tile; the
  list below shows only that state. By default the selected tile is In review
  when it has tasks, otherwise To do. Dropping a task on the To do or Backlog
  tile moves it there. Done shows the tasks accepted in the last 7 days and
  counts only them; the list ends with "Show older (N)", which adds the older
  ones.
- **List**: one row per task. A row starts with a header line: the meta (ID,
  target or session, the session status for In progress, time, who filed it) on
  the left and the action icons on the right. The icons have tooltips and
  appear on hover. The same list of stage actions, in the same order, is used
  everywhere: on the desktop row, on the Android card and in the task settings
  on both. An action always sits in the same place, and the stage's main action
  is the rightmost: Edit (pencil), Move to Backlog (clock), Move to top (double
  chevron) and Send now (play) in To do; Edit, Move to To do (list with a plus)
  and Send now in Backlog; Cancel (✕), which opens the cancel dialog, in In
  progress; Back to To do (list with a plus) and Accept (check) in In review;
  Back to To do in Done. The header line has a fixed height, so showing the
  icons never moves anything, and the task text below it, clamped to two
  lines, uses the full width and is never covered. An In review row shows the
  agent's latest report, or the cancellation, under the text; a Done row shows
  only the text, the report stays in the settings and history. A click on the
  row, or its Edit icon, opens the task settings. Rows in To do can be
  reordered by dragging. There is no "new task" row.
- **Task settings**: a dialog with the prompt, the session, the history and a
  comment field. In To do and Backlog the prompt is editable, and the session
  is a choice of Any session or one of the project's sessions; Save applies
  both, Cancel or Escape discards them. In the other stages the prompt and the
  session are shown read-only. The dialog also has the stage's actions, the
  same as on the row except Edit; an action first applies unsaved changes to
  the prompt and the session, then closes the dialog.
- **Changing the session by dragging**: dropping a To do or Backlog task on a
  session in the session list pins it to that session; dropping it on the
  project's header makes it Any session. Sessions and headers of other
  projects do not accept it.
- **The open session's tasks are highlighted**: tasks pinned to it in To do and
  Backlog, and tasks sent to it in the other states. Their rows get an accent
  tint and an accent bar on the left, and the session name in the header line
  uses the accent colour.
- **Only this session**: a square framed toggle tile with a funnel, just
  before the settings gear, hides the tasks of other sessions: the open
  session's own tasks (the highlighted ones) stay, and so do Any session tasks
  in To do and Backlog, which the open session can take. Stage counts follow
  the filter, and To do keeps its project-wide queue numbers. The toggle stays
  on while the person switches sessions. Android has the same toggle as a
  filter icon in the Tasks top bar, beside the gear.
- **Task input**: tasks are created with the same input as Activity
  (`MessageComposer`), without the Activity toolbar. Its bottom row keeps the
  status text, has no attach button (a task is text only) and replaces the
  model selector, Stop and Send with one button named after the stage the task
  goes to: To do, or Backlog while the Backlog stage is open. The open session
  is the recipient: a task created here is pinned to it, and there is no
  target picker. Enter does the same as the button. Shift+Enter starts a new
  line. Nothing is sent to the agent.
- Project settings hold the task prompt and cancel message templates, the
  switch for agents acting on other sessions, and the optional limit on tasks
  started by agents.
- The inspector tab "Tasks & agents" becomes "Plan & agents": it shows the
  agent's own todo list, which must not be confused with Zerus tasks.

## Android interface

Android follows the desktop and keeps the app's own components. The prototype
page shows the phone beside the desktop window, with the same tasks in both.

- **Entry**: a Tasks button in the session screen's tools row, between Terminal
  and Session details, with the number of tasks In review as a badge. It opens
  the project's tasks full-screen, as Terminal does; Back returns to the
  conversation.
- **Session chips**: under the title, one chip per session of the project, with
  a status dot (working, needs input, ready). Tasks opens on the session of the
  conversation it was opened from. A tap on a chip picks another session for
  Tasks only: it receives new tasks, Any session tasks are sent to it and its
  tasks are highlighted. Back still returns to the conversation Tasks was
  opened from. The selected tab stays. A last ⋯ chip opens a bottom sheet with
  the sessions of every project, each with its status; picking one from
  another project shows that project's tasks.
- **Stages**: a scrollable tab row with a count on each tab, in the order of
  the desktop tiles, like the tabs of Session details. Done shows the last 7
  days and ends with "Show older (N)", as on the desktop. The selected tab
  scrolls into view. The gear in the top bar opens the task settings as a
  bottom sheet.
- **Cards**: the session card style. The header line holds the stage glyph,
  the ID, the session label (or "any session") and the same action icons as
  the desktop row, always visible; the pencil opens the task settings. In
  progress cards show the session badge; In review cards the agent's report or
  the cancellation. The open session's cards get an accent border. A tap opens
  the task settings as a bottom sheet: the prompt (editable in To do and
  Backlog), Session (a sheet with Any session and the project's sessions), the
  same actions as the desktop settings, the history and a comment field. Save
  appears once something changed; an action first applies unsaved changes.
  Send now is disabled there with its reason; on the card a tap on a disabled
  Send now shows the reason in a snackbar. There is no dragging on the phone:
  the session changes in the task settings, and To do is reordered with Move
  to the top.
- **Task input**: the app's message input without the attach button. With
  text, its round button adds the task: to To do, or to Backlog while the
  Backlog tab is open, as on the desktop. The placeholder names the stage.
  Nothing is sent to the agent. For now the task input has no microphone
  (owner decision, 2026-10-20).
- **Cancel**: an M3 alert dialog with the task text, what will happen, the
  optional Reason, and the buttons Keep running and Cancel task.
- **Colours**: popups use the app's own surfaces, not the Material 3 baseline
  (purple-tinted) defaults: bottom sheets #1A2225, dialogs #1F292C, snackbars
  #2A3538, sheet handles #354449. The existing Session details sheet should
  follow.
- **Notifications**: none for tasks. Tasks reaching In review show in the badge
  on the Tasks button.
- The Tasks tab of Session details, which shows the agent's own todo list, is
  renamed Plan, as on the desktop.

## Failure cases

| Case | Result |
|---|---|
| hgs refuses the input (agent not running, question open, …) | Send now disabled or refused with hgs's reason; nothing changes |
| The pasted prompt never reaches the agent | Not detected; the task stays In progress until the operator cancels it |
| The session ends, is cleared, archived or forgotten with a task In progress | The task stays In progress |
| A pinned task's session no longer exists | Send now says "api no longer exists — choose another session" |
| A report for a task that is not In progress | Added to the history; the stage stays |
| hgs refuses the cancel message | The task is cancelled all the same; the reason is shown and logged |

## Open questions

- Storage, replication, the mobile relay and phases are specified in
  `docs/superpowers/specs/2026-10-18-task-replication-loro-design.md`.
- Testing of the task logic, the interface and the agent commands.
