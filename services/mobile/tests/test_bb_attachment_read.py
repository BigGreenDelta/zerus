"""Black-box attachments: inline files in a send, and reading sent files back.

A phone reads a file by its delivery receipt and position, never by a path.
The connector checks the exact conversation, reads the whole file through one
native call, keeps a few verified files briefly in memory and serves 512 KiB
ranges, because one relay result holds at most 1 MiB."""
import asyncio
import base64
import hashlib
import json
import subprocess
import unittest
import uuid

from bb_operations import (ARCHIVE, ARCHIVES, CONVERSATION, OPERATIONS, RECEIPTS, RUN, SESSION, OperationCase,
                           help_text, listing, size_vector, validation_cases)
from connector_harness import MIB, BlackBoxCase, canonical, default_rules

MESSAGE = "00000000-0000-4000-8000-000000000001"
MAX_FILE = OPERATIONS["constants"]["attachments.LIMITS"]["max_file_bytes"]
CHUNK = OPERATIONS["constants"]["attachments.MAX_READ_BYTES"]
NATIVE_OUTPUT = OPERATIONS["constants"]["attachment_reads.NATIVE_OUTPUT_BYTES"]
IDENTITY = ["inspect", SESSION, "--skip-processes"]
INLINE = "invalid or oversized inline attachment/message"


def encoded(data: bytes) -> str:
    return base64.b64encode(data).decode()


def native_file(data: bytes, message: str = MESSAGE, index: int = 0, **changes) -> dict:
    return {"request_id": message, "index": index, "name": "Скрин \U0001f600.png",
            "mime": "image/png", "bytes": len(data), "reference": "[Image #1]", "data_base64": encoded(data), **changes}


def detail(**changes) -> dict:
    return {"name": SESSION, "run_id": RUN, "conversation_id": CONVERSATION, "subagents": {"worker-1": {}}, **changes}


def attachment_argv(message: str = MESSAGE, index: int = 0, *extra: str) -> list[str]:
    return ["attachment", SESSION, "--request", message, "--index", str(index), "--conversation", CONVERSATION, *extra]


class ScriptedAnswerTests(BlackBoxCase):
    """Harness self-test of the scripted answers these suites add."""

    tests_connector = False

    def run_hgs(self, *argv, stdin=b""):
        return subprocess.run([str(self.hgs.bin / "hgs"), *argv], input=stdin, capture_output=True,
                              env={"PATH": "/usr/bin:/bin"}, timeout=10)

    async def test_file_answers_stdin_matches_and_delegation(self):
        answer = self.scratch / "answer.bin"
        answer.write_bytes(b'{"exact": "bytes"}  ')
        echo = self.scratch / "echo.sh"
        echo.write_text('#!/bin/sh\nprintf "%s|" "$@"\ncat\necho delegated >&2\nexit 4\n')
        echo.chmod(0o700)
        self.hgs.script([{"argv": ["file"], "stdout_path": str(answer)},
                         {"argv": ["pick"], "stdin_has": {"action": "input"}, "stdout": {"picked": "input"}},
                         {"argv": ["pick"], "stdout": {"picked": "other"}},
                         {"prefix": ["real"], "exec": str(echo)}])
        self.assertEqual((await asyncio.to_thread(self.run_hgs, "file")).stdout, b'{"exact": "bytes"}  ')
        for stdin, picked in ((b'{"action": "input"}', "input"), (b'{"action": "snapshot"}', "other"),
                              (b'["action"]', "other"), (b"", "other")):
            self.assertEqual(json.loads((await asyncio.to_thread(self.run_hgs, "pick", stdin=stdin)).stdout),
                             {"picked": picked})
        delegated = await asyncio.to_thread(self.run_hgs, "real", "a b", stdin=b"in")
        self.assertEqual((delegated.returncode, delegated.stdout, delegated.stderr),
                         (4, b"real|a b|in", b"delegated\n"))
        self.assertEqual(self.hgs.finished(self.hgs.calls("real")[0]["pid"])["exit"], 4)


