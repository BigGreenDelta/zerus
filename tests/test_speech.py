"""Private speech service contracts with synthetic PCM and no model/native agents."""
from array import array
import base64
import json
import os
from pathlib import Path
import socket
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
from zerus_speech.audio import MAX_AUDIO_BYTES, decode_audio, join_text, segments
from zerus_speech.config import load
from zerus_speech.jobs import Jobs
from zerus_speech.providers.gigaam import GigaAM
from zerus_speech.service import Server

HGS = Path(os.environ.get("HGS_TEST_BIN", REPO / "target/debug/hgs")).resolve()


class SpeechService(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="hgs-speech-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.config_path = self.root / "speech.json"
        self.config_path.write_text(json.dumps({"schema": 1, "state_dir": str(self.root / "private"),
            "profiles": [{"id": "test", "name": "Synthetic speech", "adapter": "fixture",
                          "model": "fixture", "language": "ru", "text": "Hello fixture"}]}))
        self.config = load(self.config_path)

    def jobs(self, **kwargs):
        jobs = Jobs(self.config, test_mode=True, **kwargs)
        self.addCleanup(jobs.close)
        return jobs

    def submit(self, *, job_id=None, pcm=b"\x10\x01" * 16000):
        return {"action": "submit", "request_id": job_id or str(uuid.uuid4()), "profile_id": "test",
                "audio": {"format": "pcm_s16le", "sample_rate": 16000, "channels": 1,
                          "data_base64": base64.b64encode(pcm).decode()}}

    def settle(self, jobs, job_id, expected="completed"):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            result = jobs.handle({"action": "status", "job_id": job_id})
            if result["state"] == expected:
                return result
            time.sleep(0.01)
        self.fail(str(result))

    def test_unknown_job_has_a_typed_nonacceptance_response(self):
        jobs = self.jobs()
        result = jobs.handle({"action": "status", "job_id": str(uuid.uuid4())})
        self.assertEqual(result["error_code"], "not_found")
        self.assertEqual(result["state"], "failed")
        self.assertEqual(jobs.handle({"action": "stats"})["totals"]["submitted"], 0)

    def test_completion_idempotency_conflict_stats_and_audio_cleanup(self):
        jobs = self.jobs()
        request = self.submit()
        accepted = jobs.handle(request)
        result = self.settle(jobs, accepted["job_id"])
        self.assertEqual(result["text"], "Hello fixture")
        self.assertEqual(jobs.handle(request), {key: value for key, value in result.items() if key != "text"})
        self.assertNotIn("text", jobs.handle({"action": "cancel", "job_id": accepted["job_id"]}))
        conflict = self.submit(job_id=accepted["job_id"], pcm=b"\x20\x01" * 16000)
        self.assertEqual(jobs.handle(conflict)["state"], "failed")
        self.assertEqual(jobs.handle({"action": "status", "job_id": accepted["job_id"]}), result)
        self.assertFalse(list((self.root / "private").glob("*.pcm")))
        stats = jobs.handle({"action": "stats"})
        self.assertEqual(stats["totals"]["submitted"], 1)
        self.assertEqual(stats["totals"]["completed"], 1)
        self.assertEqual(stats["totals"]["audio_ms"], 1000)
        self.assertNotIn("Hello fixture", json.dumps(stats))

    def test_queue_bound_and_cancel_are_durable(self):
        self.config["max_queue"] = 1
        jobs = self.jobs(start_worker=False)
        first = jobs.handle(self.submit())
        with self.assertRaisesRegex(ValueError, "queue is full"):
            jobs.handle(self.submit())
        cancelled = jobs.handle({"action": "cancel", "job_id": first["job_id"]})
        self.assertEqual(cancelled["state"], "cancelled")
        self.assertFalse(list((self.root / "private").glob("*.pcm")))
        self.assertEqual(jobs.handle({"action": "cancel", "job_id": first["job_id"]}), cancelled)
        self.assertEqual(jobs.handle({"action": "stats"})["totals"]["cancelled"], 1)

    def test_running_cancel_does_not_publish_text_or_double_count(self):
        self.config["profiles"][0]["delay"] = 0.3
        jobs = self.jobs()
        submitted = jobs.handle(self.submit())
        self.settle(jobs, submitted["job_id"], "running")
        jobs.handle({"action": "cancel", "job_id": submitted["job_id"]})
        time.sleep(0.4)
        result = jobs.handle({"action": "status", "job_id": submitted["job_id"]})
        self.assertEqual(result["state"], "cancelled")
        self.assertNotIn("text", result)
        totals = jobs.handle({"action": "stats"})["totals"]
        self.assertEqual((totals["submitted"], totals["cancelled"], totals["completed"]), (1, 1, 0))

    def test_restart_marks_jobs_uncertain_without_replay(self):
        jobs = Jobs(self.config, test_mode=True, start_worker=False)
        request = self.submit()
        jobs.handle(request)
        jobs.close()
        recovered = self.jobs()
        result = recovered.handle(request)
        self.assertEqual(result["state"], "uncertain")
        self.assertEqual(result["error_code"], "service_restarted")
        self.assertFalse(list((self.root / "private").glob("*.pcm")))
        totals = recovered.handle({"action": "stats"})["totals"]
        self.assertEqual((totals["submitted"], totals["uncertain"], totals["completed"]), (1, 1, 0))
        self.assertEqual(totals["error_codes"], {"service_restarted": 1})

    def test_result_ttl_keeps_receipt_and_statistics(self):
        jobs = self.jobs()
        request = self.submit()
        submitted = jobs.handle(request)
        self.settle(jobs, submitted["job_id"])
        with jobs.lock, jobs.db:
            jobs.db.execute("UPDATE jobs SET updated=0")
        expired = jobs.handle(request)
        self.assertEqual(expired["state"], "uncertain")
        self.assertEqual(expired["error_code"], "result_expired")
        self.assertNotIn("text", expired)
        self.assertEqual(jobs.handle({"action": "stats"})["totals"]["completed"], 1)

    def test_failure_is_typed_and_analytics_can_be_disabled(self):
        self.config["profiles"][0]["fail"] = True
        jobs = self.jobs()
        job = jobs.handle(self.submit())
        self.settle(jobs, job["job_id"], "failed")
        stats = jobs.handle({"action": "stats"})
        self.assertEqual(stats["totals"]["error_codes"], {"inference_failed": 1})
        self.assertNotIn("Synthetic failure", json.dumps(stats))
        self.config["analytics_enabled"] = False
        another = jobs.handle(self.submit())
        self.settle(jobs, another["job_id"], "failed")
        self.assertEqual(jobs.handle({"action": "stats"})["totals"]["submitted"], 1)

    def test_fixture_provider_cannot_be_enabled_by_host_config_alone(self):
        jobs = Jobs(self.config, start_worker=False)
        self.addCleanup(jobs.close)
        self.assertFalse(jobs.discovery()["profiles"][0]["available"])
        with self.assertRaisesRegex(ValueError, "not installed"):
            jobs.handle(self.submit())

    def test_discovery_never_imports_gigaam(self):
        profile = {"model": "v3_e2e_rnnt", "download_root": str(self.root / "weights")}
        with patch("importlib.util.find_spec", return_value=object()) as find:
            self.assertFalse(GigaAM(self.root).available(profile)[0])
            self.assertEqual(find.call_count, 2)
        self.assertNotIn("gigaam", sys.modules)

    def test_audio_validation_and_120_second_bound(self):
        audio = self.submit()["audio"]
        self.assertEqual(len(decode_audio(audio)), 32000)
        for change in ({"sample_rate": True}, {"channels": 2}, {"format": "wav"},
                       {"data_base64": "!!!!"}, {"data_base64": "AA=="}, {"data_base64": ""}):
            with self.assertRaises(ValueError):
                decode_audio(audio | change)
        with self.assertRaises(ValueError):
            decode_audio(audio | {"data_base64": base64.b64encode(b"\0" * (MAX_AUDIO_BYTES + 2)).decode()})

    def test_segmentation_handles_silence_and_continuous_long_audio(self):
        self.assertEqual(segments(b"\0\0" * 16000), [])
        pcm = array("h", [200] * (16000 * 120)).tobytes()
        boundaries = segments(pcm)
        self.assertEqual(boundaries[0][0], 0)
        self.assertEqual(boundaries[-1][1], len(pcm) // 2)
        self.assertTrue(all(0 < end - start <= 24 * 16000 for start, end in boundaries))
        self.assertTrue(all(a[1] - b[0] == 6400 for a, b in zip(boundaries, boundaries[1:])))
        self.assertEqual(join_text("A shared word.", "Shared word continues", overlap=True), "A shared word. continues")
        self.assertEqual(join_text("Again", "again", overlap=False), "Again again")

    def test_segmentation_prefers_silence_without_dropping_surrounding_samples(self):
        samples = array("h", [200] * (16000 * 40))
        samples[20 * 16000:21 * 16000] = array("h", [0] * 16000)
        boundaries = segments(samples.tobytes())
        self.assertEqual(boundaries, [(0, 20 * 16000), (20 * 16000, 40 * 16000)])

    def test_socket_and_hgs_bridge(self):
        server = Server(self.config, test_mode=True)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        self.addCleanup(lambda: (server.shutdown(), server.server_close(), worker.join()))
        env = {k: v for k, v in os.environ.items() if not k.startswith("HGS_")}
        env.update(HGS_CONFIG_DIR=str(self.root), HOME=str(self.root), HGS_SELF="speech-fixture")
        def cli(data):
            result = subprocess.run([str(HGS), "speech", "--json"], input=json.dumps(data),
                                    text=True, capture_output=True, env=env, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(result.stdout)
        self.assertTrue(cli({"action": "providers"})["available"])
        request = self.submit()
        self.assertEqual(cli(request)["job_id"], request["request_id"])
        self.settle(server.jobs, request["request_id"])
        self.assertEqual(cli({"action": "status", "job_id": request["request_id"]})["text"], "Hello fixture")
        self.assertEqual(cli({"action": "stats"})["totals"]["submitted"], 1)
        self.assertEqual(self.config["socket_path"].stat().st_mode & 0o777, 0o600)

    def test_absent_service_is_graceful_and_old_socket_cannot_redirect(self):
        env = {k: v for k, v in os.environ.items() if not k.startswith("HGS_")}
        env.update(HGS_CONFIG_DIR=str(self.root), HOME=str(self.root), HGS_SELF="speech-fixture")
        result = subprocess.run([str(HGS), "speech", "--json"], input='{"action":"providers"}',
                                text=True, capture_output=True, env=env, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["profiles"], [])
        self.assertFalse(json.loads(result.stdout)["available"])

    def test_private_directory_and_single_service_required(self):
        jobs = self.jobs(start_worker=False)
        with self.assertRaisesRegex(ValueError, "already running"):
            Jobs(self.config)
        bad = self.root / "public"
        bad.mkdir(mode=0o755)
        with self.assertRaisesRegex(ValueError, "private"):
            Jobs(self.config | {"state_dir": bad})

    def test_guarded_peer_dispatch_preserves_speech_job_and_machine_identity(self):
        # Reuse the project's isolated SSH transport. Both hgs boundary checks
        # run normally; only the SSH process itself is synthetic.
        from test_mobile_peers import MobilePeers
        peer = MobilePeers("test_payload_reaches_existing_scoped_json_dispatch")
        peer.setUp()
        self.addCleanup(peer.doCleanups)
        (peer.root / "b/speech.json").write_text(self.config_path.read_text())
        server = Server(self.config, test_mode=True)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        self.addCleanup(lambda: (server.shutdown(), server.server_close(), worker.join()))
        request = self.submit()
        envelope = peer.request(["speech", "--json"], payload=request)
        accepted = peer.call("a", "swarm", "mobile-peer", "--json", data=envelope).json()
        self.assertEqual(accepted["job_id"], request["request_id"])
        self.settle(server.jobs, request["request_id"])
        envelope["payload"] = {"action": "status", "job_id": request["request_id"]}
        completed = peer.call("a", "swarm", "mobile-peer", "--json", data=envelope).json()
        self.assertEqual(completed["text"], "Hello fixture")
        envelope["target_machine_id"] = peer.ids["c"]
        peer.call("a", "swarm", "mobile-peer", "--json", data=envelope, ok=False)
        self.assertFalse((peer.root / "native-called").exists())
        self.assertEqual(server.jobs.handle({"action": "stats"})["totals"]["submitted"], 1)

    def test_targeted_help_reads_actual_remote_capabilities_without_tty(self):
        fake_bin = self.root / ".local/bin"
        fake_bin.mkdir(parents=True)
        ssh = fake_bin / "ssh"
        ssh.write_text("""#!/usr/bin/env python3
import json, pathlib, sys
pathlib.Path(__file__).with_name('ssh-call.json').write_text(json.dumps(sys.argv[1:]))
assert '-t' not in sys.argv and '-tt' not in sys.argv
assert 'BatchMode=yes' in sys.argv and 'ConnectTimeout=3' in sys.argv
assert sys.argv[-2:] == ['retained-host', '~/.local/bin/hgs --help']
print('usage: retained hgs (no speech ABI)')
""")
        ssh.chmod(0o700)
        env = {key: value for key, value in os.environ.items() if not key.startswith("HGS_")}
        env.update(HOME=str(self.root), HGS_CONFIG_DIR=str(self.root), HGS_SELF="this-host")
        for option in ("--help", "-h", "help"):
            remote = subprocess.run([str(HGS), "@retained-host", option], env=env, text=True,
                                    capture_output=True, timeout=10)
            self.assertEqual(remote.returncode, 0, remote.stderr)
            self.assertEqual(remote.stdout, "usage: retained hgs (no speech ABI)\n")
        call_path = fake_bin / "ssh-call.json"
        self.assertTrue(call_path.is_file())
        call_path.unlink()
        for prefix in ([], ["@this-host"]):
            local = subprocess.run([str(HGS), *prefix, "--help"], env=env, text=True,
                                   capture_output=True, timeout=10)
            self.assertEqual(local.returncode, 0, local.stderr)
            self.assertIn("speech --json", local.stdout)
            self.assertFalse(call_path.exists())
        preview = subprocess.run([str(HGS), "--dry-run", "@retained-host", "--help"], env=env,
                                 text=True, capture_output=True, timeout=10)
        self.assertEqual(preview.returncode, 0, preview.stderr)
        self.assertIn("ssh -o BatchMode=yes -o ConnectTimeout=3 retained-host", preview.stdout)
        self.assertFalse(call_path.exists())


if __name__ == "__main__":
    unittest.main()
