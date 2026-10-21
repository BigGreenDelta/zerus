#!/usr/bin/env python3
"""Keyless loopback fakes of the Anthropic Messages and OpenAI Responses APIs for real agents.

Stdlib only, so it runs as `python -I -X utf8 model_api_fixture.py --root DIR --project DIR`
(UTF-8 whatever the console's code page). It listens on 127.0.0.1 only, on a free port that
it writes to DIR/port, and appends one JSON line per request to DIR/requests.jsonl. Claude
Code reaches it through ANTHROPIC_BASE_URL with a dummy key, Codex through a model provider
whose base_url is http://127.0.0.1:PORT/v1; nothing here knows a real account.

Every request gets a short text reply, except the agent loop of a typed prompt that is exactly
one of these phrases (not a request that quotes it, such as a title request):

- QUESTIONS: Claude's AskUserQuestion with QUESTION_ITEMS, Codex's request_user_input_async
  with CODEX_QUESTIONS when it offers that tool.
- TALL_QUESTION: Claude's AskUserQuestion with TALL_QUESTION_ITEMS, taller than the 80x24
  of a pane no client views.
- WRITE: Claude's Write tool, creating APPROVED_FILE in --project with APPROVED_TEXT.
- BASH: Claude's Bash tool with BASH_INPUT, which creates BASH_FILE in its folder; LONG_BASH
  the same command after LONG_LINES no-op lines, a panel taller than the 192 rows to which
  hgs grows a pane no client views (question_terminal::MAX_UNVIEWED_ROWS), so it stays clipped.

After the tool's result the turn ends with DONE. Side requests (titles, token counts) get
the plain reply.
"""
import argparse
import itertools
import json
import os
from pathlib import Path
import socketserver
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

QUESTIONS = "Ask the fixture questions now."
WRITE = "Use the Write tool now."
TALL_QUESTION = "Ask the tall fixture question now."
REPLY = "Fixture reply."
DONE = "Fixture complete."
APPROVED_FILE = "approved.txt"
APPROVED_TEXT = "approved through hgs\n"
BASH = "Use the Bash tool now."
LONG_BASH = "Use the long Bash tool now."
BASH_FILE = "bash-approved.txt"
BASH_INPUT = {"command": f"printf approved > {BASH_FILE}", "description": "Create the fixture file after approval"}
LONG_LINES = 240
# The questions of tests/test_claude_questions_native.py, which the Windows screen fixtures
# tests/fixtures/claude-question-*-windows.txt were recorded with.
QUESTION_ITEMS = [
    {"question": "Which release should we prepare?", "header": "Release", "options": [
        {"label": "Stable", "description": "Use stable build"}, {"label": "Preview", "description": "Use preview build"}]},
    {"question": "Which checks should we run?", "header": "Checks", "multiSelect": True, "options": [
        {"label": "Unit", "description": "Unit checks"}, {"label": "Integration", "description": "Integration checks"},
        {"label": "UI", "description": "UI checks"}]},
    {"question": "What else should we know?", "header": "Notes", "options": [
        {"label": "Nothing", "description": "No extra notes"}, {"label": "Later", "description": "Discuss later"}]},
]
CODEX_QUESTIONS = [{"title": "Which option should the fixture use?", "options": ["One", "Two"]}]
# The tall question of tests/test_claude_questions_native.py.
TALL_QUESTION_ITEMS = [
    {"question": "Which release should we prepare?", "header": "Провайдер", "multiSelect": True, "options": [
        {"label": "API-ключи", "description": "Любой провайдер из списка (Z.AI, OpenRouter, DeepSeek и другие): ввести, "
         "заменить или удалить ключ на выбранной машине. Дополнительные поля провайдер описывает сам."},
        {"label": "Вход через браузер", "description": "Подписки: показываем ссылку и код, вход выполняется в своём "
         "браузере, даже если машина удалённая. Для этого нужен запущенный сервер на той машине."},
        {"label": "Свои адреса", "description": "Свой совместимый адрес (локальная модель, корпоративный прокси): URL, "
         "ключ и список моделей, которые появятся в выборе модели."},
        {"label": "Копировать на другую машину", "description": "Перенести уже настроенного провайдера с одной машины "
         "на другую без повторного ввода ключа, с подтверждением на обеих сторонах."}]},
]


def items(body):
    return body["input"] if isinstance(body.get("input"), list) else body.get("messages") or []


def typed_parts(item):
    """The text parts a person typed in a user message of either API: not the `<tag>`
    context blocks that agents add."""
    if not isinstance(item, dict) or item.get("role") != "user":
        return []
    content = item.get("content")
    parts = [content] if isinstance(content, str) else [
        part.get("text", "") for part in content if isinstance(part, dict)
        and part.get("type") in ("text", "input_text")] if isinstance(content, list) else []
    return [text for text in parts if text.strip() and not text.lstrip().startswith("<")]


