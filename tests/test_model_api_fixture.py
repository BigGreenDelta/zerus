#!/usr/bin/env python3
"""The keyless model API fixture of the real-agent suites (tests/model_api_fixture.py)."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.request

import model_api_fixture as api

SCRIPT = Path(__file__).resolve().with_name("model_api_fixture.py")


def claude_request(*messages, tools=("AskUserQuestion", "Write")):
    return {"model": "claude-sonnet-4-6", "stream": True, "messages": list(messages),
            "tools": [{"name": name, "input_schema": {}} for name in tools]}


def codex_request(*items, tools=("request_user_input_async",)):
    # Codex 0.162 lists most tools in an additional_tools item, some in namespaces.
    extra = {"type": "additional_tools", "tools": [{"type": "namespace", "name": "functions",
                                                     "tools": [{"type": "function", "name": name} for name in tools]}]}
    return {"model": "gpt-6.1-sol", "stream": True, "input": [extra, *items],
            "tools": [{"type": "custom", "name": "exec"}]}


def user(*parts):
    return {"role": "user", "content": [{"type": "text", "text": text} for text in parts]}


class Plans(unittest.TestCase):
    def test_typed_text_skips_context_blocks_and_tool_results_end_the_turn(self):
        asked = user("<system-reminder>context</system-reminder>", api.QUESTIONS)
        self.assertEqual(api.typed(claude_request(asked)), api.QUESTIONS)
        self.assertEqual(api.plan(claude_request(asked), "/work/project"),
                         ("tool", "AskUserQuestion", {"questions": api.QUESTION_ITEMS}))
        result = {"role": "user", "content": [{"type": "tool_result", "tool_use_id": "t", "content": "answered"}]}
        later = claude_request(asked, {"role": "assistant", "content": []}, result)
        self.assertEqual((api.typed(later), api.results(later)), (api.QUESTIONS, ["answered"]))
        self.assertEqual(api.plan(later, "/work/project"), ("text", api.DONE))
        # A new typed message starts over.
        again = claude_request(asked, result, user("Plain fixture message."))
        self.assertEqual((api.results(again), api.plan(again, "")), ([], ("text", api.REPLY)))

    def test_only_the_exact_prompt_with_its_tool_offered_calls_a_tool(self):
        write = claude_request(user(api.WRITE))
        self.assertEqual(api.plan(write, "/work/project"),
                         ("tool", "Write", {"file_path": os.path.join("/work/project", api.APPROVED_FILE),
                                            "content": api.APPROVED_TEXT}))
        self.assertEqual(api.plan(write, ""), ("text", api.REPLY))
        # Side requests offer no tools; a title request quotes the prompt.
        self.assertEqual(api.plan(claude_request(user(api.WRITE), tools=()), "/w"), ("text", api.REPLY))
        quoted = codex_request({"role": "user", "content": [{"type": "input_text",
                                                             "text": "Title this.\n\nUser prompt:\n" + api.QUESTIONS}]})
        self.assertEqual(api.plan(quoted, ""), ("text", api.REPLY))

    def test_codex_tools_come_from_namespaces_and_outputs_end_the_turn(self):
        asked = {"role": "user", "content": [{"type": "input_text", "text": api.QUESTIONS}]}
        request = codex_request({"role": "user", "content": [{"type": "input_text", "text": "<environment_context/>"}]},
                                asked)
        self.assertEqual(api.tool_names(request), ["exec", "request_user_input_async"])
        self.assertEqual(api.plan(request, ""), ("tool", "request_user_input_async", {"questions": api.CODEX_QUESTIONS}))
        answered = codex_request(asked, {"type": "function_call", "call_id": "c"},
                                 {"type": "function_call_output", "call_id": "c", "output": '{"accepted":true}'})
        self.assertEqual((api.results(answered), api.plan(answered, "")), (['{"accepted":true}'], ("text", api.DONE)))

    def test_streams_carry_the_whole_answer(self):
        message = api.anthropic_message({}, ("tool", "Write", {"file_path": "x"}), 3)
        events = list(api.anthropic_events(message))
        self.assertEqual([kind for kind, _ in events], ["message_start", "content_block_start", "content_block_delta",
                                                        "content_block_stop", "message_delta", "message_stop"])
        self.assertEqual(json.loads(events[2][1]["delta"]["partial_json"]), {"file_path": "x"})
        self.assertEqual(events[4][1]["delta"]["stop_reason"], "tool_use")
        events = list(api.responses_events({}, ("text", "Fixture reply."), 4))
        self.assertEqual(events[-1][0], "response.completed")
        self.assertEqual(events[-1][1]["response"]["output"][0]["content"][0]["text"], "Fixture reply.")


class Server(unittest.TestCase):
    """The script as the suites run it: `python -I -X utf8`, on loopback, logging each request."""

    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="hgs-model-api-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.errors = self.root / "fixture.err"
        with self.errors.open("wb") as errors:
            self.process = subprocess.Popen([sys.executable, "-I", "-X", "utf8", str(SCRIPT), "--root", str(self.root),
                                             "--project", str(self.root)], stdin=subprocess.DEVNULL,
                                            stdout=subprocess.DEVNULL, stderr=errors,
                                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        self.addCleanup(self.process.wait, 10)
        self.addCleanup(self.process.kill)
        deadline = time.monotonic() + 30
        while not (self.root / "port").is_file():
            self.assertIsNone(self.process.poll(), f"the fixture exited: {self.errors.read_text(errors='replace')}")
            self.assertLess(time.monotonic(), deadline, f"no port file: {self.errors.read_text(errors='replace')}")
            time.sleep(0.05)
        self.url = f"http://127.0.0.1:{int((self.root / 'port').read_text())}"
        # Never through a proxy of the environment.
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def post(self, path, body):
        request = urllib.request.Request(self.url + path, data=json.dumps(body).encode(),
                                         headers={"Content-Type": "application/json"})
        with self.opener.open(request, timeout=30) as response:
            return response.read().decode("utf-8")

    def events(self, text):
        return [json.loads(line[len("data: "):]) for line in text.splitlines() if line.startswith("data: ")]

    def test_both_apis_answer_and_every_request_is_logged(self):
        events = self.events(self.post("/v1/messages?beta=true", claude_request(user("Привет ✓", api.QUESTIONS))))
        tool, = [event["content_block"] for event in events if event["type"] == "content_block_start"]
        self.assertEqual((tool["type"], tool["name"]), ("tool_use", "AskUserQuestion"))
        events = self.events(self.post("/v1/responses", codex_request(
            {"role": "user", "content": [{"type": "input_text", "text": "Привет ✓"}]})))
        self.assertEqual(events[-1]["response"]["output"][0]["content"][0]["text"], api.REPLY)
        self.assertEqual(json.loads(self.post("/v1/messages/count_tokens", {})), {"input_tokens": 100})
        logged = api.requests(self.root)
        self.assertEqual([(entry["route"], entry["prompt"], entry["answer"]) for entry in logged],
                         [("/v1/messages", api.QUESTIONS, ["tool", "AskUserQuestion"]),
                          ("/v1/responses", "Привет ✓", ["text", api.REPLY]),
                          ("/v1/messages/count_tokens", None, ["text", api.REPLY])])


if __name__ == "__main__":
    unittest.main()
