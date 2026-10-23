"""Durable receipts and one inference worker; accepted jobs never replay on restart."""
import fcntl
from datetime import datetime, timedelta, timezone
import hashlib
import json
import logging
import os
from pathlib import Path
import re
import sqlite3
import stat
import threading
import time
import uuid

from .audio import MAX_DURATION_MS, decode_audio
from .config import profiles as read_profiles
from .providers.base import Cancelled, ProviderError
from .providers.gigaam import GigaAM
from .providers.openai_http import OpenAIHTTP

COUNTERS = ("submitted", "completed", "failed", "cancelled", "uncertain", "audio_ms", "processing_ms", "queue_ms")
RETENTION_DAYS = 90


class AdmissionError(ValueError):
    def __init__(self, code, message):
        self.code = code
        super().__init__(message)


def empty_counters():
    return dict.fromkeys(COUNTERS, 0) | {"error_codes": {}}


def private_directory(path: Path):
    path.mkdir(parents=True, mode=0o700, exist_ok=True)
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o077:
        raise ValueError("Speech service directories must be private to this user (mode 0700)")


def canonical_uuid(value):
    if not isinstance(value, str) or str(uuid.UUID(value)) != value or not uuid.UUID(value).int:
        raise ValueError("Speech job identity must be a canonical lowercase UUID")
    return value


class Fixture:
    """Synthetic provider, registered exclusively with explicit --test-mode."""
    def available(self, profile):
        return True, ""

    def transcribe(self, profile, pcm, cancelled):
        end = time.monotonic() + min(10, float(profile.get("delay", 0)))
        while time.monotonic() < end:
            if cancelled():
                raise Cancelled()
            time.sleep(0.01)
        if profile.get("fail"):
            raise RuntimeError("Synthetic failure")
        return profile.get("text", "Fixture transcript")


