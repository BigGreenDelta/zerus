#!/usr/bin/env python3
"""Keyless contract of the real Claude Code and Codex with the task instructions
that Zerus's SessionStart hook prints: the agent hands them to the model. Each
agent runs the real `hgs __state hook` and talks to a localhost model fixture.

Run with HGS_CLAUDE_TEST_BIN and HGS_CODEX_TEST_BIN (otherwise skipped)."""
import http.server
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import threading
import unittest

REPO = Path(__file__).resolve().parents[1]
HGS = Path(os.environ.get("HGS_TEST_BIN", REPO / "target/debug/hgs")).resolve()
PHRASE = "Run `hgs task` in your shell"


class Native(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="hgs-task-context-native-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name).resolve()
        (self.root / "state").mkdir()
        self.requests = []

    def serve(self, handler):
        handler.requests = self.requests
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        return server.server_port

    def session(self, agent):
        return dict(HGS_SESSION=agent + "/orchard/api", HGS_RUN_ID="run-native", HGS_AGENT=agent,
                    HGS_STATE_DIR=str(self.root / "state"), HGS_CONFIG_DIR=str(self.root / "config"))

    def hooks(self):
        return {"SessionStart": [{"hooks": [{"type": "command", "command": shlex.join([str(HGS), "__state", "hook"])}]}]}


class Claude(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
        if not self.path.startswith("/v1/messages") or "count_tokens" in self.path:
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"input_tokens":1}')
            return
        self.requests.append(body)
        message = dict(id="msg_1", type="message", role="assistant", model=body.get("model"), content=[],
                       stop_reason=None, stop_sequence=None, usage=dict(input_tokens=1, output_tokens=1))
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        for kind, data in [("message_start", dict(message=message)),
                           ("content_block_start", dict(index=0, content_block=dict(type="text", text=""))),
                           ("content_block_delta", dict(index=0, delta=dict(type="text_delta", text="Fixture complete."))),
                           ("content_block_stop", dict(index=0)),
                           ("message_delta", dict(delta=dict(stop_reason="end_turn", stop_sequence=None), usage=dict(output_tokens=1))),
                           ("message_stop", {})]:
            self.wfile.write(("event: " + kind + "\ndata: " + json.dumps(dict(type=kind, **data)) + "\n\n").encode())
            self.wfile.flush()


class Codex(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        self.requests.append(json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}"))
        item = dict(type="message", id="msg_1", role="assistant", status="completed",
                    content=[dict(type="output_text", text="Fixture complete.", annotations=[])])
        response = dict(id="resp_1", object="response", status="completed", output=[item],
                        usage=dict(input_tokens=1, output_tokens=1, total_tokens=2))
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        for kind, value in [("response.created", dict(response=dict(response, status="in_progress", output=[]))),
                            ("response.output_item.added", dict(output_index=0, item=item)),
                            ("response.output_item.done", dict(output_index=0, item=item)),
                            ("response.completed", dict(response=response))]:
            self.wfile.write(("event: " + kind + "\ndata: " + json.dumps(dict(type=kind, **value)) + "\n\n").encode())
            self.wfile.flush()


@unittest.skipUnless(os.environ.get("HGS_CLAUDE_TEST_BIN"), "Set HGS_CLAUDE_TEST_BIN for the isolated native Claude task context test")
class NativeClaudeTaskContext(Native):
    def test_claude_hands_the_task_instructions_to_the_model(self):
        port = self.serve(Claude)
        home = self.root / "home"
        (home / ".claude").mkdir(parents=True)
        (home / ".claude.json").write_text(json.dumps(dict(hasCompletedOnboarding=True,
                                                           customApiKeyResponses=dict(approved=["fixture-key"], rejected=[]))))
        (home / ".claude/settings.json").write_text(json.dumps({"hooks": self.hooks()}))
        env = dict(HOME=str(home), PATH=os.environ["PATH"], CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC="1",
                   ANTHROPIC_API_KEY="fixture-key", ANTHROPIC_BASE_URL="http://127.0.0.1:" + str(port), **self.session("claude"))
        run = subprocess.run([os.environ["HGS_CLAUDE_TEST_BIN"], "-p", "Hello.", "--model", "claude-sonnet-4-6"],
                             env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=120)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertTrue(self.requests, "Claude sent no request")
        self.assertIn(PHRASE, json.dumps(self.requests[-1]["messages"]))


@unittest.skipUnless(os.environ.get("HGS_CODEX_TEST_BIN"), "Set HGS_CODEX_TEST_BIN for the isolated native Codex task context test")
class NativeCodexTaskContext(Native):
    def test_codex_hands_the_task_instructions_to_the_model(self):
        port = self.serve(Codex)
        home = self.root / "codex"
        home.mkdir()
        work = self.root / "work"
        work.mkdir()
        (home / "hooks.json").write_text(json.dumps({"hooks": self.hooks()}))
        (home / "config.toml").write_text(
            'model="gpt-6.1-sol"\nmodel_provider="fixture"\ncheck_for_update_on_startup=false\n'
            '[model_providers.fixture]\nname="Fixture"\nbase_url="http://127.0.0.1:' + str(port) + '/v1"\n'
            'wire_api="responses"\nrequest_max_retries=0\nstream_max_retries=0\n[features]\nhooks=true\n')
        env = dict(HOME=str(self.root), PATH=os.environ["PATH"], CODEX_HOME=str(home), OPENAI_API_KEY="fixture",
                   **self.session("codex"))
        run = subprocess.run([os.environ["HGS_CODEX_TEST_BIN"], "exec", "--ephemeral", "--skip-git-repo-check",
                              "-s", "read-only", "--dangerously-bypass-hook-trust", "-C", str(work), "--color", "never", "Hello."],
                             env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=120)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertTrue(self.requests, "Codex sent no request")
        developer = [item for item in self.requests[0].get("input", []) if item.get("role") == "developer"]
        self.assertIn(PHRASE, json.dumps(developer))


if __name__ == "__main__":
    unittest.main()
