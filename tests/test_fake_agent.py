#!/usr/bin/env python3
"""The native fake agent (examples/fake_agent.rs) in a private tmux socket; no real agents.

Build it with `cargo build --locked --example fake_agent`; HGS_TEST_FAKE_AGENT
names another build. Windows drives it under the session host instead.
"""
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

REPO = Path(__file__).resolve().parents[1]
TMUX = shutil.which("tmux")
FAKE_AGENT = Path(os.environ.get("HGS_TEST_FAKE_AGENT", REPO / "target/debug/examples/fake_agent")).resolve()


def running(pid):
    result = subprocess.run(["ps", "-p", str(pid), "-o", "stat="], capture_output=True, text=True, timeout=5)
    return bool(result.stdout.strip()) and not result.stdout.strip().startswith(("Z", "X"))


@unittest.skipIf(sys.platform == "win32", "Windows drives the fake agent under the session host")
class Fixture(unittest.TestCase):
    def setUp(self):
        if not FAKE_AGENT.is_file():
            raise AssertionError(f"{FAKE_AGENT} is missing; run cargo build --locked --example fake_agent")
        self.temp = tempfile.TemporaryDirectory(prefix="hgs-fake-agent-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.record = self.root / "record.jsonl"
        self.ready = self.root / "ready.json"
        # Nothing from a surrounding session, agent or fake agent configuration.
        self.env = {key: value for key, value in os.environ.items()
                    if not key.startswith(("HGS_", "FAKE_AGENT_", "TMUX", "CLAUDE_", "CODEX_", "KIMI_"))}
        self.env["HOME"] = str(self.root)

    def events(self, kind=None):
        # A line still being appended has no newline yet.
        lines = self.record.read_text().split("\n")[:-1] if self.record.exists() else []
        events = [json.loads(line) for line in lines]
        return [event for event in events if kind is None or event["kind"] == kind]

    def tokens(self):
        return [event for event in self.events() if "read" in event]

    def wait_for(self, predicate, timeout=5):
        deadline = time.monotonic() + timeout
        while not predicate():
            if time.monotonic() > deadline:
                self.fail("fake agent fixture timed out; record:\n" + "\n".join(map(json.dumps, self.events())))
            time.sleep(.02)


class PipedAgent(Fixture):
    def run_agent(self, *options, stdin=b"", keep_open=False):
        """Run without a terminal; with keep_open its input never ends."""
        reader, writer = os.pipe()
        try:
            agent = subprocess.Popen([str(FAKE_AGENT), "--record", str(self.record), *options], stdin=reader,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.env)
            os.close(reader)
            reader = None
            os.write(writer, stdin)
            if not keep_open:
                os.close(writer)
                writer = None
            try:
                out, err = agent.communicate(timeout=10)
            finally:
                if agent.poll() is None:
                    agent.kill()
                    agent.wait()
        finally:
            for fd in (reader, writer):
                if fd is not None:
                    os.close(fd)
        return agent.returncode, out, err

    @staticmethod
    def end(agent):
        if agent.poll() is None:
            agent.kill()
            agent.wait()
        if agent.stdin and not agent.stdin.closed:
            agent.stdin.close()

    def test_end_of_input_exits_with_the_configured_code_after_recording_every_byte(self):
        sent = b"ab\xc3\xa9\r\x1b[200~x\ny\x1b[201~\x1b[A\x7f"
        code, out, _ = self.run_agent("--on-eof", "exit:5", "--modes", "paste,alt", stdin=sent)
        self.assertEqual(code, 5)
        start, *_ = self.events("start")
        self.assertEqual((start["tty"], start["raw"], start["name"]), (False, False, "fake_agent"))
        tokens = self.tokens()
        self.assertEqual(bytes.fromhex("".join(token["hex"] for token in tokens)), sent)
        self.assertEqual([(token["kind"], token.get("text"), token.get("name")) for token in tokens],
                         [("text", "ab\u00e9", None), ("enter", None, None), ("paste", "x\ny", None),
                          ("key", "\x1b[A", None), ("control", None, "backspace")])
        exit_event = self.events("exit")[-1]
        self.assertEqual((exit_event["code"], exit_event["cause"]), (5, "eof"))
        # Modes are set in order and reset in reverse order on exit.
        self.assertEqual(out, b"\x1b[?2004h\x1b[?1049h\x1b[?1049l\x1b[?2004l")

    def test_replies_are_matched_to_queries_and_typed_escape_stays_text(self):
        sent = b"\x1b[5;1R\x1b[?1;2c\x1b[5;1R\x1bPlease\r\x1b]11;rgb:0/0/0\x07"
        code, out, _ = self.run_agent("--query", "dsr,da1", "--query-wait", "2000", "--modes", "none", stdin=sent)
        self.assertEqual((code, out), (0, b"\x1b[6n\x1b[c"))
        tokens = self.tokens()
        self.assertEqual(bytes.fromhex("".join(token["hex"] for token in tokens)), sent)
        # A second answer is flagged; ESC before typed text is no device string.
        self.assertEqual([(token["kind"], token.get("query"), token.get("unsolicited"), token.get("text"))
                          for token in tokens],
                         [("reply", "dsr", None, "\x1b[5;1R"), ("reply", "da1", None, "\x1b[?1;2c"),
                          ("reply", "dsr", True, "\x1b[5;1R"), ("esc", None, None, None),
                          ("text", None, None, "Please"), ("enter", None, None, None),
                          ("reply", "osc11", True, "\x1b]11;rgb:0/0/0\x07")])

    def test_unanswered_queries_can_stop_the_start(self):
        code, out, _ = self.run_agent("--ready", str(self.ready), "--query", "dsr,da1", "--query-wait", "100",
                                      "--query-fail-code", "3", "--modes", "none", keep_open=True)
        self.assertEqual(code, 3)
        self.assertEqual(out, b"\x1b[6n\x1b[c")
        self.assertEqual(self.events("query-timeout")[0]["pending"], ["dsr", "da1"])
        self.assertEqual(self.events("exit")[-1]["cause"], "query-timeout")
        self.assertFalse(self.ready.exists())

    def test_end_of_input_keeps_a_scheduled_signal_exit_or_waits_for_one(self):
        for options, number, expected in [(["--on-signal", "TERM=delay:500:9"], signal.SIGTERM, (9, "signal:TERM")),
                                          (["--on-eof", "ignore"], signal.SIGINT, (130, "signal:INT"))]:
            with self.subTest(options=options):
                self.record.unlink(missing_ok=True)
                agent = subprocess.Popen([str(FAKE_AGENT), "--record", str(self.record), *options],
                                         stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, env=self.env)
                self.addCleanup(self.end, agent)
                self.wait_for(lambda: self.events("start"))
                if number == signal.SIGTERM:
                    os.kill(agent.pid, number)
                    self.wait_for(lambda: self.events("signal"))
                agent.stdin.close()
                self.wait_for(lambda: self.events("eof"))
                if number == signal.SIGINT:
                    time.sleep(.1)
                    self.assertIsNone(agent.poll())
                    os.kill(agent.pid, number)
                self.assertEqual(agent.wait(timeout=5), expected[0])
                exit_event = self.events("exit")[-1]
                self.assertEqual((exit_event["code"], exit_event["cause"]), expected)
                if number == signal.SIGTERM:
                    # The agent's own clock: the end of input did not shorten the delay.
                    self.assertGreaterEqual(exit_event["t"] - self.events("signal")[0]["t"], 0.5)

    def test_bad_options_are_refused(self):
        for options, message in [(["--bogus", "1"], "unknown option --bogus"),
                                 (["--on-signal", "TERM=sometimes"], "unknown policy"),
                                 (["--on-eof", "default"], "the end of input has no default"),
                                 (["--modes", "paste,blink"], "unknown mode"),
                                 (["stray"], "agent arguments follow --")]:
            with self.subTest(options=options):
                code, _, err = self.run_agent(*options)
                self.assertEqual(code, 2)
                self.assertIn(message, err.decode())


@unittest.skipUnless(TMUX, "tmux is required")
class TerminalAgent(Fixture):
    def setUp(self):
        super().setUp()
        # An explicit socket path never falls back to the user's server.
        self.socket = self.root / "tmux.sock"
        self.control_path = self.root / "control"
        self.pids = []
        self.addCleanup(self.stop_server)

    def tmux(self, *args, check=True):
        result = subprocess.run([TMUX, "-S", str(self.socket), "-f", os.devnull, *args], env=self.env,
                                capture_output=True, text=True, timeout=5)
        if check:
            self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def stop_server(self):
        self.tmux("kill-server", check=False)
        # Panes end after the server's SIGHUP; never remove their folder first.
        deadline = time.monotonic() + 3
        for pid in self.pids:
            while running(pid) and time.monotonic() < deadline:
                time.sleep(.02)
        survivors = [pid for pid in self.pids if running(pid)]
        for pid in survivors:
            os.kill(pid, signal.SIGKILL)
        self.assertFalse(survivors, "fake agents survived tmux shutdown")

    def start(self, *options, program=FAKE_AGENT, env=None):
        variables = dict(FAKE_AGENT_RECORD=str(self.record), FAKE_AGENT_READY=str(self.ready), **(env or {}))
        command = ["new-session", "-d", "-s", "agent", "-x", "100", "-y", "30"]
        for key, value in variables.items():
            command += ["-e", f"{key}={value}"]
        self.tmux(*command, str(program), *options, ";", "set-option", "-w", "-t", "agent", "remain-on-exit", "on")
        self.wait_for(self.ready.exists)
        info = json.loads(self.ready.read_text())
        self.pids.append(info["pid"])
        return info

    def pane(self, form):
        return self.tmux("display-message", "-p", "-t", "agent", form).strip()

    def keys(self, *keys):
        self.tmux("send-keys", "-t", "agent", *keys)

    def control(self, text):
        done = len(self.events("control")) + len(text.strip().splitlines())
        temporary = self.root / "control.tmp"
        temporary.write_text(text)
        os.replace(temporary, self.control_path)
        self.wait_for(lambda: len(self.events("control")) == done)
        self.assertFalse([event for event in self.events("control") if "error" in event])

    def test_input_is_recorded_byte_for_byte_with_framing(self):
        self.start("--modes", "paste,cursor", "--esc-ms", "40")
        steps = [(("-l", "h\u00e9llo"), 1), (("Enter",), 2), (None, 3), (("Escape",), 4), (("C-c",), 5),
                 (("C-s",), 6), (("Up",), 7)]
        for keys, count in steps:
            if keys:
                self.keys(*keys)
            else:
                self.tmux("set-buffer", "-b", "fixture", "line one\nline two")
                self.tmux("paste-buffer", "-p", "-d", "-b", "fixture", "-t", "agent")
            self.wait_for(lambda: len(self.tokens()) == count)
        tokens = self.tokens()
        self.assertEqual([(token["kind"], token.get("text"), token.get("name")) for token in tokens], [
            ("text", "h\u00e9llo", None), ("enter", None, None), ("paste", "line one\rline two", None),
            ("esc", None, None), ("ctrl-c", None, None), ("control", None, "ctrl-s"), ("key", "\x1bOA", None)])
        self.assertEqual(bytes.fromhex("".join(token["hex"] for token in tokens)),
                         "h\u00e9llo\r".encode() + b"\x1b[200~line one\rline two\x1b[201~\x1b\x03\x13\x1bOA")
        self.assertEqual([token["t"] for token in tokens], sorted(token["t"] for token in tokens))
        self.assertEqual(len({token["read"] for token in tokens}), len(tokens))

    def test_modes_renders_queries_and_resizes_reach_the_terminal(self):
        screen = self.root / "screen.bin"
        screen.write_bytes("\x1b[2J\x1b[Hfirst line\r\nsecond \u2713".encode())
        steps = -(-len(screen.read_bytes()) // 4)
        self.start("--modes", "paste,alt,cursor,keypad,mouse-button,mouse-sgr", "--render", str(screen),
                   "--render-chunk", "4", "--render-delay", "2", "--query", "dsr,da1,da2", "--query-wait", "3000",
                   "--control", str(self.control_path))
        flags = ("#{bracket_paste_flag}#{alternate_on}#{keypad_cursor_flag}#{keypad_flag}"
                 "#{mouse_button_flag}#{mouse_sgr_flag}")
        self.assertEqual(self.pane(flags), "111111")
        self.assertEqual(self.events("render")[0]["steps"], steps)
        self.assertEqual(self.tmux("capture-pane", "-p", "-t", "agent").splitlines()[:2], ["first line", "second \u2713"])
        # tmux answers every query itself, before the agent is ready.
        replies = self.events("reply")
        self.assertEqual([reply["query"] for reply in replies], ["dsr", "da1", "da2"])
        self.assertFalse(self.events("query-timeout"))
        self.assertEqual(replies[0]["text"], "\x1b[2;9R")
        self.control("modes no-alt,no-cursor,no-mouse-button\nwrite \\e[2J\\e[Hcontrolled\n")
        self.assertEqual(self.pane(flags), "100101")
        self.assertEqual(self.tmux("capture-pane", "-p", "-t", "agent").splitlines()[0], "controlled")
        self.keys("Up")
        self.wait_for(lambda: self.tokens() and self.tokens()[-1]["hex"] == b"\x1b[A".hex())
        self.tmux("resize-window", "-t", "agent", "-x", "90", "-y", "20")
        self.wait_for(lambda: self.events("resize") and self.events("resize")[-1]["cols"] == 90)
        self.assertEqual(self.events("resize")[-1]["rows"], int(self.pane("#{pane_height}")))
        self.control("exit 4")
        self.wait_for(lambda: self.pane("#{pane_dead}") == "1")
        self.assertEqual(self.pane("#{pane_dead_status}"), "4")
        self.assertEqual(self.events("modes")[-1]["active"], ["paste", "keypad", "mouse-sgr"])

    def test_ready_file_hooks_and_exit_keys(self):
        hooks = self.root / "hooks.jsonl"
        info = self.start("--hook-command", "{ cat; echo; } >> " + shlex.quote(str(hooks)),
                          "--session-id", "fixture-session", "--turn-ms", "50", "--exit-keys", "\\x04",
                          "--exit-code", "7")
        self.assertEqual(info["pid"], int(self.pane("#{pane_pid}")))
        started = subprocess.check_output(["ps", "-p", str(info["pid"]), "-o", "lstart="],
                                          env=dict(self.env, LC_ALL="C", TZ="UTC"), text=True).strip()
        self.assertEqual(info["process_start"], started)
        self.assertEqual((info["tty"], info["raw"], info["session_id"]), (True, True, "fixture-session"))
        self.assertEqual(info["size"], [int(self.pane("#{pane_width}")), int(self.pane("#{pane_height}"))])
        self.keys("-l", "hi there")
        self.keys("Enter")
        payloads = lambda: [json.loads(line) for line in hooks.read_text().splitlines()] if hooks.exists() else []
        self.wait_for(lambda: len(payloads()) == 3)
        self.keys("C-d")
        self.wait_for(lambda: self.pane("#{pane_dead}") == "1")
        self.assertEqual(self.pane("#{pane_dead_status}"), "7")
        self.assertEqual([(payload["hook_event_name"], payload.get("source"), payload.get("prompt"),
                           payload.get("reason")) for payload in payloads()],
                         [("SessionStart", "startup", None, None), ("UserPromptSubmit", None, "hi there", None),
                          ("Stop", None, None, None), ("SessionEnd", None, None, "other")])
        self.assertTrue(all(payload["session_id"] == "fixture-session" and payload["cwd"] for payload in payloads()))
        exit_event = self.events("exit")[-1]
        self.assertEqual((exit_event["code"], exit_event["cause"]), (7, "keys"))
        submitted, stopped = [event for event in self.events("hook") if event["event"] in ("UserPromptSubmit", "Stop")]
        self.assertGreaterEqual(stopped["t"] - submitted["t"], 0.05)

    def test_signal_policy(self):
        pid = self.start("--on-signal", "INT=ignore,HUP=ignore", "--on-signal", "TERM=delay:300:42")["pid"]
        for number, name in [(signal.SIGINT, "INT"), (signal.SIGHUP, "HUP")]:
            os.kill(pid, number)
            self.wait_for(lambda: self.events("signal") and self.events("signal")[-1]["signal"] == name)
        self.assertEqual(self.pane("#{pane_dead}"), "0")
        sent = time.monotonic()
        os.kill(pid, signal.SIGTERM)
        self.wait_for(lambda: self.pane("#{pane_dead}") == "1")
        self.assertGreaterEqual(time.monotonic() - sent, 0.3)
        self.assertEqual(self.pane("#{pane_dead_status}"), "42")
        self.assertEqual([(event["signal"], event["policy"]) for event in self.events("signal")],
                         [("INT", "ignore"), ("HUP", "ignore"), ("TERM", "delay:300:42")])
        self.assertEqual(self.events("exit")[-1]["cause"], "signal:TERM")

    def test_default_signal_policy_dies_by_the_signal(self):
        pid = self.start("--on-signal", "TERM=default")["pid"]
        os.kill(pid, signal.SIGTERM)
        self.wait_for(lambda: self.pane("#{pane_dead}") == "1")
        # tmux prints the signal name where libc has sys_signame (macOS), else the number.
        self.assertIn(self.pane("#{pane_dead_signal}"), {str(int(signal.SIGTERM)), "term"})
        self.assertFalse(self.events("signal") or self.events("exit"))

    def test_named_like_an_agent_it_runs_that_agents_hooks(self):
        program = self.root / "bin/claude"
        program.parent.mkdir()
        program.symlink_to(FAKE_AGENT)
        log = self.root / "hooks.jsonl"
        logged = {"type": "command", "command": "{ cat; echo; } >> " + shlex.quote(str(log)), "timeout": 5}
        config = self.root / "claude-config"
        config.mkdir()
        (config / "settings.json").write_text(json.dumps({"hooks": {
            "SessionStart": [{"hooks": [logged]}, {"hooks": [{"type": "command", "command": "exit 2"}, logged]}],
            "UserPromptSubmit": [{"matcher": "", "hooks": [logged]}],
            "SessionEnd": [{"hooks": [logged]}]}}))
        info = self.start("--resume", "abc-123", "--model", "fixture", program=program,
                          env=dict(CLAUDE_CONFIG_DIR=str(config), FAKE_AGENT_TURN_MS="never"))
        self.assertEqual((info["name"], info["argv"], info["session_id"], info["resumed"]),
                         ("claude", ["--resume", "abc-123", "--model", "fixture"], "abc-123", True))
        self.assertEqual(self.events("hooks")[0]["count"], 5)
        self.assertEqual([(event["event"], event["status"], event["blocked"]) for event in self.events("hook")],
                         [("SessionStart", 0, False), ("SessionStart", 2, True)])
        self.keys("-l", "go")
        self.keys("Enter")
        self.wait_for(lambda: len(self.events("hook")) == 3)
        os.kill(info["pid"], signal.SIGTERM)
        self.wait_for(lambda: self.pane("#{pane_dead}") == "1")
        self.assertEqual(self.pane("#{pane_dead_status}"), "143")
        payloads = [json.loads(line) for line in log.read_text().splitlines()]
        self.assertEqual([(payload["hook_event_name"], payload["session_id"]) for payload in payloads],
                         [("SessionStart", "abc-123"), ("UserPromptSubmit", "abc-123"), ("SessionEnd", "abc-123")])
        self.assertEqual((payloads[0]["source"], payloads[1]["prompt"]), ("resume", "go"))


if __name__ == "__main__":
    unittest.main()
