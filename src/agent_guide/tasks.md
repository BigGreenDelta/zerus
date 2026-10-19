Project tasks (hgs guide tasks)

The project's shared task list. Run these in your own session: they act as it
and on its project unless a reference names another (PROJECT/T-xxxxxx). Texts
go on stdin; a change prints one line saying what happened (--json: the task).

  hgs task list [--stage review|active|todo|backlog|done]   the tasks, JSON
  hgs task get REF                          one task with its history, JSON
  hgs task add [--backlog] [--position N] <<'EOF'   file a task for yourself
  hgs task report REF <<'EOF'               report a task you were sent
  hgs task comment REF <<'EOF'              a note, never pasted into a session
  hgs task edit REF <<'EOF'                 a new prompt (To do, Backlog)
  hgs task move REF --position N            priority in To do or Backlog
  hgs task stage REF todo|backlog|done      done only when the person asks
  hgs task target REF SESSION|any [--host ALIAS]   where it will be sent
  hgs task send REF [--session SESSION [--host ALIAS]]   Send now
  hgs task cancel REF [--reason TEXT]       stop another session's task

Rules
- A report says what changed, how it was checked and what is left undone or
  why it cannot be done; it moves the task to In review.
- You cannot cancel a task you were sent; report what is left undone instead.
- When the person asks to keep something for later, file it with add, not in
  your own plan.
