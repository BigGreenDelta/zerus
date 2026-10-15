"""Scoped resident-adapter contract via a private synthetic Unix RPC server."""
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import unittest
import uuid

HGS = Path(os.environ.get("HGS_TEST_BIN", "target/debug/hgs")).resolve()


class DeepSeekScopedActions(unittest.TestCase):
    def setUp(self):
        # macOS TMPDIR can exceed the Unix socket path limit; keep the private root short.
        self.temp = tempfile.TemporaryDirectory(prefix="hgs-scoped-dsh-", dir="/tmp")
        self.root = Path(self.temp.name)
        self.name = "dsh/example/session"
        self.record = {"name": self.name, "backend": "dsh", "agent": "dsh", "run_id": "run", "conversation_id": "session-synthetic", "agent_home": str(self.root / "native")}
        self.binding = self.root / "state/dsh/bindings" / (hashlib.sha256(self.name.encode()).hexdigest() + ".json")
        self.binding.parent.mkdir(parents=True)
        self.binding.write_text(json.dumps(self.record))
        host = self.root / "state/dsh/hosts" / hashlib.sha256(self.record["agent_home"].encode()).hexdigest()[:16]
        host.mkdir(parents=True)
        self.server = socket.socket(socket.AF_UNIX)
        self.server.bind(str(host / "host.sock")); self.server.listen(); self.server.settimeout(.05)
        self.supported = True
        self.status = "completed"
        self.answer_generation = "fixture-generation"
        self.calls = []
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.serve, daemon=True); self.thread.start()
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("HGS_")}
        self.env.update(HOME=str(self.root), HGS_STATE_DIR=str(self.root / "state"), HGS_CONFIG_DIR=str(self.root / "config"), HGS_SELF="fixture", HGS_PEERS="")

    def tearDown(self):
        self.stop.set(); self.thread.join(timeout=2); self.server.close(); self.temp.cleanup()

    def serve(self):
        while not self.stop.is_set():
            try:
                client, _ = self.server.accept()
            except socket.timeout:
                continue
            with client:
                reader = client.makefile("rb")
                request = json.loads(reader.readline())
                self.calls.append(request)
                method = request["method"]
                if method == "ping":
                    value = {"generation": "fixture-generation", "sessionActions": self.supported}
                elif method == "inspect":
                    value = {"generation": "fixture-generation", "lifecycleActions": ["pause", "resume", "rename", "forget"]}
                elif method == "session-action":
                    value = {"generation": self.answer_generation, "sessionId": "session-synthetic", "action": request["params"]["action"], "status": self.status, "error": "Synthetic refusal"}
                else:
                    value = {}
                client.sendall((json.dumps({"ok": True, "value": value}) + "\n").encode())

    def run_action(self, action, **extra):
        request = {"request_id": str(uuid.uuid4()), "action": action, "expected_run_id": "run", "expected_conversation_id": "session-synthetic", **extra}
        result = subprocess.run([str(HGS), "session-action", self.name, "--json"], env=self.env, input=json.dumps(request), capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return request, json.loads(result.stdout)

    def test_supported_resident_adapter_uses_generation_without_create_or_restart(self):
        _, result = self.run_action("pause")
        self.assertEqual(result["status"], "completed")
        self.assertEqual([call["method"] for call in self.calls], ["ping", "inspect", "session-action"])
        self.assertEqual(self.calls[-1]["params"]["generation"], "fixture-generation")
        self.assertTrue(json.loads(self.binding.read_text())["paused"])
        self.assertEqual(result["result_target"]["run_id"], "run")

    def test_old_resident_adapter_and_wrong_identity_are_definite_failures(self):
        self.supported = False
        _, result = self.run_action("pause")
        self.assertEqual(result["status"], "failed")
        self.assertEqual([call["method"] for call in self.calls], ["ping"])
        self.assertEqual(json.loads(self.binding.read_text()), self.record)
        _, result = self.run_action("pause", expected_conversation_id="session-other")
        self.assertEqual(result["status"], "failed")
        self.assertEqual(len(self.calls), 1)

    def test_confirmed_zero_effect_failure_and_uncertain_native_handoff_remain_distinct(self):
        for status in ("failed", "uncertain"):
            self.status = status
            request, result = self.run_action("pause")
            self.assertEqual(result["status"], status)
            self.assertEqual(json.loads(self.binding.read_text()), self.record)
            before = len(self.calls)
            duplicate = subprocess.run([str(HGS), "session-action", self.name, "--json"], env=self.env, input=json.dumps(request), capture_output=True, text=True, timeout=10)
            self.assertEqual(json.loads(duplicate.stdout), result)
            self.assertEqual(len(self.calls), before)

    def test_replacement_generation_failed_ack_cannot_claim_zero_effects(self):
        self.status = "failed"
        self.answer_generation = "replacement-generation"
        _, result = self.run_action("pause")
        self.assertEqual(result["status"], "uncertain")
        self.assertEqual(json.loads(self.binding.read_text()), self.record)

    def test_confirmed_resume_gets_new_run_and_rename_preserves_conversation(self):
        _, result = self.run_action("resume")
        self.assertEqual(result["status"], "completed")
        self.assertNotEqual(result["result_target"]["run_id"], "run")
        self.binding.write_text(json.dumps(self.record))
        _, result = self.run_action("rename", new_name="dsh/example/new")
        self.assertEqual(result["result_target"]["name"], "dsh/example/new")
        self.assertEqual(result["result_target"]["conversation_id"], "session-synthetic")
        self.assertFalse(self.binding.exists())

    def test_context_reset_keeps_the_explicit_fallback_without_native_effects(self):
        # The reviewed native release has no context reset; a clear must never reach the host.
        state = sorted(path.relative_to(self.root) for path in (self.root / "state").rglob("*"))
        request = {"request_id": str(uuid.uuid4()), "expected_run_id": "run", "expected_conversation_id": "session-synthetic"}
        result = subprocess.run([str(HGS), "clear-context", self.name, "--json"], env=self.env, input=json.dumps(request), capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("This DeepSeek harness does not expose a native context reset. Use its native UI.", result.stderr)
        self.assertEqual(self.calls, [])
        self.assertEqual(json.loads(self.binding.read_text()), self.record)
        self.assertEqual(sorted(path.relative_to(self.root) for path in (self.root / "state").rglob("*")), state)


if __name__ == "__main__":
    unittest.main()
