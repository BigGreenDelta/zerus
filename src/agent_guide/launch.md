Start a session (hgs guide launch)

  hgs [@h] AGENT DIRECTORY --new -n TAG -d --launch-id "$(uuidgen)" -- "first prompt"

- AGENT is claude, codex or kimi; DIRECTORY is an absolute path on that
  computer, or a project name from hgs [@h] project ls.
- -d starts it detached and prints "hgs: started NAME". Without --new, an
  existing session of that name is reused instead.
- Text after -- is the agent's first prompt; resume never replays it.
- A launch that ended early appears in hgs [@h] ls --json under
  ended_launches with its launch_id, exit code and last screen lines.
- A folder the agent has not trusted yet stops at its trust prompt, which
  only the owner may confirm. inspect then shows the session needing attention.

Continue existing work instead of starting over:
  hgs [@h] resume SESSION -d          restore a Saved conversation
  hgs [@h] fork SESSION -n TAG -d     branch into a new conversation
