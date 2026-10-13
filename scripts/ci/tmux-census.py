#!/usr/bin/env python3
"""Golden census of the tmux and ps calls hgs makes, for the Unix compatibility check.

  record --out DIR -- COMMAND...   run COMMAND with recording tmux and ps shims first
                                   on PATH; raw records land in DIR/calls
  summarize DIR... --out FILE      normalize raw records (merged) into a deterministic census
  compare BASE NEW                 report operations whose calls differ; exit 1 if any do

The shims log one record per call and then behave like the real program: same argv,
stdin, stdout, stderr and exit status. Interactive or streaming calls (attach, a
new-session without -d, control mode, a terminal stdin) exec the real program after
logging. Only `load-buffer ... -` has its stdin read (and recorded). Calls are grouped
into operations by their nearest hgs ancestor process. See docs/ci-and-aur.md.
"""
import argparse
import base64
import collections
import difflib
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

FORMAT = 2

# The shim is written into the private shim directory as `tmux` and `ps`. It runs
# under `python -IS` and must stay self-contained and fast.
SHIM = r'''
import base64, json, os, signal, sys, time
CONFIG = __CONFIG__
TOOL = os.path.basename(sys.argv[0])
ARGS = sys.argv[1:]
# getopt templates of tmux (tmux.c) and of the commands whose flags matter here.
GLOBAL, NEW_SESSION, LOAD_BUFFER = "2c:CDdf:hlL:NqS:T:uUvV", "Ac:dDe:EF:f:n:Ps:t:x:Xy:", "b:t:w"


def getopt(args, template):
    """The option letters at the start of args (getopt rules) and how many args they use."""
    letters, index = [], 0
    while index < len(args):
        arg = args[index]
        if arg == "--":
            index += 1
            break
        if not arg.startswith("-") or arg == "-":
            break
        for position, letter in enumerate(arg[1:], 1):
            letters.append(letter)
            if letter + ":" in template:
                if position == len(arg) - 1:
                    index += 1
                break
        index += 1
    return letters, index


def commands(args):
    """The tmux command list split at `;` separators."""
    result, current = [], []
    for arg in args:
        if arg == ";":
            result.append(current)
            current = []
        elif arg.endswith(";") and not arg.endswith("\\;"):
            current.append(arg[:-1])
            result.append(current)
            current = []
        else:
            current.append(arg)
    if current:
        result.append(current)
    return [command for command in result if command]


def is_command(name, full, alias, shortest):
    return name == alias or (len(name) >= shortest and full.startswith(name))


def plan():
    """(exec reason or None, whether stdin is the load-buffer text)."""
    if os.isatty(0):
        return "terminal stdin", False
    if TOOL != "tmux":
        return None, False
    letters, used = getopt(ARGS, GLOBAL)
    rest = ARGS[used:]
    if "C" in letters:
        return "control mode", False
    if "D" in letters:
        return "foreground server", False
    parts = commands(rest)
    if not parts and not ({"V", "c"} & set(letters)):
        return "default new-session", False
    stdin = False
    for part in parts:
        name, tail = part[0], part[1:]
        if is_command(name, "attach-session", "attach", 1):
            return "attach", False
        if is_command(name, "new-session", "new", 5):
            if "d" not in getopt(tail, NEW_SESSION)[0]:
                return "attached new-session", False
        if is_command(name, "load-buffer", "loadb", 3):
            if tail[getopt(tail, LOAD_BUFFER)[1]:][:1] == ["-"]:
                stdin = True
    return None, stdin


def linux_process(pid):
    with open("/proc/%d/stat" % pid, "rb") as handle:
        data = handle.read()
    close = data.rindex(b")")
    fields = data[close + 2:].split()
    entry = dict(pid=pid, ppid=int(fields[1]), start=fields[19].decode(),
                 comm=data[data.index(b"(") + 1:close].decode(errors="replace"))
    try:
        with open("/proc/%d/cmdline" % pid, "rb") as handle:
            raw = handle.read()
        parts = raw.split(b"\0")
        if raw.endswith(b"\0"):
            parts.pop()
        entry["argv"] = [part.decode(errors="replace") for part in parts]
    except OSError:
        entry["argv"] = None
    try:
        exe = os.readlink("/proc/%d/exe" % pid)
        entry["exe"] = exe[:-10] if exe.endswith(" (deleted)") else exe
    except OSError:
        entry["exe"] = None
    return entry


def ancestors():
    """Ancestors from the parent upwards, stopping below the recording process or init."""
    root = int(os.environ.get("ZERUS_TMUX_CENSUS_ROOT") or CONFIG["root"])
    chain, pid = [], os.getppid()
    if os.path.isdir("/proc/self"):
        while pid > 1 and pid != root and len(chain) < 24:
            try:
                entry = linux_process(pid)
            except (OSError, ValueError, IndexError):
                break
            chain.append(entry)
            pid = entry["ppid"]
        return chain
    import subprocess
    table = {}
    listing = subprocess.run([CONFIG["ps"], "-A", "-ww", "-o", "pid=,ppid=,args="],
                             capture_output=True, text=True, errors="replace").stdout
    for line in listing.splitlines():
        parts = line.split(None, 2)
        if len(parts) >= 2 and parts[0].isdigit() and parts[1].isdigit():
            table[int(parts[0])] = (int(parts[1]), parts[2] if len(parts) > 2 else "")
    while pid > 1 and pid != root and pid in table and len(chain) < 24:
        ppid, args = table[pid]
        argv = args.split()
        exe = argv[0] if argv else ""
        chain.append(dict(pid=pid, ppid=ppid, start=None, comm=os.path.basename(exe), exe=exe, argv=argv))
        pid = ppid
    return chain


def is_hgs(entry):
    return entry["comm"] == "hgs" or os.path.basename(entry.get("exe") or "") == "hgs"


def nearest_hgs(chain):
    for entry in chain:
        if is_hgs(entry):
            if entry["start"] is None:
                import subprocess
                entry["start"] = subprocess.run([CONFIG["ps"], "-o", "lstart=", "-p", str(entry["pid"])],
                                                capture_output=True, text=True).stdout.strip()
            return dict(pid=entry["pid"], start=entry["start"], argv=entry["argv"])
    return None


def first_on_path():
    for directory in os.environ.get("PATH", "").split(os.pathsep):
        candidate = os.path.join(directory or ".", TOOL)
        if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    return None


def tmpdir_class():
    value = os.environ.get("TMUX_TMPDIR")
    if value is None:
        return "unset"
    if value == "":
        return "empty"
    return "dir" if os.path.isdir(value) else "missing"


def restore_locale():
    """Undo Python's C-locale coercion (PEP 538). Before any shim code runs, the
    interpreter may set LC_CTYPE=C.UTF-8 in its own environment, which the real
    program would inherit; /proc/self/environ still holds the caller's environment."""
    try:
        with open("/proc/self/environ", "rb") as handle:
            entries = handle.read().split(b"\0")
    except OSError:
        return  # Not Linux: the coercion stays (only when the caller has a C locale).
    original = next((entry[9:] for entry in entries if entry.startswith(b"LC_CTYPE=")), None)
    if original is None:
        os.environ.pop("LC_CTYPE", None)
    elif os.environ.get("LC_CTYPE") != os.fsdecode(original):
        os.environ["LC_CTYPE"] = os.fsdecode(original)


def stdio_class(descriptor):
    """What the caller connected to a standard descriptor: tty, null, pipe, file, ..."""
    import stat
    try:
        mode = os.fstat(descriptor)
    except OSError:
        return "closed"
    if stat.S_ISCHR(mode.st_mode):
        if os.isatty(descriptor):
            return "tty"
        try:
            return "null" if mode.st_rdev == os.stat(os.devnull).st_rdev else "chr"
        except OSError:
            return "chr"
    for name, test in (("pipe", stat.S_ISFIFO), ("socket", stat.S_ISSOCK), ("file", stat.S_ISREG)):
        if test(mode.st_mode):
            return name
    return "other"


def sequence(directory):
    import fcntl
    descriptor = os.open(os.path.join(directory, "seq"), os.O_RDWR | os.O_CREAT, 0o600)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX)
        value = int(os.pread(descriptor, 32, 0) or b"0") + 1
        os.pwrite(descriptor, b"%020d" % value, 0)
    finally:
        os.close(descriptor)
    return value


def save(directory, record):
    path = os.path.join(directory, "calls", "%012d-%s.json" % (record["seq"], TOOL))
    temporary = path + ".%d.tmp" % os.getpid()
    with open(temporary, "w") as handle:
        json.dump(record, handle)
    os.replace(temporary, path)


def write_all(descriptor, data):
    while data:
        data = data[os.write(descriptor, data):]


def default_signals():
    """Python ignores SIGPIPE and SIGXFSZ; the real program starts with the defaults, as
    when hgs (Rust) or Python's subprocess start it."""
    for name in ("SIGPIPE", "SIGXFSZ"):
        if hasattr(signal, name):
            signal.signal(getattr(signal, name), signal.SIG_DFL)


def run(real, stdin):
    """Run the real program as a child; forward stderr live; return (status, stderr)."""
    if stdin is not None:
        if hasattr(os, "memfd_create"):
            feed = os.memfd_create("tmux-census-stdin")
        else:
            name = os.path.join(CONFIG["dir"], "stdin-%d" % os.getpid())
            feed = os.open(name, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o600)
            os.unlink(name)
        write_all(feed, stdin)
        os.lseek(feed, 0, os.SEEK_SET)
    death = None
    if sys.platform.startswith("linux"):
        try:
            import ctypes
            death = ctypes.CDLL(None, use_errno=True).prctl
        except (ImportError, OSError, AttributeError):
            death = None
    parent = os.getpid()
    reader, writer = os.pipe()
    child = os.fork()
    if child == 0:
        try:
            if death is not None:
                death(1, signal.SIGKILL)  # PR_SET_PDEATHSIG: die with the shim, like the real client.
                if os.getppid() != parent:
                    os._exit(1)
            os.dup2(writer, 2)
            if stdin is not None:
                os.dup2(feed, 0)
            default_signals()
            os.execv(real, [TOOL] + ARGS)
        except BaseException:
            os._exit(127)
    os.close(writer)
    if stdin is not None:
        os.close(feed)
    def forward(received, frame):
        try:
            os.kill(child, received)
        except OSError:
            pass
    for number in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP, signal.SIGQUIT):
        signal.signal(number, forward)
    captured, forwarding = [], True
    while True:
        chunk = os.read(reader, 65536)
        if not chunk:
            break
        captured.append(chunk)
        if forwarding:
            try:
                write_all(2, chunk)
            except OSError:
                forwarding = False
    os.close(reader)
    while True:
        try:
            _, status = os.waitpid(child, 0)
            break
        except ChildProcessError:
            status = 0
            break
    return status, b"".join(captured)


def finish(status):
    if os.WIFSIGNALED(status):
        number = os.WTERMSIG(status)
        try:
            signal.signal(number, signal.SIG_DFL)
        except (OSError, ValueError):
            pass
        os.kill(os.getpid(), number)
        os._exit(128 + number)
    os._exit(os.WEXITSTATUS(status) if os.WIFEXITED(status) else 1)


def main():
    restore_locale()
    real = os.environ.get("ZERUS_TMUX_CENSUS_" + TOOL.upper()) or CONFIG[TOOL]
    try:
        if os.path.samefile(real, sys.argv[0]):
            real = CONFIG[TOOL]
    except OSError:
        real = CONFIG[TOOL]
    directory = os.environ.get("ZERUS_TMUX_CENSUS") or CONFIG["dir"]
    reason, wants_stdin = plan()
    stdin = None
    if wants_stdin:
        chunks = []
        while True:
            chunk = os.read(0, 65536)
            if not chunk:
                break
            chunks.append(chunk)
        stdin = b"".join(chunks)
    record = None
    try:
        first = first_on_path()
        try:
            wrapper = first is not None and not os.path.samefile(first, sys.argv[0])
        except OSError:
            wrapper = True
        try:
            cwd = os.getcwd()
        except OSError:
            cwd = None
        chain = ancestors()
        used = getopt(ARGS, GLOBAL)[1] if TOOL == "tmux" else 0
        record = dict(version=2, seq=sequence(directory), tool=TOOL, argv=ARGS, global_args=ARGS[:used],
                      command=ARGS[used:], cwd=cwd, pid=os.getpid(), ppid=os.getppid(),
                      env=dict(TMUX="TMUX" in os.environ, TMUX_value=os.environ.get("TMUX"),
                               TMUX_TMPDIR=tmpdir_class(), TMUX_TMPDIR_value=os.environ.get("TMUX_TMPDIR"),
                               # Locale and time zone change what tmux and ps print (hgs sets
                               # LC_ALL=C TZ=UTC for ps lstart).
                               locale={name: os.environ[name] for name in ("LC_ALL", "LC_CTYPE", "LANG", "TZ")
                                       if name in os.environ}),
                      stdio=[stdio_class(descriptor) for descriptor in (0, 1, 2)],
                      path_first=first, wrapper=first if wrapper else None,
                      hgs=nearest_hgs(chain), chain=chain,
                      mode="exec" if reason else "wrapped", reason=reason,
                      stdin=None if stdin is None else base64.b64encode(stdin).decode(),
                      started=time.monotonic_ns(), finished=None, status=None, signal=None, stderr=None)
        save(directory, record)
    except Exception as error:
        record = None
        try:
            with open(os.path.join(directory, "shim-errors.log"), "a") as handle:
                handle.write("%s %r: %r\n" % (TOOL, ARGS, error))
        except OSError:
            pass
    if reason:
        default_signals()
        os.execv(real, [TOOL] + ARGS)
    status, stderr = run(real, stdin)
    if record is not None:
        try:
            record.update(finished=time.monotonic_ns(),
                          status=os.WEXITSTATUS(status) if os.WIFEXITED(status) else None,
                          signal=os.WTERMSIG(status) if os.WIFSIGNALED(status) else None,
                          stderr=stderr.decode(errors="replace"))
            save(directory, record)
        except Exception:
            pass
    finish(status)


main()
'''


