"""Real Claude startup trust on a private socket and home; no user credentials.

Run with HGS_CLAUDE_TEST_BIN=/absolute/path/to/claude (otherwise skipped).
"""
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
import uuid
from pathlib import Path

import test_input as fixtures
import test_questions as questions
import test_claude_questions_native as native


@unittest.skipUnless(native.CLAUDE, "Set HGS_CLAUDE_TEST_BIN for isolated native trust coverage")
class NativeClaudeTrust(unittest.TestCase):
    script = fixtures.InputTransport.script
    tmux = fixtures.InputTransport.tmux
    write_record = fixtures.InputTransport.write_record
    tearDown = fixtures.InputTransport.tearDown
    wait_for = native.NativeClaudeQuestions.wait_for
    screen = native.NativeClaudeQuestions.screen
    inspect = questions.Questions.inspect
    answer = questions.Questions.answer

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="hgs-claude-trust-native-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.bin = self.root / ".local/bin"
        self.bin.mkdir(parents=True)
        self.state = self.root / "state"
        self.state.mkdir()
        self.socket = self.root / "socket"
        work = self.root / "work"
        (work / ".claude").mkdir(parents=True)
        (self.root / ".claude").mkdir()
        (self.root / ".claude.json").write_text(json.dumps(dict(
            hasCompletedOnboarding=True, hasSeenAutoDefaultNudge=True, theme="dark",
            customApiKeyResponses=dict(approved=["fixture-key"], rejected=[]))))
        (work / ".claude/settings.local.json").write_text(json.dumps({
            "permissions": {"allow": [f"Bash(echo fixture-{i}:*)" for i in range(83)]}}))
        prelude = ("run_claude: SOPS_AGE_KEY already set — reusing it.\n"
                   if "reuse" in self._testMethodName else
                   "Bitwarden master password:\nunlock: SOPS_AGE_KEY loaded for this session.\n")
        prelude += "run_claude: secrets unlocked → launching claude\n"
        launcher = self.root / "launcher.py"
        launcher.write_text("import os\nos.write(1," + repr(prelude.encode()) + ")\n"
                            "os.execv(" + repr(native.CLAUDE) + ",[" + repr(native.CLAUDE) + "])\n")
        # Record only fixture input; the private wrapper cannot reach a resident server.
        self.script("tmux", "#!" + sys.executable + "\n" +
                    "import json,pathlib,subprocess,sys\n" +
                    "args=sys.argv[1:]\n" +
                    "if args and args[0]=='send-keys':\n" +
                    " with pathlib.Path(" + repr(str(self.root / "sent-keys")) +
                    ").open('a') as log:log.write(json.dumps(args)+'\\n')\n" +
                    "sys.exit(subprocess.call(" + repr([fixtures.TMUX, "-S", str(self.socket), "-f", "/dev/null"]) + "+args))\n")
        self.env = dict(os.environ, HOME=str(self.root), HGS_STATE_DIR=str(self.state),
                        HGS_CONFIG_DIR=str(self.root / "config"), HGS_SELF="test",
                        PATH=str(self.bin) + ":" + os.environ["PATH"])
        for key in ("TMUX", "TMUX_PANE", "HGS_RUN_ID", "HGS_SESSION", "HGS_EXECUTABLE"):
            self.env.pop(key, None)
        self.name = "claude/trust-test"
        self.run_id = str(uuid.uuid4())
        self.conversation_id = ""
        self.addCleanup(lambda: subprocess.run([fixtures.TMUX, "-S", str(self.socket), "kill-server"], capture_output=True))
        # An unused loopback port prevents any model request from leaving the fixture.
        self.tmux("new-session", "-d", "-s", self.name, "-x", "95", "-y", "47", "-c", str(work),
                  "env", "-i", "HOME=" + str(self.root), "PATH=" + os.environ["PATH"],
                  "SHELL=/bin/bash", "TERM=xterm-256color", "LANG=C.UTF-8",
                  "CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC=1", "DISABLE_AUTOUPDATER=1",
                  "ANTHROPIC_API_KEY=fixture-key", "ANTHROPIC_BASE_URL=http://127.0.0.1:1",
                  sys.executable, str(launcher))
        self.pane = self.tmux("display-message", "-p", "-t", "=" + self.name + ":", "#{pane_id}").strip()
        self.tmux("set-option", "-t", "=" + self.name + ":", "remain-on-exit", "on")
        self.wait_for(lambda: "Enter to confirm" in self.screen())
        pid = int(self.tmux("display-message", "-p", "-t", self.pane, "#{pane_pid}"))
        start = subprocess.check_output(["ps", "-p", str(pid), "-o", "lstart="],
                                        env=dict(self.env, LC_ALL="C", TZ="UTC"), text=True).strip()
        self.tmux("set-option", "-t", "=" + self.name + ":", "@hgs_run", self.run_id)
        self.record_path = self.state / (hashlib.sha256(self.name.encode()).hexdigest() + ".json")
        self.record = dict(version=1, name=self.name, agent="claude", run_id=self.run_id,
                           run_identity_version=1, supervisor={}, startup_kind="new",
                           conversation_id=None, launch_dir=str(work), pane=self.pane,
                           pid=pid, process_start=start, activity="unknown", phase="unknown",
                           active_tools={}, subagents={}, last_event_at=0)
        self.write_record()
        state = self.inspect()
        self.assertEqual(state["phase"], "approval", self.screen())
        self.card = state["pending_questions"][0]
        self.assertTrue(self.card["can_answer"])
        body = self.card["questions"][0]["body"]
        self.assertIn("83 tool permissions", body)
        self.assertIn("These will apply without asking.", body)
        self.assertIn(str(work), body)
        self.assertNotIn("SOPS_AGE_KEY", body)
        self.assertFalse((self.root / "sent-keys").exists())

    def payload(self, choice):
        return dict(request_id=str(uuid.uuid4()), expected_run_id=self.run_id,
                    expected_conversation_id="", question_id=self.card["question_id"],
                    expected_question_hash=self.card["question_hash"],
                    answers=[dict(question_id="trust", selected_option_ids=["trust_" + str(choice)], text="")])

    def test_unlock_prelude_explicit_trust_and_retry(self):
        payload = self.payload(1)
        receipt = self.answer(payload)
        self.assertEqual(receipt["status"], "answered")
        self.wait_for(lambda: json.loads((self.root / ".claude.json").read_text())
                      .get("projects", {}).get(self.record["launch_dir"], {}).get("hasTrustDialogAccepted"))
        self.assertEqual(self.inspect()["pending_questions"], [])
        sent = (self.root / "sent-keys").read_bytes()
        self.assertEqual(self.answer(payload), receipt)
        self.assertEqual((self.root / "sent-keys").read_bytes(), sent)

    def test_reuse_prelude_explicit_decline_and_retry(self):
        payload = self.payload(0)
        receipt = self.answer(payload)
        self.assertEqual(receipt["status"], "answered")
        self.wait_for(lambda: self.tmux("display-message", "-p", "-t", self.pane, "#{pane_dead}").strip() == "1")
        self.assertFalse(json.loads((self.root / ".claude.json").read_text())
                         .get("projects", {}).get(self.record["launch_dir"], {}).get("hasTrustDialogAccepted"))
        sent = (self.root / "sent-keys").read_bytes()
        self.assertEqual(self.answer(payload), receipt)
        self.assertEqual((self.root / "sent-keys").read_bytes(), sent)


if __name__ == "__main__":
    unittest.main()
