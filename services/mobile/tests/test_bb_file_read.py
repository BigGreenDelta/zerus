"""Black-box computer file reads: documents a conversation links to.

A phone opens an explicit file path of the selected conversation. The first
range captures the whole current file through one native `file-read` call;
later ranges name that capture's digest and are served from it, so a file
replaced in the meantime is reported as changed instead of mixing two
versions. Reads leave the journal two minutes after delivery
(`test_bb_read_classes`); the in-process cases are in
`test_computer_file_read`."""
import base64
import hashlib
import json
import unittest

from bb_operations import CONVERSATION, OPERATIONS, RUN, SESSION, OperationCase, help_text, listing
from connector_harness import BlackBoxCase, default_rules

PATH = "/workspace/example/Сводная итог.xlsx"
ARGV = ["file-read", SESSION, "--json"]
CHUNK = OPERATIONS["constants"]["attachments.MAX_READ_BYTES"]
CHANGED = "file changed during download; open it again"
UNREADABLE = ("The file could not be read. Check that it still exists, is readable and is at most 10 MiB, "
              "then open it again.")


def native_file(data: bytes, **changes) -> dict:
    """`hgs file-read` for the request it reads, which echoes the request identity."""
    return {"request_id": "{{stdin.request_id}}", "name": SESSION, "run_id": RUN, "conversation_id": CONVERSATION,
            "requested_path": PATH, "path": PATH, "file_name": PATH.rsplit("/", 1)[-1],
            "mime": "application/octet-stream", "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
            "data_base64": base64.b64encode(data).decode(), **changes}


class FileReadTests(OperationCase, BlackBoxCase):
    async def asyncSetUp(self):
        await super().asyncSetUp()
        self.hgs.script(default_rules(listing(), help_text("file_read")))

    def read(self, offset=0, length=CHUNK, **payload):
        return self.operation("file_read", path=PATH, offset=offset, length=length, **payload)

    async def test_vector_read(self):
        process = await self.launch()
        await self.replay(process, "file-read-snapshot")

    async def test_ranges_follow_one_capture_of_the_file(self):
        data = b"\x00\xffPK" + bytes(range(256)) + "итог".encode()
        self.hgs.prepend({"argv": ARGV, "stdout": native_file(data)})
        process = await self.launch()
        mark = self.mark()
        received, digest = b"", None
        while len(received) < len(data):
            extra = {} if digest is None else {"sha256": digest}
            request = self.read(len(received), min(100, len(data) - len(received)), **extra)
            response = await self.submit(request, process)
            self.assertEqual(response["state"], "completed", response)
            result = response["result"]
            self.assertEqual(result, {
                "request_id": request["request_id"], "name": SESSION, "run_id": RUN, "conversation_id": CONVERSATION,
                "requested_path": PATH, "path": PATH, "file_name": PATH.rsplit("/", 1)[-1],
                "mime": "application/octet-stream", "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                "offset": len(received), "length": request["payload"]["length"],
                "data_base64": result["data_base64"]})
            digest = result["sha256"]
            received += base64.b64decode(result["data_base64"])
        self.assertEqual(received, data)
        # One native read, given the request without its range and never a shell.
        calls = self.work(mark)
        self.assertEqual([call["argv"] for call in calls], [ARGV])
        first = self.read()["payload"]
        self.assertEqual(set(json.loads(calls[0]["stdin"])), set(first) - {"offset", "length"})
        # A range beyond the captured file reads as a changed file.
        beyond = await self.submit(self.read(len(data), 1, sha256=digest), process)
        self.assertEqual(beyond, {"state": "failed", "result": None, "error": CHANGED})
        self.assertEqual(len(self.work(mark)), 1)

    async def test_a_new_open_reads_the_current_file(self):
        self.hgs.prepend({"argv": ARGV, "times": 1, "stdout": native_file(b"first")},
                         {"argv": ARGV, "stdout": native_file(b"second")})
        process = await self.launch()
        mark = self.mark()
        first, second = (await self.submit(self.read(), process)), (await self.submit(self.read(), process))
        self.assertEqual([base64.b64decode(response["result"]["data_base64"]) for response in (first, second)],
                         [b"first", b"second"])
        self.assertEqual(len(self.work(mark)), 2)

    async def test_a_changed_or_unreadable_file_fails_as_a_read(self):
        self.hgs.prepend({"argv": ARGV, "times": 1, "stdout": native_file(b"changed")},
                         {"argv": ARGV, "exit": 1})
        process = await self.launch()
        # A later range whose capture is gone reads the file again and finds it replaced.
        later = self.read(1, 2, sha256=hashlib.sha256(b"original").hexdigest())
        self.assertEqual(await self.submit(later, process), {"state": "failed", "result": None, "error": CHANGED})
        missing = self.read()
        self.assertEqual(await self.submit(missing, process), {"state": "failed", "result": None, "error": UNREADABLE})
        for request in (later, missing):
            self.assertEqual(self.journal.row(request["request_id"])["state"], "failed")

    async def test_a_cli_without_the_file_read_abi_advertises_and_runs_nothing(self):
        self.hgs.script(default_rules(listing(), help_text()))
        process = await self.launch()
        operations = self.relay.heartbeats()[-1].body["snapshot"]["mobile_capabilities"]["operations"]
        self.assertNotIn("file_read", operations)
        mark = self.mark()
        self.assertEqual(await self.submit(self.read(), process),
                         {"state": "failed", "result": None, "error": "native computer file read ABI is unavailable"})
        self.assertEqual(self.work(mark), [])


if __name__ == "__main__":
    unittest.main()
