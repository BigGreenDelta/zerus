"""Structured native admission refusal; ordinary errors never authorize retry."""
import base64
import hashlib
import json
import struct

from . import speech

PROOF_FIELDS = {"schema", "state", "request_id", "message_id", "run_id", "conversation_id", "intent",
                "payload_sha256", "generation", "retry_token", "code", "detail"}


def message_id(payload):
    return payload.get("message_id", payload["request_id"])


def decoded(value, maximum):
    if not isinstance(value, str) or len(value) > ((maximum + 2) // 3) * 4:
        raise ValueError("Invalid voice intent encoding")
    data = base64.b64decode(value, validate=True)
    if len(data) > maximum:
        raise ValueError("Voice intent exceeds bounds")
    return data


def intent(payload):
    audio = payload["audio"]
    pcm = decoded(audio["data_base64"], 3_840_000)
    if not 3200 <= len(pcm) <= 3_840_000 or len(pcm) % 2:
        raise ValueError("Invalid voice PCM intent")
    header = struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", len(pcm) + 36, b"WAVE", b"fmt ", 16,
                         1, 1, 16000, 32000, 2, 16, b"data", len(pcm))
    digest = hashlib.sha256(header)
    digest.update(pcm)
    result = {"speech": payload["speech"], "audio": {key: audio[key] for key in ("format", "sample_rate", "channels")}}
    result["audio"].update(bytes=len(pcm) + 44, sha256=digest.hexdigest())
    del pcm
    if "video" in payload:
        video = payload["video"]
        data = decoded(video["data_base64"], 10 * 1024 * 1024)
        result["video"] = {key: video[key] for key in ("format", "duration_ms", "width", "height")}
        result["video"].update(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
        del data
        if "poster_base64" in video:
            result["video"]["poster_sha256"] = hashlib.sha256(decoded(video["poster_base64"], 8192)).hexdigest()
    return result


def valid_proof(value):
    try:
        if not isinstance(value, dict) or set(value) != PROOF_FIELDS or len(json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode()) > 4096:
            return False
        for key in ("request_id", "message_id", "retry_token"):
            speech.identity(value[key])
        def text(item, maximum):
            return isinstance(item, str) and 0 < len(item.encode()) <= maximum and not any(ord(c) < 32 or 127 <= ord(c) <= 159 for c in item)
        def digest(item):
            return isinstance(item, str) and len(item) == 64 and all(c in "0123456789abcdef" for c in item)
        native_intent = value["intent"]
        route, audio = native_intent["speech"], native_intent["audio"]
        if (set(native_intent) - {"speech", "audio", "video"} or not {"speech", "audio"} <= set(native_intent)
                or not {"machine_id", "profile_id"} <= set(route) or set(route) - {"machine_id", "profile_id", "profile_revision"}
                or not text(route["profile_id"], 64)
                or "profile_revision" in route and (type(route["profile_revision"]) is not int or not 1 <= route["profile_revision"] <= 9_007_199_254_740_991)
                or set(audio) != {"format", "sample_rate", "channels", "bytes", "sha256"}
                or audio["format"] != "pcm_s16le" or audio["sample_rate"] != 16000 or audio["channels"] != 1
                or type(audio["bytes"]) is not int or not 3244 <= audio["bytes"] <= 3_840_044 or audio["bytes"] % 2 or not digest(audio["sha256"])):
            return False
        speech.identity(route["machine_id"])
        if "video" in native_intent:
            media = native_intent["video"]
            if (not {"format", "duration_ms", "width", "height", "bytes", "sha256"} <= set(media)
                    or set(media) - {"format", "duration_ms", "width", "height", "bytes", "sha256", "poster_sha256"}
                    or media["format"] != "mp4" or not digest(media["sha256"])
                    or type(media["bytes"]) is not int or not 1 <= media["bytes"] <= 10 * 1024 * 1024
                    or type(media["duration_ms"]) is not int or not 100 <= media["duration_ms"] <= 60_000
                    or any(type(media[key]) is not int or not 16 <= media[key] <= 1920 for key in ("width", "height"))
                    or "poster_sha256" in media and not digest(media["poster_sha256"])):
                return False
        return (type(value["schema"]) is int and value["schema"] == 1 and value["state"] == "rejected_before_acceptance"
                and text(value["run_id"], 128) and text(value["conversation_id"], 256) and digest(value["payload_sha256"])
                and type(value["generation"]) is int and 1 <= value["generation"] <= 2**63-1
                and value["code"] in {"route_unavailable", "storage_full", "queue_full"} and text(value["detail"], 2000))
    except (ValueError, TypeError, KeyError):
        return False


def validate_retry(payload):
    if "message_id" not in payload and "rejection" not in payload:
        return
    proof = payload.get("rejection")
    speech.identity(payload.get("message_id"))
    if (payload["message_id"] == payload["request_id"] or not valid_proof(proof)
            or proof["message_id"] != payload["message_id"] or proof["run_id"] != payload["expected_run_id"]
            or proof["conversation_id"] != payload["expected_conversation_id"] or proof["intent"] != intent(payload)):
        raise ValueError("Voice retry requires exact durable refusal evidence and unchanged recording")


def result_valid(result, payload):
    proof = result.get("rejection") if isinstance(result, dict) else None
    return (isinstance(result, dict) and result.get("schema") == 1 and valid_proof(proof)
            and proof["request_id"] == payload["request_id"] and proof["message_id"] == message_id(payload)
            and proof["run_id"] == payload["expected_run_id"] and proof["conversation_id"] == payload["expected_conversation_id"]
            and ("audio" not in payload or proof["intent"] == intent(payload)))