class InlineAttachmentTests(OperationCase, BlackBoxCase):
    async def asyncSetUp(self):
        await super().asyncSetUp()
        ack = {"request_id": "{{stdin.request_id}}", "status": "submitted", "name": SESSION, "run_id": RUN,
               "conversation_id": CONVERSATION}
        self.hgs.script([{"argv": ["send", SESSION, "--json"], "stdout": ack}] + default_rules(listing()))

    def send(self, *files, text="Synthetic files"):
        return self.operation("send", text=text, attachments=[
            {"name": name, "mime": "application/octet-stream", "data_base64": encoded(data)} for name, data in files])

    async def test_files_above_the_result_limit_reach_native_stdin_unchanged(self):
        process = await self.launch()
        for name, files in (("2 MiB", [("large.bin", b"x" * (2 * MIB))]),
                            ("20 MiB in total", [("one.bin", b"a" * MAX_FILE), ("two.bin", b"b" * MAX_FILE)])):
            with self.subTest(name):
                request = self.send(*files)
                mark = self.mark()
                result = await self.submit(request, process, timeout=30)
                self.assertEqual(result["state"], "completed", result)
                [call] = self.since(mark, "send")
                self.assertEqual(json.loads(call["stdin"]), request["payload"])
                batch = next(exchange for exchange in self.relay.exchanges if request["request_id"] in exchange.delivered)
                self.assertGreater(len(canonical(batch.response)), MIB)

    async def test_invalid_files_never_reach_native(self):
        process = await self.launch()
        cases = [self.send(("one.bin", b"a" * MAX_FILE), ("two.bin", b"b" * MAX_FILE), ("three.bin", b"c")),
                 self.send(("big.bin", b"x" * (MAX_FILE + 1))),
                 self.send(("../escape.txt", b"x")), self.send(("empty.bin", b"")),
                 self.send(*[(f"{index}.bin", b"x") for index in range(9)])]
        with_path = self.send(("passwd", b"x"))
        with_path["payload"]["attachments"][0]["path"] = "/etc/passwd"
        not_canonical = self.send(("note.txt", b"x"))
        not_canonical["payload"]["attachments"][0]["data_base64"] = "eA"
        for request in (*cases, with_path, not_canonical):
            with self.subTest(request["payload"]["attachments"][0]["name"], count=len(request["payload"]["attachments"])):
                mark = self.mark()
                self.assertEqual(await self.submit(request, process, timeout=30),
                                 {"state": "failed", "result": None, "error": INLINE})
                self.assertEqual(self.since(mark, "send"), [])
        await self.assert_rejections(process, [case for case in validation_cases("conversation", {"send"})
                                               if "attachment" in case["id"]], [["send"]])

    async def test_message_text_is_bounded_beside_files(self):
        process = await self.launch()
        for identity in ("send-text-64KiB", "send-text-64KiB+1"):  # 64 KiB of UTF-8, then one byte more.
            with self.subTest(identity):
                payload, expect = size_vector(identity)
                mark = self.mark()
                result = await self.submit(self.operation("send", text=payload["text"]), process)
                if expect["error"] is None:
                    self.assertEqual(result["state"], "completed", result)
                else:
                    self.assertEqual(result, {"state": "failed", "result": None, "error": expect["receipt_error"]})
                self.assertEqual(len(self.since(mark, "send")), 0 if expect["error"] else 1)
        # A file needs no text.
        request = self.send(("note.txt", b"x"), text="")
        request["payload"]["attachments"][0]["reference"] = "[Image #1]"
        mark = self.mark()
        self.assertEqual((await self.submit(request, process))["state"], "completed")
        self.assertEqual(json.loads(self.since(mark, "send")[0]["stdin"]), request["payload"])


