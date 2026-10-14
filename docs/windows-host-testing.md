# Testing the Windows session host

The Windows session host (`hgs.exe __host serve`, spec
[Windows Phase 2](superpowers/specs/2026-10-13-windows-native-host-design.md))
owns the ConPTY sessions of one user and state directory. This page shows how to
run a host that touches nothing of your own, how to run its tests, and the rules
new survival and contract tests follow. Commands are for Git Bash in the checkout,
as in [CONTRIBUTING](../CONTRIBUTING.md#windows).

## Who may run a host

A host serves only when local sessions are enabled (the `local-sessions` marker
that `hgs session-host install` writes) and only from the staged host image
(`%USERPROFILE%\.local\share\hgs\host\<hash>\hgs.exe`, named by the pointer
`current`). `hgs __host call --start` starts that image. Tests run the build under
test instead, with two variables that count only for an *isolated* state
directory, one that `HGS_STATE_DIR` names and that is not this user's default
(`XDG_STATE_HOME\hgs\sessions` or `~\.local\state\hgs\sessions`):

- `HGS_HOST_ALLOW_UNSTAGED=1`: the host serves without the marker and from any
  executable, and creates sessions without the marker.
- `HGS_HOST_IMAGE=<absolute path of an hgs.exe>`: `__host call --start` starts that
  executable. Any build works, so a test can pair one CLI with another host.

For any other state directory the first is ignored and the second is an error.
`HGS_STATE_DIR` alone proves nothing, as every hgs process exports it to its
children. The rules live in `src/host/gate.rs`.

`__host serve` exits with 3 when another host holds the state directory, 4 when
local sessions are not enabled, 5 when it does not run from a staged image and 1
on other failures; `__host call --start` reports these as errors.

## An isolated host by hand

Run this in a new Git Bash window and close it afterwards: it points `HOME` and
`USERPROFILE` at a temporary folder.

```sh
cargo build --locked
export MSYS_NO_PATHCONV=1
unset $(compgen -e | grep -E '^(HGS_|XDG_|TMUX)')
root="$(mktemp -d)"
mkdir "$root/state" "$root/home" "$root/work"
export HGS_STATE_DIR="$(cygpath -w "$root/state")"
export USERPROFILE="$(cygpath -w "$root/home")" HOME="$(cygpath -w "$root/home")"
export HGS_HOST_IMAGE="$(cygpath -w "$PWD/target/debug/hgs.exe")" HGS_HOST_ALLOW_UNSTAGED=1
hgs=target/debug/hgs.exe
create() {  # create <launch id> <session name>: a cmd.exe session in $root/work
    python -c 'import json, os, sys; print(json.dumps({"op": "create_session",
        "launch_id": sys.argv[1], "name": sys.argv[2], "cwd": sys.argv[3],
        "env": [f"{k}={v}" for k, v in os.environ.items()],
        "argv": [os.path.join(os.environ["SystemRoot"], "System32", "cmd.exe"), "/d", "/q", "/k"],
        "size": {"cols": 100, "rows": 30}}))' "$1" "$2" "$(cygpath -w "$root/work")"
}

"$hgs" __host call --start "$(create demo-1 cmd/demo)"  # starts the host, then the session
"$hgs" __host status                                   # hello (pid, image, in_job) and counts
"$hgs" __host call '{"op":"list"}'
"$hgs" __host call '{"op":"send_keys","target":{"pane":"%0"},"keys":[{"literal":"echo hello"},{"named":"Enter"}]}'
"$hgs" __host call '{"op":"capture","target":{"pane":"%0"},"styled":false}'
"$hgs" __host attach '{"target":{"pane":"%0"},"size":{"cols":100,"rows":30},"run_id":null}' \
    '[{"wait_ms":500},{"send":"echo attached\r"},{"wait_ms":500},{"detach":true}]'
"$hgs" __host call '{"op":"kill","target":{"pane":"%0"}}'
"$hgs" __host status        # {"running": false} about 5 s after the last session ended
cat "$root/state/host.log"  # starts, refusals, session ends and exits
```

`__host call` prints the response and exits with 0 (`ok`), 1 (`error`), 3 (no host
runs) or 4 (the host cannot be reached or started); without `--start` it never
starts a host. `{"op":"stop_host"}` stops an idle host at once.

Once that host has exited, the same window can enable local sessions as a user
would, in its temporary profile, and start the staged image instead:

```sh
unset HGS_HOST_IMAGE HGS_HOST_ALLOW_UNSTAGED
"$hgs" session-host install                            # stable CLI, host image, marker
"$hgs" __host call --start "$(create demo-2 cmd/staged)"
"$hgs" session-host status                             # running, with its PID and image
"$hgs" session-host stop                               # refused while a session runs
"$hgs" __host call '{"op":"kill","target":{"name":"cmd/staged"}}'
"$hgs" session-host uninstall                          # once the session has ended
"$hgs" session-host status                             # not-installed once the host exited
rm -rf "$root"
```

Remove the folder only after `__host status` reports no host: a running host keeps
files open in its state directory and image.

## Local sessions through the CLI

With local sessions enabled, `hgs` runs this computer's sessions in the host
(spec D3, D8; `src/state/backend/host.rs`). A launch starts the host from the
staged image when none runs; a listing never does, and only "no host" lists no
sessions. Each session's root process is the stable CLI's `hgs.exe --run`, which
runs the program as its child (PowerShell for `hgs sh`), with the launching
process's environment minus the variables of a session it runs in, plus the
launch's own; the launching side sets `HGS_EXECUTABLE` to the stable CLI and the
host sets `HGS_HOST_SESSION`, `HGS_HOST_PANE` and `TERM`. Programs are found with
`PATHEXT`; an npm command shim runs Node.js on its script, and any other batch
file is refused. Tracked Claude Code, Codex and Kimi sessions are refused until
their Windows hook form is settled (spec D12; `HGS_TRACKING=0` runs them
untracked). `hgs a` and a launch without `-d` attach the console they run in
([below](#attaching-a-terminal)). In a new Git Bash window, as above:

```sh
cargo build --locked && cargo build --locked --example fake_agent
export MSYS_NO_PATHCONV=1
unset $(compgen -e | grep -E '^(HGS_|XDG_|TMUX)')
root="$(mktemp -d)"
mkdir -p "$root/state" "$root/home" "$root/work/project" "$root/bin"
cp target/debug/examples/fake_agent.exe "$root/bin/"
export HGS_STATE_DIR="$(cygpath -w "$root/state")" HGS_TAB=0
export USERPROFILE="$(cygpath -w "$root/home")" HOME="$(cygpath -w "$root/home")"
export PATH="$root/bin:$PATH"
hgs=target/debug/hgs.exe project="$(cygpath -w "$root/work/project")"
"$hgs" session-host install                           # marker, stable CLI, host image
"$hgs" ls --json --local                              # no sessions, no host started
"$hgs" fake_agent "$project" --new -n one -d -- --prompt 'fake> '
"$hgs" ls --json --local                              # fake_agent/project/one
"$hgs" __host backend '{"op":"capture","pane":"%0"}'  # the agent's screen
"$hgs" __host backend '{"op":"send","pane":"%0","keys":[{"literal":"hi"},{"named":"Enter"}]}'
"$hgs" rename fake_agent/project/one fake_agent/project/two
"$hgs" --dry-run sh "$project" -n ps -d              # the create request, not sent
"$hgs" kill fake_agent/project/two
"$hgs" __host status                                  # {"running": false} once idle
rm -rf "$root"
```

`hgs __host backend '<operation>'` runs one operation of the terminal backend
(`listing`, `panes`, `pane_info`, `capture`, `send`, `paste`, `set_run_marker`,
`rename`, `kill_session`, `kill_pane`, `session_exists`, `session_name`,
`input_state`, `request_exit`, `on_terminal`; see `src/host/command.rs`) and prints
`{"ok": ...}` or `{"error": ...}`. It reaches only a running host.

## The supervisor channel

Every session's `hgs.exe --run` registers a channel with its host before it starts
its program (spec D10, D11; `src/host/supervisor.rs`). Through it the host reads
the console evidence that input checks judge (the input mode, the attached
processes and whether the supervisor's agent runs inside the session job) and asks
for a pause: Ctrl+Break for the supervisor's process group, which its agent
shares. Terminate pauses first and closes the console after 10 s. `host.log` notes
each session's end with its supervisor's exit code (130 after a pause that ended
the agent with `STATUS_CONTROL_C_EXIT`). In a window prepared as above, before its
`rm -rf`:

```sh
ready="$(cygpath -w "$root/ready.json")"
"$hgs" fake_agent "$project" --new -n three -d -- --ready "$ready" --prompt 'fake> '
until [ -s "$root/ready.json" ]; do sleep 0.2; done
pid=$(python -c 'import json, sys; print(json.load(open(sys.argv[1]))["pid"])' "$ready")
pane=$("$hgs" __host call '{"op":"resolve_target","target":{"name":"fake_agent/project/three"}}' |
    python -c 'import json, sys; print(json.load(sys.stdin)["ok"]["pane"])')
"$hgs" __host backend '{"op":"set_run_marker","name":"fake_agent/project/three","run_id":"run-1"}'
"$hgs" __host backend '{"op":"input_state","pane":"'"$pane"'","name":"fake_agent/project/three","run_id":"run-1","agent":"fake_agent","pid":'"$pid"'}'
ends=$(grep -c ' ended: ' "$root/state/host.log")
"$hgs" __host backend '{"op":"request_exit","pane":"'"$pane"'","pid":'"$pid"'}'  # the pause
until [ "$(grep -c ' ended: ' "$root/state/host.log")" -gt "$ends" ]; do sleep 0.2; done
grep ' ended: ' "$root/state/host.log" | tail -1  # session $N ended: exit 130 (0x00000082)
```

With recovery enabled (`hgs recovery set`), `hgs recovery get` asks a running host
to supervise the stable CLI's `hgs.exe recovery worker` (D14); without a host
nothing starts one.

## Attaching a terminal

`hgs a SESSION` and a launch without `-d` attach the console they run in to the
session (spec D13; `src/host/attach.rs`), as the GUI's embedded terminal runs
`hgs.exe a NAME --existing --run-id ID` on a pseudoconsole of its own. Standard
input and output must both be a console (Windows Terminal, the console host, a
pseudoconsole). The mintty window that the Git Bash shortcut opens gives pipes,
which are refused before anything is created or attached (`attaching needs a
console`, exit 1): use Git Bash inside Windows Terminal, or `winpty`. The client
gets a repaint of the pane, then its output as it comes, written with
`WriteConsoleW`; the console keeps its own scrollback, as there is no alternate
screen around the session. The pane takes the size of the client that was
active last, and an attached launch starts the session at the console's size.

| Keys | What happens |
|---|---|
| `Ctrl+B d`, `Ctrl+B Ctrl+D` | detach: `[detached (from session NAME)]`, exit 0 |
| `Ctrl+B Ctrl+B` | one Ctrl+B reaches the agent (tmux `send-prefix`) |
| `Ctrl+B`, then any other key | both reach the agent unchanged |

Keys inside a bracketed paste are never bindings, and ESC with Ctrl+B in one read
is Meta+Ctrl+B. Ctrl+C reaches the agent as a key; Ctrl+Break is ignored. At the
end the client resets what the session may have left on (mouse, paste, focus and
keypad modes, the alternate screen, a hidden cursor), restores the console modes
it found and moves below the last row. The session's end prints `[exited]` (exit
0). The host's death prints `hgs: the session host died; terminal restored,
sessions on this box are gone` (exit 1), as the tmux backend reports a dead
server. `--existing` never creates or resumes a session, and `--run-id` refuses a
changed run (`session run changed; refresh before reconnecting`, exit 1). While a
client is attached, `hgs ls --json` lists it in `clients` by an opaque ID such as
`c1`; the local box also carries `local_host` (`state`: `not-installed`,
`stopped`, `running` or `unavailable`; `pid`, `protocol`, `conpty`, `integrity`
and `windows_session` of a running host, null otherwise). In a window prepared as
above that is a console, before its `rm -rf`:

```sh
"$hgs" fake_agent "$project" --new -n four -d -- --prompt 'fake> '
"$hgs" a fake_agent/project/four       # type, then Ctrl+B d
"$hgs" ls --json --local               # local_host; clients while attached
"$hgs" fake_agent "$project" --new -n five -- --prompt 'fake> '  # created and attached
```

## Running the tests

```sh
cargo test --locked -- host:: state::backend state::control_console_tests platform::windows::tests </dev/null
cargo build --locked
cargo build --locked --example fake_agent
HGS_TEST_BIN="$(cygpath -w "$PWD/target/debug/hgs.exe")" cargo test --locked -- host::attach </dev/null
python -m unittest discover -s tests -p test_windows_host.py -v </dev/null
python -m unittest discover -s tests -p test_windows_host.py -v -k Staged </dev/null
python -m unittest discover -s tests -p test_windows_lifecycle.py -v </dev/null
```

The Rust filter `host::supervisor` includes
`a_supervisor_reports_its_console_and_pauses_its_group`, which runs the test binary
as a supervisor on a pseudoconsole of an in-process host and its raw-input agent.
`host::attach::console_tests::hgs_attaches_inside_a_pseudoconsole` runs the
`hgs.exe` that `HGS_TEST_BIN` names (it is skipped without it, as a unit test
cannot tell whether a build it finds is current) as `hgs a` on a pseudoconsole of
the test against an isolated host of that build: the repaint, keys and the prefix
bindings, a resize, `--existing --run-id`, the session's end and the host's
death, and the console modes before, during and after each attach.
`tests/test_windows_host.py` and `tests/test_windows_lifecycle.py` take the build
from `HGS_TEST_BIN` (default `target/debug/hgs.exe`) and the fake agent from
`HGS_TEST_FAKE_AGENT` (default `target/debug/examples/fake_agent.exe`); their fake
agent cases skip without one. The lifecycle cases install into a temporary profile
as `Staged` does and launch from the build under test, which plays the portable
copy: sessions must still run the staged stable CLI. Their `Attach` cases run
`hgs a` and an attached launch on a pseudoconsole of the test (`PseudoConsole`),
and `Listings` checks `local_host` in the local box.

## Writing host tests

Subclass `HostCase` in `tests/test_windows_host.py`. Each case then has its own
`HGS_STATE_DIR`, `USERPROFILE`, `HOME` and work folder below a temporary folder,
runs the build under test as an isolated test host, and stops what it started:
it kills its sessions, stops the host, waits for every host process it saw through
a handle opened while that process ran (never by PID alone), and ends one that
does not exit. Its helpers are `call`, `ok`, `error`, `create`, `cmd`, `capture`,
`info`, `keys`, `attach`, `status` and `wait_for`. `Staged` shows a case that
drops the test variables and installs into its temporary profile.

- Never point a case at your own profile or state directory, and never start a
  host without an isolated `HGS_STATE_DIR`.
- Start every process with `CREATE_NO_WINDOW` and give every wait a deadline.
- Use unique session names and launch IDs (`self.unique`); a host keeps a launch ID
  for a while after its session ended.
- A survival test that starts the host from inside a job (as sshd does) checks
  `in_job` in the hello: the host must have broken away. A job that forbids
  breakaway makes `__host call --start` fail instead of starting a host that would
  end with that job.
