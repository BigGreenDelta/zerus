"""Private stopped-conversation admission contracts; no worker, ASR or input runs."""
import base64
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import struct
import subprocess
import unittest
import uuid
from contextlib import closing

import test_public_history_contract as history_fixture

HGS = Path(os.environ.get("HGS_TEST_BIN", Path(__file__).resolve().parents[1] / "target/debug/hgs")).resolve()


class VoiceAdmission(unittest.TestCase):
    setUp = history_fixture.PublicHistoryContract.setUp
    write_binding = history_fixture.PublicHistoryContract.write_binding
    write_source = history_fixture.PublicHistoryContract.write_source

    def payload(self):
        return dict(action="submit", request_id=str(uuid.uuid4()), expected_run_id=self.run,
            expected_conversation_id=self.conversation,
            speech=dict(machine_id="22222222-2222-4222-8222-222222222222", profile_id="fixture"),
            audio=dict(format="pcm_s16le", sample_rate=16000, channels=1,
                       data_base64=base64.b64encode(bytes(3200)).decode()))

    def call(self, payload, rejected=False):
        result = subprocess.run([str(HGS), "voice", self.name, "--json"], input=json.dumps(payload),
            env=self.env, capture_output=True, text=True, timeout=15)
        if rejected:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertNotIn('"rejection"', result.stdout)
            return result
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn(str(self.root), result.stdout)
        return json.loads(result.stdout)

    def counts(self):
        with closing(sqlite3.connect(self.state / "voice/messages.sqlite3")) as db:
            return tuple(db.execute("SELECT count(*) FROM " + table).fetchone()[0]
                         for table in ("messages", "voice_refusals", "voice_admissions"))

    def test_definitive_route_refusal_is_durable_and_status_not_found_is_not_evidence(self):
        payload = self.payload()
        result = self.call(payload)
        self.assertEqual(result["schema"], 1)
        proof = result["rejection"]
        self.assertEqual(proof["state"], "rejected_before_acceptance")
        self.assertEqual(proof["request_id"], payload["request_id"])
        self.assertEqual(proof["message_id"], payload["request_id"])
        self.assertEqual(proof["run_id"], self.run)
        self.assertEqual(proof["conversation_id"], self.conversation)
        self.assertEqual(proof["intent"]["speech"], payload["speech"])
        wav = struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", 3236, b"WAVE", b"fmt ", 16,
                          1, 1, 16000, 32000, 2, 16, b"data", 3200) + bytes(3200)
        self.assertEqual(proof["intent"]["audio"]["sha256"], hashlib.sha256(wav).hexdigest())
        self.assertEqual(self.call(payload), result)
        self.assertEqual(self.counts(), (0, 1, 1))
        status = dict(action="status", request_id=str(uuid.uuid4()), message_id=payload["request_id"],
                      expected_run_id=self.run, expected_conversation_id=self.conversation)
        checked = self.call(status)["rejection"]
        self.assertEqual(checked["request_id"], status["request_id"])
        self.assertEqual(checked["generation"], proof["generation"])
        self.assertEqual(checked["retry_token"], proof["retry_token"])
        status["message_id"] = str(uuid.uuid4())
        self.call(status, rejected=True)
        self.assertEqual(self.counts(), (0, 1, 1))

    def test_explicit_retry_has_new_operation_but_retains_native_message_and_original_intent(self):
        payload = self.payload()
        first = self.call(payload)["rejection"]
        retry = dict(payload, request_id=str(uuid.uuid4()), message_id=payload["request_id"], rejection=first)
        second = self.call(retry)["rejection"]
        self.assertEqual(second["message_id"], first["message_id"])
        self.assertEqual(second["intent"], first["intent"])
        self.assertEqual(second["payload_sha256"], first["payload_sha256"])
        self.assertGreater(second["generation"], first["generation"])
        self.assertEqual(self.call(retry)["rejection"], second)
        # A delayed original envelope retains its original receipt and cannot
        # create a native message after a later explicit attempt.
        self.assertEqual(self.call(payload)["rejection"], first)
        self.assertEqual(self.counts(), (0, 1, 2))

    def test_mismatched_retry_target_provider_media_and_reused_operation_never_admit(self):
        payload = self.payload()
        proof = self.call(payload)["rejection"]
        retry = dict(payload, request_id=str(uuid.uuid4()), message_id=payload["request_id"], rejection=proof)
        for field, value in (("expected_run_id", "another-run"), ("expected_conversation_id", "another-conversation"),
                             ("message_id", str(uuid.uuid4())),
                             ("speech", dict(payload["speech"], profile_id="another-profile")),
                             ("audio", dict(payload["audio"], data_base64=base64.b64encode(bytes([1]) * 3200).decode()))):
            with self.subTest(field=field):
                self.call(dict(retry, **{field: value}), rejected=True)
        second = self.call(retry)["rejection"]
        self.call(dict(retry, rejection=second), rejected=True)
        self.assertEqual(self.counts(), (0, 1, 2))

    def test_video_refusal_preserves_both_media_and_original_native_identity(self):
        payload = self.payload()
        mp4 = b"".join((8 + len(data)).to_bytes(4, "big") + kind + data
                       for kind, data in ((b"ftyp", b"isom0000"), (b"moov", b""), (b"mdat", b"synthetic")))
        payload["video"] = dict(format="mp4", duration_ms=100, width=480, height=480,
                                data_base64=base64.b64encode(mp4).decode())
        proof = self.call(payload)["rejection"]
        self.assertEqual(proof["intent"]["video"], dict(format="mp4", duration_ms=100, width=480, height=480,
                                                      bytes=len(mp4), sha256=hashlib.sha256(mp4).hexdigest()))
        retry = dict(payload, request_id=str(uuid.uuid4()), message_id=payload["request_id"], rejection=proof)
        self.call(dict(retry, video=dict(payload["video"], width=640)), rejected=True)
        second = self.call(retry)["rejection"]
        self.assertEqual(second["message_id"], payload["request_id"])
        self.assertEqual(second["intent"], proof["intent"])
        self.assertEqual(second["payload_sha256"], proof["payload_sha256"])
        self.assertEqual(self.counts(), (0, 1, 2))

    def test_legacy_short_pcm_gets_scoped_refusal_without_changing_ui_capture_minimum(self):
        accepted = 0
        for size in (2, 4, 320, 3198, 3200, 3_840_000):
            with self.subTest(pcm_bytes=size):
                payload = self.payload()
                payload["audio"]["data_base64"] = base64.b64encode(bytes(size)).decode()
                proof = self.call(payload)["rejection"]
                accepted += 1
                self.assertEqual(proof["intent"]["audio"]["bytes"], size + 44)
                self.assertEqual(proof["message_id"], payload["request_id"])
                self.assertEqual(proof["generation"], 1)
                self.assertEqual(self.counts(), (0, accepted, accepted))
        before = self.counts()
        for size in (0, 1, 3, 3199, 3_840_002):
            with self.subTest(invalid_pcm_bytes=size):
                payload = self.payload()
                payload["audio"]["data_base64"] = base64.b64encode(bytes(size)).decode()
                self.call(payload, rejected=True)
                self.assertEqual(self.counts(), before)


if __name__ == "__main__":
    unittest.main()