def typed(body):
    """The newest typed user text, or None."""
    if isinstance(body.get("input"), str):
        return body["input"]
    return next((typed_parts(item)[-1] for item in reversed(items(body)) if typed_parts(item)), None)


def results(body):
    """Tool results after the newest typed text: Anthropic tool_result blocks, Responses outputs."""
    found = []
    for item in reversed(items(body)):
        if not isinstance(item, dict):
            continue
        if item.get("type") in ("function_call_output", "custom_tool_call_output"):
            found.append(item.get("output"))
        elif item.get("role") == "user" and isinstance(item.get("content"), list):
            found += [block.get("content") for block in reversed(item["content"])
                      if isinstance(block, dict) and block.get("type") == "tool_result"]
        if typed_parts(item):
            break
    return found[::-1]


def tool_names(body):
    """The tools a request offers, flattening Codex's namespaces and additional_tools items."""
    names = []

    def add(tool):
        if isinstance(tool, dict):
            if tool.get("type") == "namespace":
                for inner in tool.get("tools") or []:
                    add(inner)
            elif tool.get("name"):
                names.append(tool["name"])
    for tool in body.get("tools") or []:
        add(tool)
    for item in body.get("input") if isinstance(body.get("input"), list) else []:
        if isinstance(item, dict) and item.get("type") == "additional_tools":
            for tool in item.get("tools") or []:
                add(tool)
    return names


def plan(body, project):
    """What to answer: ("text", TEXT) or ("tool", NAME, INPUT)."""
    # The whole typed text: a request that quotes it, such as Codex's title request, is not it.
    offered, prompt = tool_names(body), (typed(body) or "").strip()
    if results(body):
        return ("text", DONE)
    if prompt == QUESTIONS and "AskUserQuestion" in offered:
        return ("tool", "AskUserQuestion", {"questions": QUESTION_ITEMS})
    if prompt == TALL_QUESTION and "AskUserQuestion" in offered:
        return ("tool", "AskUserQuestion", {"questions": TALL_QUESTION_ITEMS})
    if prompt == QUESTIONS and "request_user_input_async" in offered:
        return ("tool", "request_user_input_async", {"questions": CODEX_QUESTIONS})
    if prompt == WRITE and "Write" in offered and project:
        return ("tool", "Write", {"file_path": os.path.join(project, APPROVED_FILE), "content": APPROVED_TEXT})
    if prompt == BASH and "Bash" in offered:
        return ("tool", "Bash", BASH_INPUT)
    if prompt == LONG_BASH and "Bash" in offered:
        return ("tool", "Bash", dict(BASH_INPUT, command=":\n" * LONG_LINES + BASH_INPUT["command"]))
    return ("text", REPLY)


def anthropic_message(body, answer, number):
    if answer[0] == "tool":
        content = [{"type": "tool_use", "id": f"toolu_fixture_{number}", "name": answer[1], "input": answer[2]}]
    else:
        content = [{"type": "text", "text": answer[1]}]
    return {"id": f"msg_fixture_{number}", "type": "message", "role": "assistant",
            "model": body.get("model") or "claude-sonnet-4-6", "content": content,
            "stop_reason": "tool_use" if answer[0] == "tool" else "end_turn", "stop_sequence": None,
            "usage": {"input_tokens": 100, "output_tokens": 8, "cache_creation_input_tokens": 0,
                      "cache_read_input_tokens": 0}}


def anthropic_events(message):
    """The Messages stream of `message`, as (event, data) pairs."""
    yield "message_start", {"message": dict(message, content=[], stop_reason=None)}
    for index, block in enumerate(message["content"]):
        if block["type"] == "tool_use":
            yield "content_block_start", {"index": index, "content_block": dict(block, input={})}
            delta = {"type": "input_json_delta", "partial_json": json.dumps(block["input"])}
        else:
            yield "content_block_start", {"index": index, "content_block": {"type": "text", "text": ""}}
            delta = {"type": "text_delta", "text": block["text"]}
        yield "content_block_delta", {"index": index, "delta": delta}
        yield "content_block_stop", {"index": index}
    yield "message_delta", {"delta": {"stop_reason": message["stop_reason"], "stop_sequence": None},
                            "usage": {"output_tokens": message["usage"]["output_tokens"]}}
    yield "message_stop", {}


