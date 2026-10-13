"""Bounded computer-scoped speech transport; audio never enters native input."""
import base64
import binascii
import re
import uuid
from .speech_config import revision

OPERATIONS = frozenset({"speech_profiles", "speech_stats", "speech_submit", "speech_status", "speech_cancel"})
READS = frozenset({"speech_profiles", "speech_stats", "speech_status"})
READ_SQL = "('speech_profiles','speech_stats','speech_status')"
MAX_AUDIO_BYTES = 16000 * 2 * 120
HELP_MARKER = "speech --json"
ACTIONS = {"speech_profiles": "providers", "speech_stats": "stats", "speech_submit": "submit", "speech_status": "status", "speech_cancel": "cancel"}


def identity(value):
    try:
        parsed = uuid.UUID(value) if isinstance(value, str) else None
    except ValueError:
        parsed = None
    if parsed is None or not parsed.int or str(parsed) != value:
        raise ValueError("invalid speech job identity")


def validate(operation, payload, request_id):
    if not isinstance(payload, dict):
        raise ValueError("invalid speech payload")
    if operation in {"speech_profiles", "speech_stats"}:
        if payload:
            raise ValueError("speech profiles takes no fields")
    elif operation in {"speech_status", "speech_cancel"}:
        if set(payload) != {"job_id"}:
            raise ValueError("speech job requires only its identity")
        identity(payload["job_id"])
    elif operation == "speech_submit":
        if not {"request_id", "profile_id", "audio"} <= set(payload) or set(payload) - {"request_id", "profile_id", "profile_revision", "audio"} or payload["request_id"] != request_id:
            raise ValueError("speech request identity does not match")
        identity(request_id)
        if "profile_revision" in payload:
            revision(payload["profile_revision"])
        profile = payload["profile_id"]
        if not isinstance(profile, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", profile):
            raise ValueError("invalid speech profile")
        audio = payload["audio"]
        if (not isinstance(audio, dict) or set(audio) != {"format", "sample_rate", "channels", "data_base64"}
                or audio["format"] != "pcm_s16le" or type(audio["sample_rate"]) is not int
                or audio["sample_rate"] != 16000 or type(audio["channels"]) is not int or audio["channels"] != 1):
            raise ValueError("speech requires mono PCM16 at 16000 Hz")
        encoded = audio["data_base64"]
        if not isinstance(encoded, str) or not 4 <= len(encoded) <= (MAX_AUDIO_BYTES + 2) // 3 * 4:
            raise ValueError("speech recording exceeds 120 seconds")
        size = 0
        try:
            for offset in range(0, len(encoded), 65536):
                part = encoded[offset:offset + 65536]
                decoded = base64.b64decode(part, validate=True)
                if base64.b64encode(decoded).decode("ascii") != part or ("=" in part and offset + len(part) != len(encoded)):
                    raise ValueError("invalid speech audio encoding")
                size += len(decoded)
        except (binascii.Error, UnicodeError):
            raise ValueError("invalid speech audio encoding") from None
        if not 2 <= size <= MAX_AUDIO_BYTES or size % 2:
            raise ValueError("invalid speech audio size")
    else:
        raise ValueError("unsupported speech operation")


def result_valid(operation, result, payload):
    if not isinstance(result, dict) or result.get("schema") != 1:
        return False
    if operation == "speech_stats":
        return True
    if operation == "speech_profiles":
        return (isinstance(result.get("profiles"), list) and len(result["profiles"]) <= 32
                and type(result.get("available")) is bool)
    expected = payload.get("request_id") if operation == "speech_submit" else payload.get("job_id")
    return (result.get("job_id") == expected
            and result.get("state") in {"queued", "running", "completed", "failed", "cancelled", "uncertain"}
            and ("text" not in result or isinstance(result["text"], str) and len(result["text"].encode()) <= 65536))
