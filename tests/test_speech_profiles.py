"""Isolated profile management and HTTP recognition contracts; no live accounts."""
import base64
from concurrent.futures import ThreadPoolExecutor
from contextlib import closing, contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
import uuid

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "services/speech"))
from zerus_speech.config import load
from zerus_speech.jobs import Jobs
from zerus_speech.providers.base import Cancelled, ProviderError
from zerus_speech.providers.openai_http import OpenAIHTTP
from zerus_speech.service import Server

HGS = Path(os.environ.get("HGS_TEST_BIN", REPO / "target/debug/hgs")).resolve()


@contextmanager
def endpoint(status=200, response=None, *, drip=False):
    requests = []
    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
            requests.append((self.path, dict(self.headers), body))
            raw = json.dumps({"text": "Synthetic transcript"} if response is None else response).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(raw)))
            self.send_header("Location", "/must-not-follow")
            if drip:
                self.send_header("Connection", "close")
            self.end_headers()
            try:
                if drip:
                    for byte in raw:
                        self.wfile.write(bytes([byte])); self.wfile.flush(); time.sleep(0.08)
                else:
                    self.wfile.write(raw)
            except OSError:
                pass
        def log_message(self, *args):
            pass
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
    try:
        yield "http://127.0.0.1:" + str(server.server_port) + "/v1", requests
    finally:
        server.shutdown(); server.server_close(); thread.join()


