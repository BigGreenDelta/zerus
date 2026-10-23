"""Acquisition through the actual connector process; private loopback relay/native fixtures."""
import hashlib
import json
import unittest
import uuid

from connector_harness import IMPLEMENTATION
from session_harness import SessionCase, help_text

MACHINE = "22222222-2222-4222-8222-222222222222"
MARKER = "Mobile repository acquisition ABI: repository-acquisition-v1"
WRAPPER = ["swarm", "__mobile-peer-local", "--json"]


@unittest.skipUnless(IMPLEMENTATION == "rust", "acquisition is additive to the Rust connector")
class RepositoryTests(SessionCase):
    markers = ("repository",)

    def command(self, operation="repository_clone", original=None):
        r = self.envelope(operation, {"expected_machine_id": MACHINE, "url": "git@forge:owner/source.git",
            "parent": "/fixture", "name": "checkout ; $ literal"}, session="", identity=False)
        r["payload"]["request_id"] = r["request_id"]
        if operation == "repository_status":
            r["payload"]["clone_request_id"] = original or str(uuid.uuid4())
        return r

    def native(self, request, status="cloned", **changes):
        p = request["payload"]
        encoded = json.dumps([p["url"], p["parent"], p["name"]], ensure_ascii=False, separators=(",", ":")).encode()
        return {"request_id": p.get("clone_request_id", request["request_id"]),
            "status": status, "fingerprint": hashlib.sha256(encoded).hexdigest(),
            "parent": p["parent"], "name": p["name"], "path": p["parent"] + "/" + p["name"],
            "staging_path": "private-internal-path", **changes}

    def answer(self, request, **changes):
        self.hgs.prepend({"argv": WRAPPER, "stdout": self.native(request, **changes)})

    async def test_clone_runs_once_under_exact_native_machine_and_original_uuid(self):
        request = self.command()
        self.answer(request)
        process = await self.launch()
        result = await self.submit(request, process)
        self.assertEqual(result["state"], "completed")
        self.assertEqual(result["result"]["clone_request_id"], request["request_id"])
        self.assertEqual(result["result"]["machine_id"], MACHINE)
        self.assertNotIn("staging_path", result["result"])
        await self.until(lambda: self.journal.row(request["request_id"])["delivered"], process=process)
        self.assertEqual(await self.submit(request, process, redeliver=True), result)
        [call] = self.native_calls()
        self.assertEqual(call["argv"], WRAPPER)
        payload = json.loads(call["stdin"])
        self.assertEqual(payload["target_machine_id"], MACHINE)
        self.assertEqual(payload["argv"], ["repository", "clone", "--json", "--url", request["payload"]["url"],
            "--parent", "/fixture", "--name", request["payload"]["name"], "--request-id", request["request_id"]])
        self.assertIsNone(payload["payload"])

    async def test_status_only_reads_original_uuid_and_keeps_fresh_outer_identity(self):
        request = self.command("repository_status")
        self.answer(request)
        process = await self.launch()
        result = await self.submit(request, process)
        self.assertEqual(result["state"], "completed")
        self.assertEqual(result["result"]["request_id"], request["request_id"])
        self.assertEqual(result["result"]["clone_request_id"], request["payload"]["clone_request_id"])
        [call] = self.native_calls()
        self.assertEqual(json.loads(call["stdin"])["argv"], ["repository", "status", "--json", "--request-id", request["payload"]["clone_request_id"]])

    async def test_old_native_marker_refuses_before_any_acquisition_command(self):
        self.hgs.script(self.rules(markers=()))
        self.hgs.prepend({"argv": ["--help"], "stdout_text": help_text() + "Repository acquisition ABI: repository-clone-v1\n"})
        process = await self.launch()
        result = await self.submit(self.command(), process)
        self.assertEqual(result["state"], "failed")
        self.assertEqual(self.native_calls(), [])

    async def test_mismatched_original_fingerprint_stays_uncertain_without_launch(self):
        request = self.command()
        self.answer(request, fingerprint="0" * 64)
        process = await self.launch()
        result = await self.submit(request, process)
        self.assertEqual(result["state"], "uncertain")
        self.assertEqual(len(self.native_calls()), 1)
        self.assertEqual(await self.submit(request, process, redeliver=True), result)
        self.assertEqual(len(self.native_calls()), 1)

    async def test_large_failure_receipt_is_bounded_and_duplicate_remains_readable(self):
        request = self.command()
        self.answer(request, status="failed", detail='界\\"' * 1200)
        process = await self.launch()
        result = await self.submit(request, process)
        self.assertEqual(result["state"], "completed")
        self.assertEqual(result["result"]["status"], "failed")
        self.assertLess(len(json.dumps(result, ensure_ascii=False).encode()), 16384)
        self.assertEqual(await self.submit(request, process, redeliver=True), result)

    async def test_read_only_check_reports_canonical_alias_for_explicit_confirmation(self):
        request = self.command("repository_check")
        request["payload"]["parent"] = "/var/tmp"
        self.answer(request, status="ready", parent="/private/var/tmp", path="/private/var/tmp/" + request["payload"]["name"])
        process = await self.launch()
        result = await self.submit(request, process)
        self.assertEqual(result["state"], "completed")
        self.assertEqual(result["result"]["parent"], "/private/var/tmp")
        self.assertEqual(len(self.native_calls()), 1)
        self.assertEqual(json.loads(self.native_calls()[0]["stdin"])["argv"][1], "check")