def responses_events(body, answer, number):
    """The Responses stream of `answer`, as (event, data) pairs."""
    if answer[0] == "tool":
        item = {"type": "function_call", "id": f"fc_fixture_{number}", "call_id": f"call_fixture_{number}",
                "name": answer[1], "arguments": json.dumps(answer[2]), "status": "completed"}
    else:
        item = {"type": "message", "id": f"msg_fixture_{number}", "role": "assistant", "status": "completed",
                "content": [{"type": "output_text", "text": answer[1], "annotations": []}]}
    response = {"id": f"resp_fixture_{number}", "object": "response", "status": "completed",
                "model": body.get("model") or "fixture", "output": [item],
                "usage": {"input_tokens": 10, "output_tokens": 5, "total_tokens": 15,
                          "input_tokens_details": {"cached_tokens": 0},
                          "output_tokens_details": {"reasoning_tokens": 0}}}
    yield "response.created", {"response": dict(response, status="in_progress", output=[])}
    if item["type"] == "function_call":
        yield "response.output_item.added", {"output_index": 0, "item": dict(item, status="in_progress", arguments="")}
        yield "response.function_call_arguments.delta", {"output_index": 0, "item_id": item["id"],
                                                          "delta": item["arguments"]}
        yield "response.function_call_arguments.done", {"output_index": 0, "item_id": item["id"],
                                                         "arguments": item["arguments"]}
    else:
        part = item["content"][0]
        yield "response.output_item.added", {"output_index": 0, "item": dict(item, status="in_progress", content=[])}
        yield "response.content_part.added", {"output_index": 0, "content_index": 0, "item_id": item["id"],
                                              "part": dict(part, text="")}
        yield "response.output_text.delta", {"output_index": 0, "content_index": 0, "item_id": item["id"],
                                             "delta": part["text"]}
        yield "response.output_text.done", {"output_index": 0, "content_index": 0, "item_id": item["id"],
                                            "text": part["text"]}
        yield "response.content_part.done", {"output_index": 0, "content_index": 0, "item_id": item["id"],
                                             "part": part}
    yield "response.output_item.done", {"output_index": 0, "item": item}
    yield "response.completed", {"response": response}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, code, payload):
        data = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def stream(self, events):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        for kind, data in events:
            self.wfile.write(f"event: {kind}\ndata: {json.dumps(dict(type=kind, **data))}\n\n".encode())
            self.wfile.flush()

    def do_GET(self):
        route = urlsplit(self.path).path.rstrip("/")
        self.server.record({"method": "GET", "route": route})
        if route.endswith("/models"):
            return self.reply(200, {"object": "list", "data": [], "has_more": False})
        self.reply(404, {"error": {"type": "not_found_error", "message": "no such route"}})

    def do_POST(self):
        route = urlsplit(self.path).path.rstrip("/")
        try:
            body = json.loads(self.rfile.read(int(self.headers.get("Content-Length") or 0)) or b"{}")
        except ValueError:
            body = {}
        body = body if isinstance(body, dict) else {}
        number = next(self.server.numbers)
        answer = plan(body, self.server.project)
        self.server.record({"method": "POST", "route": route, "n": number, "model": body.get("model"),
                            "tools": tool_names(body), "prompt": typed(body), "results": results(body),
                            "answer": list(answer[:2])})
        try:
            if route.endswith("/count_tokens"):
                return self.reply(200, {"input_tokens": 100})
            if route.endswith("/messages"):
                message = anthropic_message(body, answer, number)
                return self.stream(anthropic_events(message)) if body.get("stream") else self.reply(200, message)
            if route.endswith("/responses"):
                return self.stream(responses_events(body, answer, number))
            self.reply(404, {"error": {"type": "not_found_error", "message": "no such route"}})
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass  # The agent stopped reading, as an interrupted turn does.


class Server(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, root, project):
        super().__init__(("127.0.0.1", 0), Handler)
        self.project = project
        self.numbers = itertools.count(1)
        self.lock = threading.Lock()
        self.log = Path(root) / "requests.jsonl"

    def server_bind(self):
        # HTTPServer would name itself with socket.getfqdn, a reverse lookup that can
        # stall for many seconds on a CI runner; the loopback address is its name.
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]

    def record(self, entry):
        with self.lock, open(self.log, "a", encoding="utf-8") as log:
            log.write(json.dumps(entry, ensure_ascii=False) + "\n")


def requests(root):
    """The requests a fixture in `root` has logged so far."""
    path = Path(root) / "requests.jsonl"
    lines = path.read_text(encoding="utf-8").split("\n")[:-1] if path.exists() else []
    return [json.loads(line) for line in lines]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", required=True, help="an existing folder for the port file and the request log")
    parser.add_argument("--project", default="", help="the agent's project folder, where the Write tool writes")
    args = parser.parse_args()
    server = Server(args.root, args.project)
    staged = Path(args.root) / "port.tmp"
    staged.write_text(str(server.server_address[1]), encoding="ascii")
    os.replace(staged, Path(args.root) / "port")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