# ---------------------------------------------------------------- record

def git(*args, cwd):
    try:
        return subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True, timeout=30).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def record(arguments):
    out = Path(arguments.out).resolve()
    command = arguments.command
    if command[:1] == ["--"]:
        command = command[1:]
    if not command:
        raise SystemExit("record: missing COMMAND after --")
    if out.exists() and any(out.iterdir()):
        raise SystemExit(f"record: {out} must be new or empty")
    real = {tool: shutil.which(tool) for tool in ("tmux", "ps")}
    for tool, path in real.items():
        if not path:
            raise SystemExit(f"record: no {tool} on PATH")
        real[tool] = os.path.abspath(path)
    (out / "calls").mkdir(parents=True, exist_ok=True)
    shim = out / "shim"
    shim.mkdir(mode=0o700)
    config = dict(dir=str(out), tmux=real["tmux"], ps=real["ps"], root=os.getpid())
    source = f"#!{sys.executable} -IS\n" + SHIM.replace("__CONFIG__", json.dumps(config))
    for tool in ("tmux", "ps"):
        (shim / tool).write_text(source)
        (shim / tool).chmod(0o755)
    env = dict(os.environ)
    env.update(PATH=str(shim) + os.pathsep + env.get("PATH", ""), ZERUS_TMUX_CENSUS=str(out),
               ZERUS_TMUX_CENSUS_TMUX=real["tmux"], ZERUS_TMUX_CENSUS_PS=real["ps"],
               ZERUS_TMUX_CENSUS_ROOT=str(os.getpid()))
    cwd = os.getcwd()
    hgs = env.get("HGS_TEST_BIN")
    meta = dict(format=FORMAT, command=command, cwd=cwd, commit=git("rev-parse", "HEAD", cwd=cwd) or None,
                dirty=bool(git("status", "--porcelain", "--untracked-files=no", cwd=cwd)),
                platform=sys.platform, python=sys.version.split()[0], tmux=real["tmux"], ps=real["ps"],
                tmux_version=subprocess.run([real["tmux"], "-V"], capture_output=True, text=True).stdout.strip(),
                tmpdir=tempfile.gettempdir(), home=os.path.expanduser("~"), hgs_test_bin=hgs,
                hgs_sha256=hashlib.sha256(Path(hgs).read_bytes()).hexdigest() if hgs and Path(hgs).is_file() else None,
                started=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
    (out / "meta.json").write_text(json.dumps(meta, indent=1) + "\n")
    started = time.monotonic()
    try:
        status = subprocess.run(command, env=env).returncode
    except KeyboardInterrupt:
        status = 130
    meta.update(status=status, seconds=round(time.monotonic() - started, 1))
    (out / "meta.json").write_text(json.dumps(meta, indent=1) + "\n")
    calls = sum(1 for path in (out / "calls").glob("*.json"))
    print(f"tmux-census: {calls} calls recorded in {out} ({meta['seconds']} s, command status {status})",
          file=sys.stderr)
    return status


# ---------------------------------------------------------------- normalization

UUID = re.compile(r"[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}")
HEX = re.compile(r"(?<![0-9A-Za-z])[0-9a-f]{32,}(?![0-9A-Za-z])")
ISO_TIME = re.compile(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(?:\.\d+)?(?:Z|[+-]\d\d:?\d\d)?")
LSTART = re.compile(r"(?:Mon|Tue|Wed|Thu|Fri|Sat|Sun) (?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) +\d+ \d\d:\d\d:\d\d \d{4}")
# Names the tempfile crate randomizes: NamedTempFile (.tmpXXXXXX) and hgs prefixes.
RANDOM_NAME = re.compile(r"(?<![A-Za-z0-9])(?:(\.tmp|message-)[A-Za-z0-9]{6}|(work-)[0-9a-f]{8})(?![A-Za-z0-9])")


class Normalizer:
    """Replaces run-specific values with placeholders numbered by first appearance.

    One instance covers one operation, so equal values stay equal and distinct values
    stay distinct within it, while the placeholders do not depend on the run.
    """

    def __init__(self, meta, census_dir):
        self.tables = collections.defaultdict(dict)
        self.literal = []
        hgs = meta.get("hgs_test_bin")
        for value, name in ((str(census_dir), "<CENSUS>"), (hgs, "<HGS>"), (hgs and os.path.realpath(hgs), "<HGS>"),
                            (meta.get("cwd"), "<REPO>"), (meta.get("home"), "<HOME>")):
            if value and len(value) > 1:
                self.literal.append((value.rstrip("/"), name))
        self.literal.sort(key=lambda item: -len(item[0]))
        roots = {meta.get("tmpdir") or "/tmp", "/tmp", "/private/tmp", "/var/tmp"}
        roots = sorted({root.rstrip("/") for root in roots if root}, key=len, reverse=True)
        self.temp = re.compile("(?:" + "|".join(re.escape(root) for root in roots) +
                               r"|(?:/private)?/var/folders/[^/\s]+/[^/\s]+/T)/([^/\s'\";:,)]+)")

    def placeholder(self, kind, value):
        table = self.tables[kind]
        if value not in table:
            table[value] = len(table) + 1
        return f"<{kind}{table[value]}>"

    def __call__(self, text):
        if text is None:
            return None
        for value, name in self.literal:
            text = text.replace(value, name)
        text = self.temp.sub(lambda match: self._temp(match), text)
        text = re.sub(r"\btmux-\d+\b", "tmux-<UID>", text)
        text = RANDOM_NAME.sub(lambda match: (match[1] or match[2]) + self.placeholder("R", match[0]), text)
        text = UUID.sub(lambda match: self.placeholder("UUID", match[0].lower()), text)
        text = HEX.sub(lambda match: self.placeholder("HEX", match[0]), text)
        text = ISO_TIME.sub("<TIME>", text)
        # tmux pane, session and window IDs (%3, $1, @2) stay: every test owns a fresh
        # server, so they repeat exactly between recordings, and a placeholder numbered
        # by first appearance would hide a call that targets another pane.
        return LSTART.sub("<LSTART>", text)

    def _temp(self, match):
        component = match[1]
        if re.fullmatch(r"tmux-\d+", component):
            return "<SYSTMP>/tmux-<UID>"
        return self.placeholder("TMP", component)


def load_records(directory):
    records = []
    for path in sorted((directory / "calls").glob("*.json")):
        try:
            records.append(json.loads(path.read_text()))
        except (OSError, ValueError):
            print(f"tmux-census: skipping unreadable {path}", file=sys.stderr)
    records.sort(key=lambda item: item["seq"])
    return records


def job_of(chain):
    """The test job an operation belongs to, from its ancestors' command lines."""
    for entry in chain or []:
        argv = entry.get("argv") or []
        if "unittest" in argv and "-m" in argv:
            names = [arg for arg in argv[argv.index("unittest") + 1:] if not arg.startswith("-")]
            classes = sorted({".".join(name.split(".")[:2]) for name in names})
            return " ".join(classes) or "unittest"
        for arg in argv[1:3]:
            if arg.endswith(".sh") and "/tests/" in "/" + arg:
                return os.path.basename(arg)
    return None


def socket_of(record):
    """The tmux server socket a call addresses, for attribution only."""
    args = record.get("global_args") or []
    for index, arg in enumerate(args):
        if arg.startswith("-S"):
            return arg[2:] or (args[index + 1] if index + 1 < len(args) else None)
    env = record.get("env") or {}
    label = "default"
    for index, arg in enumerate(args):
        if arg.startswith("-L"):
            label = arg[2:] or (args[index + 1] if index + 1 < len(args) else label)
    if env.get("TMUX_value") and label == "default":
        return env["TMUX_value"].split(",")[0]
    base = env.get("TMUX_TMPDIR_value") or "/tmp"
    return os.path.join(base, "tmux-%d" % os.getuid(), label)


def default_socket(record):
    """Whether a tmux call reaches the recording user's default server: no -S or -L, no
    TMUX, and TMUX_TMPDIR unset, empty or missing (tmux then falls back to /tmp)."""
    env = record.get("env") or {}
    return (record["tool"] == "tmux" and not env.get("TMUX") and env.get("TMUX_TMPDIR") != "dir"
            and not any(arg.startswith(("-S", "-L")) for arg in record.get("global_args") or []))


def ps_arguments(argv, normalize):
    """ps arguments with process IDs replaced: `-p LIST` names PIDs of any length."""
    result, expect = [], False
    for arg in argv:
        value = None
        if expect:
            value, prefix = arg, ""
        elif arg.startswith("-p") and len(arg) > 2 and arg[2:].replace(",", "").isdigit():
            value, prefix = arg[2:], "-p"
        if value is not None and all(part.strip().isdigit() for part in value.split(",")):
            arg = prefix + ",".join(normalize.placeholder("PID", part.strip()) for part in value.split(","))
        else:
            arg = normalize(arg)
        expect = arg in ("-p", "--pid", "-q")
        result.append(arg)
    return result


def shape(record, normalize):
    call = dict(tool=record["tool"])
    if record["tool"] == "tmux":
        call["global"] = [normalize(arg) for arg in record.get("global_args") or []]
        call["argv"] = [normalize(arg) for arg in record.get("command") or []]
    else:
        call["argv"] = ps_arguments(record["argv"], normalize)
    env = record.get("env") or {}
    call["env"] = dict(TMUX=bool(env.get("TMUX")), TMUX_TMPDIR=env.get("TMUX_TMPDIR"),
                       locale={name: normalize(value) for name, value in (env.get("locale") or {}).items()},
                       stdio="/".join(record.get("stdio") or ["?"]))
    if record.get("stdin") is not None:
        raw = base64.b64decode(record["stdin"]).decode(errors="replace")
        text = normalize(raw)
        call["stdin"] = dict(bytes=len(base64.b64decode(record["stdin"])),
                             sha256=hashlib.sha256(text.encode()).hexdigest()[:16],
                             text=text if len(text) <= 2000 else text[:2000] + "...")
    if record.get("mode") == "exec":
        call["result"] = "exec: " + (record.get("reason") or "")
    elif record.get("finished") is None:
        call["result"] = "unfinished"
    elif record.get("signal"):
        call["result"] = f"signal {record['signal']}"
    else:
        call["result"] = f"exit {record['status']}"
    stderr = (record.get("stderr") or "").strip()
    if stderr:
        stderr = normalize(stderr)
        call["stderr"] = stderr if len(stderr) <= 2000 else stderr[:2000] + "..."
    return call


def line(call):
    """A one-line rendering of a call, used for diffs and pattern options."""
    parts = [call["tool"]] + call.get("global", []) + (["|"] if call.get("global") else []) + call["argv"]
    text = " ".join(json.dumps(part, ensure_ascii=False) if (not part or re.search(r"[\s\"'\\]", part)) else part
                    for part in parts)
    env = call["env"]
    locale = "".join(f"; {name}={env['locale'][name]}" for name in ("LC_ALL", "LC_CTYPE", "LANG", "TZ")
                     if name in env.get("locale", {}))
    text += (f"  [{call['result']}; TMUX={'set' if env['TMUX'] else 'unset'}; TMUX_TMPDIR={env['TMUX_TMPDIR']}"
             f"{locale}; stdio={env.get('stdio', '?')}]")
    if "stdin" in call:
        text += f"  stdin={json.dumps(call['stdin']['text'], ensure_ascii=False)}"
    if "stderr" in call:
        text += f"  stderr={json.dumps(call['stderr'], ensure_ascii=False)}"
    return text


def summarize_directory(directory):
    meta = json.loads((directory / "meta.json").read_text())
    records = load_records(directory)
    groups = collections.OrderedDict()
    direct = []
    for item in records:
        if item.get("hgs"):
            groups.setdefault((item["hgs"]["pid"], item["hgs"]["start"]), []).append(item)
        else:
            direct.append(item)
    # Background hgs processes (under a tmux server) have no test ancestor; attribute
    # them to the job whose calls address the same server socket.
    sockets = {}
    for item in records:
        job = job_of(item.get("chain"))
        if job:
            sockets.setdefault(socket_of(item), job)
    operations = []
    for items in groups.values():
        normalize = Normalizer(meta, directory)
        argv = items[0]["hgs"].get("argv") or ["hgs"]
        key = ["hgs"] + [normalize(arg) for arg in argv[1:]]
        job = job_of(items[0].get("chain"))
        background = job is None
        if job is None:
            for item in items:
                job = sockets.get(socket_of(item))
                if job:
                    break
        calls = [shape(item, normalize) for item in items]
        operations.append(dict(job=job or "<unattributed>", background=background, key=key, calls=calls))
    direct_calls = []
    for item in direct:
        normalize = Normalizer(meta, directory)
        job = job_of(item.get("chain")) or sockets.get(socket_of(item)) or "<unattributed>"
        direct_calls.append(dict(job=job, call=shape(item, normalize)))
    run = dict(command=meta.get("command"), commit=meta.get("commit"), dirty=meta.get("dirty"),
               status=meta.get("status"), seconds=meta.get("seconds"), records=len(records),
               record_versions=sorted({item.get("version", 1) for item in records}),
               exec_calls=sum(1 for item in records if item.get("mode") == "exec"),
               unfinished=sum(1 for item in records if item.get("mode") != "exec" and item.get("finished") is None),
               shim_errors=_count_lines(directory / "shim-errors.log"),
               default_socket=sum(1 for item in records if default_socket(item)))
    return meta, run, operations, direct_calls


def summarize(arguments):
    runs, operations, direct = [], [], []
    metas = []
    for name in arguments.directories:
        meta, run, found, calls = summarize_directory(Path(name).resolve())
        metas.append(meta)
        runs.append(run)
        operations += found
        direct += calls

    def order(operation):
        return (operation["job"], json.dumps(operation["key"]), json.dumps(operation["calls"], sort_keys=True))

    operations.sort(key=order)
    direct.sort(key=lambda item: (item["job"], json.dumps(item["call"], sort_keys=True)))
    commits = sorted({str(meta.get("commit")) for meta in metas})
    versions = sorted({version for run in runs for version in run["record_versions"]})
    census = dict(format=FORMAT, record_versions=versions, commit=commits[0] if len(commits) == 1 else commits,
                  tmux_version=metas[0].get("tmux_version"), platform=metas[0].get("platform"),
                  status=max((run["status"] or 0) for run in runs) if runs else None, runs=runs,
                  counts=dict(operations=len(operations), operation_calls=sum(len(op["calls"]) for op in operations),
                              direct_calls=len(direct), records=sum(run["records"] for run in runs),
                              exec_calls=sum(run["exec_calls"] for run in runs),
                              unfinished=sum(run["unfinished"] for run in runs),
                              shim_errors=sum(run["shim_errors"] for run in runs),
                              default_socket=sum(run["default_socket"] for run in runs)),
                  operations=operations, direct=direct)
    Path(arguments.out).write_text(json.dumps(census, indent=1, sort_keys=True, ensure_ascii=False) + "\n")
    counts = census["counts"]
    print(f"tmux-census: {counts['operations']} operations, {counts['operation_calls']} calls in them, "
          f"{counts['direct_calls']} direct calls -> {arguments.out}", file=sys.stderr)
    if counts["default_socket"]:
        print(f"tmux-census: warning: {counts['default_socket']} tmux calls addressed the default socket of the "
              "recording user (no -S, -L, TMUX or existing TMUX_TMPDIR): a test is not isolated", file=sys.stderr)
    return 0


def _count_lines(path):
    try:
        return sum(1 for _ in path.open())
    except OSError:
        return 0


# ---------------------------------------------------------------- compare

RULE_KINDS = ("replace", "optional", "collapse", "unordered", "any_count")


class Rule:
    """One comparison rule: operations whose job and key match, and the calls it covers."""

    def __init__(self, kind, key=".*", job=".*", calls=".*", why="", with_=None, **extra):
        if extra:
            raise SystemExit(f"compare: unknown fields in a {kind} rule: {sorted(extra)}")
        self.kind, self.why = kind, why
        self.key, self.job, self.calls = re.compile(key), re.compile(job), re.compile(calls)
        self.replacement = with_
        if (kind == "replace") != (with_ is not None):
            raise SystemExit(f"compare: only replace rules take (and need) \"with\": {kind} {calls!r}")

    def applies(self, job, key):
        return bool(self.job.search(job) and self.key.search(key))


def load_rules(arguments):
    rules = {kind: [] for kind in RULE_KINDS}
    for path in arguments.rules or []:
        data = json.loads(Path(path).read_text())
        unknown = set(data) - set(RULE_KINDS) - {"comment"}
        if unknown:
            raise SystemExit(f"compare: unknown rule kinds in {path}: {sorted(unknown)}")
        for kind in RULE_KINDS:
            for entry in data.get(kind, []):
                entry = dict(entry)
                if "with" in entry:
                    entry["with_"] = entry.pop("with")
                rules[kind].append(Rule(kind, **entry))
    for kind in ("collapse", "unordered", "any_count", "optional"):
        for pattern in getattr(arguments, kind) or []:
            rules[kind].append(Rule(kind, calls=pattern) if kind in ("collapse", "optional") else Rule(kind, key=pattern))
    return rules


def collapse(lines, pattern, longest=8):
    """Merge back-to-back repeats of a block of calls (a poll loop) into one occurrence.

    Every call of the block must match the pattern. One iteration and many compare equal.
    """
    changed = True
    while changed:
        changed = False
        for period in range(1, longest + 1):
            result, position = [], 0
            while position < len(lines):
                block = lines[position:position + period]
                if len(block) == period and all(pattern.search(text) for text in block):
                    end = position + period
                    while lines[end:end + period] == block:
                        end += period
                    if end > position + period:
                        result += block
                        position = end
                        changed = True
                        continue
                result.append(lines[position])
                position += 1
            lines = result
    return lines


def render_operation(job, operation, rules, dropped=None):
    """The operation's calls as lines, with the comparison rules applied; None when optional.

    The calls of an optional operation that do not match the rule go to `dropped`, so a
    call shape that occurs only there still counts as present on that side.
    """
    key = " ".join(operation["key"])
    lines = [line(call) for call in operation["calls"]]
    for rule in rules["replace"]:
        if rule.applies(job, key):
            lines = [rule.calls.sub(rule.replacement, text) for text in lines]
    for rule in rules["optional"]:
        if rule.applies(job, key) and any(rule.calls.search(text) for text in lines):
            if dropped is not None:
                dropped.update(text for text in lines if not rule.calls.search(text))
            return None
    for rule in rules["collapse"]:
        if rule.applies(job, key):
            lines = collapse(lines, rule.calls)
    for rule in rules["unordered"]:
        if rule.applies(job, key):
            lines = sorted(set(lines))
    return tuple(lines)


def index(census, rules, ignore_jobs):
    """Rendered call sequences counted per (job, key), and the call shapes of optional operations."""
    groups, any_count, dropped = collections.defaultdict(collections.Counter), set(), set()
    for operation in census["operations"]:
        key = " ".join(operation["key"])
        group = ("*" if ignore_jobs else operation["job"], key)
        rendered = render_operation(operation["job"], operation, rules, dropped)
        if rendered is not None:
            groups[group][rendered] += 1
        if any(rule.applies(operation["job"], key) for rule in rules["any_count"]):
            any_count.add(group)
    for group in any_count & set(groups):
        for sequence in groups[group]:
            groups[group][sequence] = 1
    return groups, dropped


def compare(arguments):
    base = json.loads(Path(arguments.base).read_text())
    new = json.loads(Path(arguments.new).read_text())
    formats = [(census.get("format"), census.get("record_versions")) for census in (base, new)]
    if formats[0] != formats[1]:
        print(f"tmux-census: cannot compare census format/record versions {formats[0]} with {formats[1]}; "
              "record and summarize both trees with the same tool", file=sys.stderr)
        return 2
    rules = load_rules(arguments)
    (left, left_dropped), (right, right_dropped) = (index(census, rules, arguments.ignore_jobs)
                                                    for census in (base, new))
    differences, out = 0, []
    for group in sorted(set(left) | set(right)):
        old, current = left.get(group, collections.Counter()), right.get(group, collections.Counter())
        if old == current:
            continue
        differences += 1
        job, key = group
        removed, added = old - current, current - old
        out.append(f"== operation {key!r} in {job}: {sum(old.values())} in base, {sum(current.values())} in new")
        if not old or not current:
            out.append("   present only in " + ("new" if not old else "base"))
        paired = set()
        for sequence, count in sorted(removed.items()):
            partner = _closest(sequence, list(added))
            if partner is None and len(removed) == 1 and len(added) == 1:
                partner = next(iter(added))
            if partner is None:
                out.append(f"   - {count}x sequence only in base:")
                out += ["       " + text for text in sequence]
            else:
                paired.add(partner)
                out.append(f"   ~ {count}x base sequence vs {added[partner]}x closest new sequence"
                           f" ({', '.join(_aspects(sequence, partner))}):")
                out += ["     " + text for text in difflib.unified_diff(list(sequence), list(partner), "base", "new",
                                                                         lineterm="", n=2)][2:]
        for sequence, count in sorted(added.items()):
            if sequence not in paired:
                out.append(f"   + {count}x sequence only in new:")
                out += ["       " + text for text in sequence]
    # Call shapes present on one side only, independent of grouping and counts, including
    # the calls of optional operations that did not match the rule.
    def shapes(groups, dropped):
        return {text for counter in groups.values() for sequence in counter for text in sequence} | dropped
    old_shapes, new_shapes = shapes(left, left_dropped), shapes(right, right_dropped)
    only_base, only_new = sorted(old_shapes - new_shapes), sorted(new_shapes - old_shapes)
    if only_base or only_new:
        differences += 1
        out.append("== call shapes present on one side only")
        out += ["   - " + text for text in only_base]
        out += ["   + " + text for text in only_new]
    if arguments.include_direct:
        def direct(census):
            return collections.Counter((("*" if arguments.ignore_jobs else item["job"]), line(item["call"]))
                                       for item in census["direct"])
        old, current = direct(base), direct(new)
        if old != current:
            differences += 1
            out.append("== direct calls (no hgs ancestor)")
            out += [f"   - {count}x [{job}] {text}" for (job, text), count in sorted((old - current).items())]
            out += [f"   + {count}x [{job}] {text}" for (job, text), count in sorted((current - old).items())]
    for name, census in (("base", base), ("new", new)):
        counts = census.get("counts", {})
        if counts.get("shim_errors") or counts.get("unfinished") or census.get("status") not in (0, None):
            out.append(f"note: {name} run status {census.get('status')}, shim errors {counts.get('shim_errors')}, "
                       f"unfinished calls {counts.get('unfinished')}")
    lines = "\n".join(out).splitlines()
    if arguments.max_lines and len(lines) > arguments.max_lines:
        lines = lines[:arguments.max_lines] + [f"... {len(lines) - arguments.max_lines} more lines"]
    if lines:
        print("\n".join(lines))
    active = sum(len(items) for items in rules.values())
    print(f"tmux-census: {differences} difference group(s) between {base.get('commit')} and {new.get('commit')}"
          f" ({base['counts']['operations']} vs {new['counts']['operations']} operations, {active} rules)",
          file=sys.stderr)
    return 1 if differences else 0


LINE = re.compile(r"^(?P<argv>.*?)  \[(?P<result>[^;\]]*); (?P<env>TMUX=[^\]]*?); stdio=(?P<stdio>[^\]]*)\]"
                  r"(?:  stdin=(?P<stdin>.*?))?(?:  stderr=(?P<stderr>.*))?$")


def _aspects(old, new):
    """What differs between two call sequences: call order or count, argv, result, env, stdio, stdin, stderr."""
    if sorted(old) == sorted(new):
        return ["order"] if old != new else ["count"]
    found = []
    for tag, first, last, start, end in difflib.SequenceMatcher(None, old, new, autojunk=False).get_opcodes():
        if tag == "delete":
            found.append("calls only in base")
        elif tag == "insert":
            found.append("calls only in new")
        elif tag == "replace":
            if last - first != end - start:
                found.append("calls differ")
            for left, right in zip(old[first:last], new[start:end]):
                a, b = LINE.match(left), LINE.match(right)
                if not a or not b:
                    found.append("calls differ")
                    continue
                found += [part for part in ("argv", "result", "env", "stdio", "stdin", "stderr") if a[part] != b[part]]
    return list(dict.fromkeys(found)) or ["count"]


def _closest(sequence, candidates):
    best, score = None, 0.0
    for candidate in candidates:
        ratio = difflib.SequenceMatcher(None, sequence, candidate, autojunk=False).ratio()
        if ratio > score:
            best, score = candidate, ratio
    return best if score >= 0.5 else None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="action", required=True)
    recording = commands.add_parser("record", help="run COMMAND with recording shims")
    recording.add_argument("--out", required=True, help="new or empty directory for the raw records")
    recording.add_argument("command", nargs=argparse.REMAINDER, help="-- COMMAND [ARGS...]")
    summary = commands.add_parser("summarize", help="normalize raw records into a census file")
    summary.add_argument("directories", nargs="+", metavar="DIR", help="record directories, merged in one census")
    summary.add_argument("--out", required=True)
    comparison = commands.add_parser("compare", help="compare two census files")
    comparison.add_argument("base")
    comparison.add_argument("new")
    comparison.add_argument("--rules", action="append", metavar="FILE",
                            help="JSON rules for measured nondeterminism (scripts/ci/tmux-census-rules.json)")
    comparison.add_argument("--collapse", action="append", metavar="REGEX",
                            help="merge back-to-back repeats of call blocks whose lines all match (poll loops)")
    comparison.add_argument("--unordered", action="append", metavar="REGEX",
                            help="compare the calls of operations whose key matches as a set")
    comparison.add_argument("--any-count", action="append", metavar="REGEX", dest="any_count",
                            help="ignore how often a distinct sequence occurs for operation keys that match")
    comparison.add_argument("--optional", action="append", metavar="REGEX",
                            help="drop operations with a call that matches (a background process raced teardown)")
    comparison.add_argument("--ignore-jobs", action="store_true", help="group operations by key only")
    comparison.add_argument("--include-direct", action="store_true",
                            help="also compare calls without an hgs ancestor (test fixtures)")
    comparison.add_argument("--max-lines", type=int, default=400, help="truncate the report (0: no limit)")
    arguments = parser.parse_args()
    return dict(record=record, summarize=summarize, compare=compare)[arguments.action](arguments)


if __name__ == "__main__":
    sys.exit(main())