class AttachmentReadTests(OperationCase, BlackBoxCase):
    async def asyncSetUp(self):
        await super().asyncSetUp()
        self.hgs.script([{"argv": IDENTITY, "stdout": detail()}]
                        + default_rules(listing(), help_text("attachment_read")))

    def read(self, offset=0, length=CHUNK, message=MESSAGE, **payload):
        return self.operation("attachment_read", message_id=message, index=payload.pop("index", 0), offset=offset,
                              length=length, **payload)

    def serve(self, data: bytes, message: str = MESSAGE, argv=None, **changes) -> None:
        """Answer the file read from a file, as large answers need."""
        path = self.scratch / f"answer-{uuid.uuid4()}.json"
        path.write_text(json.dumps(native_file(data, message, **changes)))
        self.hgs.prepend({"argv": argv or attachment_argv(message), "stdout_path": str(path)})

    async def test_vector_reads(self):
        process = await self.launch()
        # Both name one receipt file: a failed read is never cached, a verified one is.
        missing = await self.replay(process, "attachment-read-missing-file")
        self.assertEqual(missing["error"], RECEIPTS["execute:attachment-read-missing-file"]["error"])
        await self.replay(process, "attachment-read-chunk")

    async def test_ranges_return_exact_bytes_from_one_native_read(self):
        data = bytes(range(256)) * 4097  # Three ranges, the last one partial.
        self.serve(data)
        process = await self.launch()
        mark = self.mark()
        received = b""
        while len(received) < len(data):
            request = self.read(len(received), min(CHUNK, len(data) - len(received)))
            response = await self.submit(request, process)
            self.assertEqual(response["state"], "completed", response)
            result = response["result"]
            self.assertEqual(result, {
                "request_id": request["request_id"], "name": SESSION, "run_id": RUN, "conversation_id": CONVERSATION,
                "message_id": MESSAGE, "index": 0, "file_name": native_file(b"")["name"], "mime": "image/png",
                "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(), "offset": len(received),
                "length": request["payload"]["length"], "data_base64": result["data_base64"]})
            self.assertLess(len(canonical({"request_id": request["request_id"], **response})), MIB)
            received += base64.b64decode(result["data_base64"])
        self.assertEqual(received, data)
        self.assertEqual([call["argv"] for call in self.work(mark)], [IDENTITY, attachment_argv()])
        # A range that starts beyond the file is refused from the same copy.
        beyond = await self.submit(self.read(len(data), 1), process)
        self.assertEqual(beyond, {"state": "failed", "result": None, "error": "attachment range is outside the file"})
        self.assertEqual(len(self.work(mark)), 2)

    async def test_archived_and_child_reads_name_their_flags(self):
        archived = ["inspect", SESSION, "--archive", ARCHIVE, "--skip-processes"]
        self.hgs.prepend({"argv": archived, "stdout": detail(archive_id=ARCHIVE, state="archived")})
        self.serve(b"archived child file", argv=attachment_argv(MESSAGE, 0, "--archive", ARCHIVE, "--agent", "worker-1"))
        process = await self.launch()
        mark = self.mark()
        response = await self.submit(self.read(0, 19, archive_id=ARCHIVE, agent_id="worker-1"), process)
        self.assertEqual(response["state"], "completed", response)
        self.assertEqual((response["result"]["archive_id"], response["result"]["agent_id"]), (ARCHIVE, "worker-1"))
        self.assertEqual([call["argv"] for call in self.work(mark)],
                         [archived, attachment_argv(MESSAGE, 0, "--archive", ARCHIVE, "--agent", "worker-1")])

    async def test_foreign_identities_and_answers_are_refused(self):
        process = await self.launch()
        data = b"example"
        conversation = "native attachment conversation identity did not match"
        file = "native attachment identity did not match"
        cases = [
            ("other run", detail(run_id="other-run"), {}, conversation),
            ("other conversation", detail(conversation_id="other-conversation"), {}, conversation),
            ("archived answer", detail(archive_id=ARCHIVE, state="archived"), {}, conversation),
            ("foreign child", None, {"agent_id": "worker-2"}, "child does not belong to the exact selected parent"),
            ("other receipt", None, {"answer": {"request_id": str(uuid.uuid4())}}, file),
            ("other index", None, {"answer": {"index": 1}}, file),
            ("changed size", None, {"answer": {"bytes": len(data) + 1}}, "attachment size changed since it was sent"),
            ("invalid data", None, {"answer": {"data_base64": "not base64"}}, "native attachment data is invalid"),
            ("empty", None, {"data": b""}, "attachment exceeds 10 MiB"),
            # One byte more still has the encoded length of 10 MiB; three more do not.
            ("10 MiB + 1", None, {"data": b"x" * (MAX_FILE + 1)}, "attachment must be nonempty and at most 10 MiB"),
            ("10 MiB + 3", None, {"data": b"x" * (MAX_FILE + 3)}, "attachment exceeds 10 MiB"),
            ("missing", None, {"exit": 1}, "attachment is not available from this conversation on the computer"),
        ]
        for name, identity, change, error in cases:
            with self.subTest(name):
                message = str(uuid.uuid4())  # Never a cached file.
                self.hgs.prepend({"argv": IDENTITY, "stdout": detail() if identity is None else identity})
                payload = {"agent_id": change["agent_id"]} if "agent_id" in change else {}
                argv = attachment_argv(message, 0, *(["--agent", change["agent_id"]] if payload else []))
                if "exit" in change:
                    self.hgs.prepend({"argv": argv, "exit": 1, "stderr": "synthetic missing file\n"})
                else:
                    self.serve(change.get("data", data), message, argv=argv, **change.get("answer", {}))
                response = await self.submit(self.read(0, 7, message, **payload), process)
                self.assertEqual(response, {"state": "failed", "result": None, "error": error})
        mark = self.mark()
        for name, request, error in (
                ("unknown session", {**self.read(), "session": "codex/example/other"},
                 "session must match an exact nonarchived snapshot entry"),
                ("unknown archive", self.read(archive_id="00000000-0000-4000-8000-00000000abcd"),
                 "archive must match a fresh exact local snapshot entry")):
            with self.subTest(name):
                self.assertEqual(await self.submit(request, process), {"state": "failed", "result": None, "error": error})
        self.assertEqual(self.work(mark), [])
        await self.assert_rejections(process, validation_cases("conversation", {"attachment_read"}), [["attachment"]])

    async def test_invalid_positions_and_fields_never_reach_native(self):
        process = await self.launch()
        fields, position = "invalid attachment read fields", "invalid attachment position or range"
        message = "invalid attachment message identity"
        nil = "00000000-0000-0000-0000-000000000000"
        self.assertIn((SESSION, nil), ARCHIVES)  # The snapshot lists it, so only the payload check refuses it.
        mark = self.mark()
        for name, change, error in (  # A None value removes the field.
                ("path", {"path": "/private/file.png"}, fields),
                ("message path", {"message_id": "../../etc/passwd"}, message),
                ("message option", {"message_id": "--archive"}, message),
                ("index 8", {"index": 8}, position), ("index -1", {"index": -1}, position),
                ("index float", {"index": 0.0}, position),
                ("offset at 10 MiB", {"offset": MAX_FILE}, position), ("negative offset", {"offset": -1}, position),
                ("long range", {"length": CHUNK + 1}, position),
                ("range past 10 MiB", {"offset": MAX_FILE - 10, "length": 11}, position),
                ("empty conversation", {"expected_conversation_id": ""}, "invalid bounded native identity"),
                ("empty run", {"expected_run_id": ""}, "expected_run_id is required"),
                ("nil archive", {"archive_id": nil}, "invalid archive identity"),
                ("no message", {"message_id": None}, fields), ("no length", {"length": None}, fields),
                ("other request", {"request_id": str(uuid.uuid4())}, "payload request_id must match request identity")):
            with self.subTest(name):
                request = self.read()
                for key, value in change.items():
                    if value is None:
                        del request["payload"][key]
                    else:
                        request["payload"][key] = value
                self.assertEqual(await self.submit(request, process), {"state": "failed", "result": None, "error": error})
        self.assertEqual(self.work(mark), [])

    async def test_native_output_is_capped_at_one_encoded_file(self):
        process = await self.launch()
        for extra, error in ((0, None), (1, "native output exceeds size limit")):
            with self.subTest(extra=extra):
                message = str(uuid.uuid4())
                text = json.dumps(native_file(b"bounded", message))
                path = self.scratch / f"padded-{extra}.json"
                path.write_text(text + " " * (NATIVE_OUTPUT - len(text) + extra))  # JSON allows trailing spaces.
                self.hgs.prepend({"argv": attachment_argv(message), "stdout_path": str(path)})
                response = await self.submit(self.read(0, 7, message), process)
                self.assertEqual(response["error"], error)

    async def test_verified_files_are_kept_least_recently_used_up_to_three(self):
        files = {name: bytes([index]) * MAX_FILE for index, name in enumerate("ABCD")}
        messages = {name: str(uuid.UUID(int=index + 1)) for index, name in enumerate("ABCD")}
        for name, data in files.items():
            self.serve(data, messages[name])
        child = attachment_argv(messages["A"], 0, "--agent", "worker-1")
        self.serve(files["A"], messages["A"], argv=child)
        process = await self.launch()
        mark = self.mark()
        for name in "ABCDCAB" "C":
            response = await self.submit(self.read(0, 1, messages[name]), process, timeout=20)
            self.assertEqual(base64.b64decode(response["result"]["data_base64"]), files[name][:1])
        # Another identity of the same receipt is never served from the cache.
        self.assertEqual((await self.submit(self.read(0, 1, messages["A"], agent_id="worker-1"), process))["state"],
                         "completed")
        reads = [call["argv"] for call in self.since(mark, "attachment")]
        self.assertEqual([reads.count(attachment_argv(messages[name])) for name in "ABCD"], [2, 2, 1, 1])
        self.assertEqual(reads.count(child), 1)

    async def test_the_cache_budget_is_bytes_not_files(self):
        # Three 10 MiB files fill the 30 MiB budget; five small files all stay.
        messages = [str(uuid.UUID(int=index + 1)) for index in range(5)]
        for index, message in enumerate(messages):
            self.serve(bytes([index + 1]) * 7, message)
        process = await self.launch()
        mark = self.mark()
        for _ in range(2):
            for message in messages:
                response = await self.submit(self.read(0, 1, message), process)
                self.assertEqual(response["state"], "completed", response)
        self.assertEqual([call["argv"] for call in self.work(mark)],
                         [argv for message in messages for argv in (IDENTITY, attachment_argv(message))])

    async def test_without_the_attachment_abi_nothing_runs(self):
        self.hgs.script([{"argv": IDENTITY, "stdout": detail()}] + default_rules(listing()))
        process = await self.launch()
        self.assertNotIn("attachment_read",
                         self.relay.heartbeats()[-1].body["snapshot"]["mobile_capabilities"]["operations"])
        await self.replay(process, "attachment-read-unavailable")


if __name__ == "__main__":
    unittest.main()