class Jobs:
    def __init__(self, config, *, test_mode=False, start_worker=True):
        self.config = config
        self.state_dir = config["state_dir"]
        private_directory(self.state_dir)
        self._owner = open(self.state_dir / "service.lock", "a+b")
        os.chmod(self.state_dir / "service.lock", 0o600)
        try:
            fcntl.flock(self._owner, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            self._owner.close()
            raise ValueError("Speech service is already running") from None
        self.lock = threading.RLock()
        self.changed = threading.Condition(self.lock)
        self.stopping = False
        self.providers = {"gigaam": GigaAM(self.state_dir), "openai_http": OpenAIHTTP()}
        if test_mode:
            self.providers["fixture"] = Fixture()
        self.profiles = {p["id"]: p for p in config["profiles"]}
        self.db = sqlite3.connect(self.state_dir / "jobs.sqlite3", check_same_thread=False)
        os.chmod(self.state_dir / "jobs.sqlite3", 0o600)
        self.db.row_factory = sqlite3.Row
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute("PRAGMA synchronous=FULL")
        self.db.execute("""CREATE TABLE IF NOT EXISTS jobs (
            id TEXT PRIMARY KEY, fingerprint TEXT NOT NULL, profile TEXT NOT NULL,
            state TEXT NOT NULL, created REAL NOT NULL, updated REAL NOT NULL,
            text TEXT, error TEXT, started REAL)""")
        if "started" not in {row[1] for row in self.db.execute("PRAGMA table_info(jobs)")}:
            self.db.execute("ALTER TABLE jobs ADD COLUMN started REAL")
        if "error_code" not in {row[1] for row in self.db.execute("PRAGMA table_info(jobs)")}:
            self.db.execute("ALTER TABLE jobs ADD COLUMN error_code TEXT")
        columns = {row[1] for row in self.db.execute("PRAGMA table_info(jobs)")}
        if "profile_snapshot" not in columns:
            self.db.execute("ALTER TABLE jobs ADD COLUMN profile_snapshot TEXT")
        if "purpose" not in columns:
            self.db.execute("ALTER TABLE jobs ADD COLUMN purpose TEXT NOT NULL DEFAULT 'voice'")
        self.db.execute("CREATE TABLE IF NOT EXISTS daily_stats (day TEXT,profile TEXT,counters TEXT NOT NULL,PRIMARY KEY(day,profile))")
        with self.db:
            now = time.time()
            for row in self.db.execute("SELECT * FROM jobs WHERE state IN ('queued','running')").fetchall():
                self._count(row["profile"], row["created"], {"uncertain": 1}, "service_restarted", purpose=row["purpose"])
            self.db.execute("UPDATE jobs SET state='uncertain', profile_snapshot=NULL, error='Speech service restarted; record again', error_code='service_restarted', updated=? WHERE state IN ('queued','running')", (now,))
        for pattern in ("*.pcm", "*.wav"):
            for path in self.state_dir.glob(pattern):
                path.unlink(missing_ok=True)
        self.worker = threading.Thread(target=self._work, name="speech-inference", daemon=True)
        if start_worker:
            self.worker.start()

    def close(self):
        with self.changed:
            self.stopping = True
            self.changed.notify_all()
        if self.worker.is_alive():
            self.worker.join(timeout=10)
        if not self.worker.is_alive():
            self.db.close()
            self._owner.close()

    def _expire(self):
        with self.db:
            self.db.execute("UPDATE jobs SET state='uncertain',text=NULL,error='Speech result expired; record again',error_code='result_expired' WHERE updated < ? AND state IN ('completed','failed')", (time.time() - self.config["result_ttl_seconds"],))
            cutoff = (datetime.now(timezone.utc) - timedelta(days=RETENTION_DAYS - 1)).strftime("%Y-%m-%d")
            self.db.execute("DELETE FROM daily_stats WHERE day < ?", (cutoff,))

    def _count(self, profile, created, increments, error_code=None, *, purpose="voice"):
        """Called inside the same transaction as the durable job transition."""
        if purpose != "voice" or not self.config.get("analytics_enabled", True):
            return
        day = datetime.fromtimestamp(created, timezone.utc).strftime("%Y-%m-%d")
        previous = self.db.execute("SELECT counters FROM daily_stats WHERE day=? AND profile=?", (day, profile)).fetchone()
        counters = json.loads(previous[0]) if previous else empty_counters()
        for key, increment in increments.items():
            counters[key] += max(0, int(increment))
        if error_code:
            counters["error_codes"][error_code] = counters["error_codes"].get(error_code, 0) + 1
        self.db.execute("INSERT INTO daily_stats VALUES (?,?,?) ON CONFLICT(day,profile) DO UPDATE SET counters=excluded.counters", (day, profile, json.dumps(counters, separators=(",", ":"))))
        # Configuration can rename profiles over time. Bound historical rows as
        # well as age, including on hosts that repeatedly change profile IDs.
        self.db.execute("DELETE FROM daily_stats WHERE rowid IN (SELECT rowid FROM daily_stats ORDER BY day DESC,rowid DESC LIMIT -1 OFFSET 2880)")

    def stats(self):
        days, totals = [], empty_counters()
        for row in self.db.execute("SELECT * FROM daily_stats ORDER BY day,profile"):
            counters = json.loads(row["counters"])
            days.append({"day": row["day"], "profile_id": row["profile"], **counters})
            for key in COUNTERS:
                totals[key] += counters[key]
            for code, count in counters["error_codes"].items():
                totals["error_codes"][code] = totals["error_codes"].get(code, 0) + count
        return {"schema": 1, "enabled": self.config.get("analytics_enabled", True),
                "retention_days": RETENTION_DAYS, "days": days, "totals": totals}

    def _result(self, row):
        result = {"schema": 1, "job_id": row["id"], "state": row["state"]}
        if row["text"] is not None and row["purpose"] == "voice":
            result["text"] = row["text"]
        if row["error"]:
            result["error"] = row["error"]
        if row["error_code"]:
            result["error_code"] = row["error_code"]
        return result

    def discovery(self):
        profiles = []
        for profile in read_profiles(self.config).values():
            provider = self.providers.get(profile["adapter"])
            available, reason = provider.available(profile) if provider else (False, "Provider adapter is not installed")
            public = {k: profile[k] for k in ("id", "name", "adapter", "model", "language")}
            public.update(available=available, max_duration_ms=MAX_DURATION_MS)
            if "revision" in profile:
                public["profile_revision"] = profile["revision"]
            if reason:
                public["reason"] = reason
            profiles.append(public)
        return {"schema": 1, "profiles": profiles, "available": any(p["available"] for p in profiles)}

    def handle(self, request):
        if not isinstance(request, dict):
            raise ValueError("Speech request must be an object")
        action = request.get("action")
        fields = {"providers": {"action"}, "stats": {"action"}, "submit": {"action", "request_id", "profile_id", "audio"},
                  "probe": {"action", "request_id", "profile_id", "profile_revision"},
                  "status": {"action", "job_id"}, "cancel": {"action", "job_id"}}
        if (not isinstance(action, str) or action not in fields
                or set(request) != fields[action] | ({"profile_revision"} if action == "submit" and "profile_revision" in request else set())):
            raise ValueError("Unknown speech action or invalid request fields")
        if action == "providers":
            return self.discovery()
        if action == "stats":
            with self.lock:
                self._expire()
                return self.stats()
        job_id = canonical_uuid(request.get("request_id" if action in {"submit", "probe"} else "job_id"))
        with self.changed:
            self._expire()
            if action in {"submit", "probe"}:
                return self._submit(job_id, request, probe=action == "probe")
            row = self.db.execute("SELECT * FROM jobs WHERE id=?", (job_id,)).fetchone()
            if row is None:
                return {"schema": 1, "job_id": job_id, "state": "failed", "error_code": "not_found", "error": "Speech job was not found"}
            if action == "cancel" and row["state"] in {"queued", "running"}:
                with self.db:
                    now = time.time()
                    self.db.execute("UPDATE jobs SET state='cancelled',text=NULL,profile_snapshot=NULL,updated=? WHERE id=?", (now, job_id))
                    self._count(row["profile"], row["created"], {"cancelled": 1,
                        "processing_ms": (now - row["started"]) * 1000 if row["started"] else 0}, purpose=row["purpose"])
                (self.state_dir / (job_id + ".pcm")).unlink(missing_ok=True)
                row = self.db.execute("SELECT * FROM jobs WHERE id=?", (job_id,)).fetchone()
            result = self._result(row)
            if action == "cancel":
                result.pop("text", None)
            return result

    def _submit(self, job_id, request, *, probe=False):
        profile_id = request["profile_id"]
        if not isinstance(profile_id, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", profile_id):
            raise ValueError("Invalid speech profile ID")
        # An explicit test uses synthetic sound, never a person's recording.
        pcm = b"\x00\x10\x00\xf0" * 4000 if probe else decode_audio(request["audio"])
        # Fingerprint is independent of configuration changes: retrying accepted
        # input always returns its original receipt and can never select new work.
        suffix = (b"\0probe" if probe else b"") + (b"\0revision=" + str(request["profile_revision"]).encode() if "profile_revision" in request else b"")
        fingerprint = hashlib.sha256(profile_id.encode() + b"\0" + pcm + suffix).hexdigest()
        previous = self.db.execute("SELECT * FROM jobs WHERE id=?", (job_id,)).fetchone()
        if previous:
            if previous["fingerprint"] != fingerprint:
                return {"schema": 1, "job_id": job_id, "state": "failed", "error_code": "request_conflict", "error": "Speech request ID was already used for different input"}
            result = self._result(previous)
            result.pop("text", None)
            return result
        profile = read_profiles(self.config).get(profile_id)
        if profile is None:
            return {"schema": 1, "job_id": job_id, "state": "failed", "error_code": "profile_not_found", "error": "Speech profile is no longer available"}
        if "profile_revision" in request and (type(request["profile_revision"]) is not int
                or request["profile_revision"] < 1 or request["profile_revision"] != profile.get("revision")):
            return {"schema": 1, "job_id": job_id, "state": "failed", "error_code": "profile_revision_conflict", "error": "Speech profile changed; review the selected provider"}
        provider = self.providers.get(profile["adapter"])
        available, reason = provider.available(profile) if provider else (False, "Provider adapter is not installed")
        if not available:
            raise ValueError(reason)
        if self.db.execute("SELECT count(*) FROM jobs WHERE state IN ('queued','running')").fetchone()[0] >= self.config["max_queue"]:
            raise AdmissionError("queue_full", "Speech queue is full; try a new recording later")
        if self.db.execute("SELECT count(*) FROM jobs").fetchone()[0] >= self.config["max_receipts"]:
            raise AdmissionError("receipt_storage_full", "Speech receipt storage is full; contact this computer's owner")
        path = self.state_dir / (job_id + ".pcm")
        try:
            with path.open("xb") as output:
                os.chmod(path, 0o600)
                output.write(pcm)
                output.flush()
                os.fsync(output.fileno())
            now = time.time()
            with self.db:
                purpose = "probe" if probe else "voice"
                self.db.execute("INSERT INTO jobs (id,fingerprint,profile,state,created,updated,profile_snapshot,purpose) VALUES (?, ?, ?, 'queued', ?, ?, ?, ?)", (job_id, fingerprint, profile_id, now, now, json.dumps(profile), purpose))
                self._count(profile_id, now, {"submitted": 1, "audio_ms": len(pcm) * 1000 // 32000}, purpose=purpose)
        except BaseException:
            path.unlink(missing_ok=True)
            raise
        self.changed.notify()
        return {"schema": 1, "job_id": job_id, "state": "queued"}

    def _cancelled(self, job_id):
        with self.lock:
            return self.stopping or self.db.execute("SELECT state FROM jobs WHERE id=?", (job_id,)).fetchone()[0] != "running"

    def _work(self):
        while True:
            with self.changed:
                self._expire()
                if self.stopping:
                    return
                row = self.db.execute("SELECT * FROM jobs WHERE state='queued' ORDER BY created LIMIT 1").fetchone()
                if row is None:
                    self.changed.wait(timeout=60)
                    continue
                job_id = row["id"]
                path = self.state_dir / (job_id + ".pcm")
                with self.db:
                    started = time.time()
                    self.db.execute("UPDATE jobs SET state='running',updated=?,started=? WHERE id=?", (started, started, job_id))
                    self._count(row["profile"], row["created"], {"queue_ms": (started - row["created"]) * 1000}, purpose=row["purpose"])
                try:
                    pcm = path.read_bytes()
                except OSError:
                    pcm = None
                path.unlink(missing_ok=True)
            state, text, error, error_code = "completed", None, None, None
            try:
                if pcm is None:
                    raise ValueError("Audio is unavailable")
                profile = json.loads(row["profile_snapshot"])
                text = self.providers[profile["adapter"]].transcribe(profile, pcm, lambda: self._cancelled(job_id))
                if not isinstance(text, str) or len(text.encode("utf-8")) > 32000:
                    raise ValueError("Transcript exceeds the result limit")
            except Cancelled:
                state = "cancelled"
            except ProviderError as exc:
                state, error, error_code = "failed", "The HTTP speech provider could not complete recognition", exc.code
            except Exception as exc:
                logging.warning("Speech inference failed (%s)", type(exc).__name__)
                state, error, error_code = "failed", "Speech recognition failed (" + type(exc).__name__ + ")", "inference_failed"
            finally:
                pcm = None
            with self.changed:
                with self.db:
                    now = time.time()
                    changed = self.db.execute("UPDATE jobs SET state=?,text=?,error=?,error_code=?,profile_snapshot=NULL,updated=? WHERE id=? AND state='running'", (state, text if row["purpose"] == "voice" else None, error, error_code, now, job_id)).rowcount
                    if changed:
                        self._count(row["profile"], row["created"], {state: 1, "processing_ms": (now - started) * 1000}, error_code, purpose=row["purpose"])