class SpeechProfiles(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="zerus-speech-profiles-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.config_dir = self.root / "config"; self.config_dir.mkdir(mode=0o755)
        self.path = self.config_dir / "speech.json"
        self.path.write_text(json.dumps({"schema": 1, "state_dir": str(self.root / "private"), "profiles": []}))
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("HGS_")}
        self.env.update(HOME=str(self.root), HGS_CONFIG_DIR=str(self.config_dir), HGS_STATE_DIR=str(self.root / "sessions"))
        self.initial = self.call({"action": "config_list"})
        self.machine = self.initial["machine_id"]

    def call(self, payload):
        process = subprocess.run([str(HGS), "speech", "--json"], input=json.dumps(payload), text=True,
                                 capture_output=True, env=self.env, timeout=20)
        self.assertEqual(process.returncode, 0, process.stderr)
        return json.loads(process.stdout)

    def upsert(self, url="https://asr.example/v1", *, revision=None, name="Remote", credential=None):
        return {"action": "profile_upsert", "request_id": str(uuid.uuid4()), "expected_machine_id": self.machine,
                "expected_revision": self.initial["revision"] if revision is None else revision,
                "profile": {"id": "remote", "name": name, "adapter": "openai_http", "model": "gigaam-v3",
                            "language": "ru", "base_url": url, "allow_insecure_http": url.startswith("http:")},
                "credential": {"mode": "keep"} if credential is None else credential}

    def test_crud_cas_duplicate_receipts_and_secret_redaction_without_service(self):
        self.assertFalse(self.initial["service"]["available"])
        request = self.upsert(credential={"mode": "replace", "value": "synthetic-private-key"})
        accepted = self.call(request)
        self.assertEqual(accepted["state"], "completed")
        listed = self.call({"action": "config_list"})
        self.assertTrue(listed["profiles"][0]["credential"]["configured"])
        self.assertNotIn("synthetic-private-key", json.dumps(listed))
        self.assertEqual(self.call(request), accepted)  # receipt precedes now-stale CAS
        changed = {**request, "profile": {**request["profile"], "name": "Different"}}
        self.assertEqual(self.call(changed)["error_code"], "request_conflict")
        self.assertEqual(self.call(self.upsert())["error_code"], "revision_conflict")
        status = {"action": "config_status", "expected_machine_id": self.machine, "operation_id": request["request_id"]}
        self.assertEqual(self.call(status), accepted)
        remove = {"action": "profile_remove", "request_id": str(uuid.uuid4()), "expected_machine_id": self.machine,
                  "expected_revision": listed["revision"], "profile_id": "remote"}
        removed = self.call(remove); self.assertEqual(removed["state"], "completed")
        self.assertEqual(self.call({"action": "config_list"})["profiles"], [])
        self.assertEqual(self.call(self.upsert(revision=removed["revision"]))["error_code"], "profile_id_retired")
        self.assertEqual(self.config_dir.stat().st_mode & 0o777, 0o755)
        self.assertEqual((self.config_dir / "speech-profiles").stat().st_mode & 0o777, 0o700)
        with closing(sqlite3.connect(self.config_dir / "speech-profiles/profiles.sqlite3")) as db:
            self.assertIsNone(db.execute("SELECT secret FROM profiles").fetchone()[0])
            self.assertNotIn("synthetic-private-key", str(db.execute("SELECT * FROM operations").fetchall()))

    def test_concurrent_edit_one_winner_and_persistent_key(self):
        requests = [self.upsert(name="First"), self.upsert(name="Second")]
        with ThreadPoolExecutor(2) as pool:
            results = list(pool.map(self.call, requests))
        self.assertEqual(sorted(x["state"] for x in results), ["completed", "failed"])
        listed = self.call({"action": "config_list"})
        self.assertEqual(listed["credential_key"], self.initial["credential_key"])
        self.assertEqual(listed["revision"], self.initial["revision"] + 1)

    def test_test_without_runtime_is_definite_and_does_not_mutate_profile(self):
        saved = self.call(self.upsert())
        request = {"action": "profile_test", "request_id": str(uuid.uuid4()), "expected_machine_id": self.machine,
                   "profile_id": "remote", "expected_profile_revision": saved["profile_revision"]}
        result = self.call(request)
        self.assertEqual(result["error_code"], "service_unavailable")
        self.assertEqual(self.call(request), result)

    def test_http_full_job_snapshot_revision_pin_and_no_secret_discovery(self):
        with endpoint() as (url, received):
            saved = self.call(self.upsert(url, credential={"mode": "replace", "value": "synthetic-original"}))
            config = load(self.path)
            jobs = Jobs(config, start_worker=False)
            self.addCleanup(jobs.close)
            self.assertEqual(jobs.discovery()["profiles"][0]["profile_revision"], saved["profile_revision"])
            self.assertNotIn("synthetic-original", json.dumps(jobs.discovery()))
            audio = {"format": "pcm_s16le", "sample_rate": 16000, "channels": 1,
                     "data_base64": base64.b64encode(b"\x10\x00" * 1600).decode()}
            request = {"action": "submit", "request_id": str(uuid.uuid4()), "profile_id": "remote",
                       "profile_revision": saved["profile_revision"], "audio": audio}
            jobs.handle(request)
            newer = self.call(self.upsert("http://127.0.0.1:1/v1", revision=saved["revision"],
                                         credential={"mode": "replace", "value": "synthetic-new"}))
            refused = jobs.handle({**request, "request_id": str(uuid.uuid4())})
            self.assertEqual(refused["error_code"], "profile_revision_conflict")
            jobs.worker.start()
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                result = jobs.handle({"action": "status", "job_id": request["request_id"]})
                if result["state"] == "completed": break
                time.sleep(0.01)
            self.assertEqual(result["text"], "Synthetic transcript")
            self.assertEqual(len(received), 1)
            self.assertEqual(received[0][0], "/v1/audio/transcriptions")
            self.assertEqual(received[0][1]["Authorization"], "Bearer synthetic-original")
            self.assertIn(b'filename="audio.wav"', received[0][2])
            self.assertIn(b"RIFF", received[0][2]); self.assertIn(b"gigaam-v3", received[0][2])
            self.assertEqual(jobs.stats()["totals"]["submitted"], 1)
            self.assertIsNone(jobs.db.execute("SELECT profile_snapshot FROM jobs").fetchone()[0])
            self.assertGreater(newer["revision"], saved["revision"])

    def test_explicit_synthetic_probe_async_receipt_no_voice_counters(self):
        with endpoint() as (url, received):
            saved = self.call(self.upsert(url))
            server = Server(load(self.path))
            thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
            try:
                request = {"action": "profile_test", "request_id": str(uuid.uuid4()), "expected_machine_id": self.machine,
                           "profile_id": "remote", "expected_profile_revision": saved["profile_revision"]}
                result = self.call(request)
                self.assertEqual(result["state"], "running")
                status = {"action": "config_status", "expected_machine_id": self.machine, "operation_id": request["request_id"]}
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    result = self.call(status)
                    if result["state"] == "completed": break
                    time.sleep(0.02)
                self.assertEqual(result["state"], "completed")
                self.assertNotIn("Synthetic transcript", json.dumps(result))
                self.assertEqual(self.call(request), result)
                self.assertEqual(len(received), 1)
                self.assertEqual(server.jobs.stats()["totals"]["submitted"], 0)
            finally:
                server.shutdown(); server.server_close(); thread.join()

    def test_http_never_follows_redirect_or_exposes_remote_error(self):
        for status, response, code in [(302, {"text": "redirect"}, "http_status"),
                                       (401, {"error": "synthetic-private-key"}, "http_authentication"),
                                       (200, {"wrong": "synthetic-private-key"}, "http_response_invalid")]:
            with self.subTest(status=status), endpoint(status, response) as (url, received):
                profile = self.upsert(url)["profile"]
                with self.assertRaises(ProviderError) as raised:
                    OpenAIHTTP().transcribe(profile, b"\x10\x00" * 1600, lambda: False)
                self.assertEqual(str(raised.exception), code)
                self.assertEqual(len(received), 1)

    def test_http_absolute_deadline_and_cancel_stop_slow_close_response(self):
        with endpoint(drip=True) as (url, received):
            profile = self.upsert(url)["profile"]
            start = time.monotonic()
            with patch("zerus_speech.providers.openai_http.TIMEOUT_SECONDS", 0.2):
                with self.assertRaises(ProviderError) as raised:
                    OpenAIHTTP().transcribe(profile, b"\0\0" * 1600, lambda: False)
            self.assertEqual(str(raised.exception), "http_timeout")
            self.assertLess(time.monotonic() - start, 1.5)
            cancelled = threading.Event(); timer = threading.Timer(0.15, cancelled.set); timer.start()
            start = time.monotonic()
            try:
                with self.assertRaises(Cancelled):
                    OpenAIHTTP().transcribe(profile, b"\0\0" * 1600, cancelled.is_set)
            finally:
                timer.cancel()
            self.assertLess(time.monotonic() - start, 1.5)
            self.assertEqual(len(received), 2)

    def test_insecure_http_rejects_resolved_public_address_before_connect(self):
        profile = self.upsert("http://public.example/v1")["profile"]
        with patch("zerus_speech.providers.openai_http.resolve", return_value=[(2, 1, 6, "", ("8.8.8.8", 80))]):
            with self.assertRaises(ProviderError) as raised:
                OpenAIHTTP().transcribe(profile, b"\0\0" * 1600, lambda: False)
        self.assertEqual(str(raised.exception), "http_address_blocked")


if __name__ == "__main__":
    unittest.main()
