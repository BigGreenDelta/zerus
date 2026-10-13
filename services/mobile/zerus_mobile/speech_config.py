"""Host-owned speech settings; mobile credentials are sealed before queuing."""
import base64
import binascii
import re
import uuid

OPERATIONS = frozenset({"speech_config", "speech_profile_upsert", "speech_profile_remove",
                        "speech_profile_test", "speech_config_status"})
READS = frozenset({"speech_config", "speech_config_status"})
MUTATIONS = OPERATIONS - READS
READ_SQL = "('speech_config','speech_config_status')"
HELP_MARKER = "Speech profile management v1"
ACTIONS = {"speech_config": "config_list", "speech_profile_upsert": "profile_upsert",
           "speech_profile_remove": "profile_remove", "speech_profile_test": "profile_test",
           "speech_config_status": "config_status"}
ALGORITHM = "P256-HKDF-SHA256-A256GCM"
MAX_REVISION = 2**53 - 1


def fields(value, required=(), optional=()):
    if not isinstance(value, dict) or not set(required) <= set(value) or set(value) - set(required) - set(optional):
        raise ValueError("invalid speech configuration fields")


def bounded(value, maximum, empty=False):
    if (not isinstance(value, str) or len(value.encode()) > maximum or (not value and not empty)
            or any(ord(c) < 32 or ord(c) == 127 for c in value)):
        raise ValueError("invalid speech configuration text")


def identity(value):
    try:
        parsed = uuid.UUID(value) if isinstance(value, str) else None
    except ValueError:
        parsed = None
    if parsed is None or not parsed.int or str(parsed) != value:
        raise ValueError("invalid speech configuration identity")


def revision(value):
    if type(value) is not int or not 0 <= value <= MAX_REVISION:
        raise ValueError("invalid speech configuration revision")


def profile_id(value):
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", value):
        raise ValueError("invalid speech profile identity")


def encoded(value, minimum, maximum):
    if not isinstance(value, str) or len(value) > ((maximum + 2) // 3) * 4:
        raise ValueError("invalid sealed speech credential")
    try:
        raw = base64.b64decode(value, validate=True)
    except (binascii.Error, UnicodeError):
        raise ValueError("invalid sealed speech credential") from None
    if not minimum <= len(raw) <= maximum or base64.b64encode(raw).decode() != value:
        raise ValueError("invalid sealed speech credential")


def profile(value):
    adapter = value.get("adapter") if isinstance(value, dict) else None
    if adapter == "gigaam":
        fields(value, ("id", "name", "adapter", "model", "language", "device"))
        if value["device"] not in {"cpu", "cuda", "cuda:0", "cuda:1", "mps"}:
            raise ValueError("invalid speech inference device")
    elif adapter == "openai_http":
        fields(value, ("id", "name", "adapter", "model", "language", "base_url", "allow_insecure_http"))
        bounded(value["base_url"], 2048)
        if type(value["allow_insecure_http"]) is not bool:
            raise ValueError("invalid speech HTTP transport choice")
    else:
        raise ValueError("unsupported speech adapter")
    profile_id(value["id"])
    bounded(value["name"], 128)
    bounded(value["model"], 128)
    bounded(value["language"], 32, empty=True)
    if value["language"] and not re.fullmatch(r"[a-z]{2}", value["language"]):
        raise ValueError("speech language must be an ISO 639-1 code")


def credential(value):
    mode = value.get("mode") if isinstance(value, dict) else None
    if mode in {"keep", "remove"}:
        fields(value, ("mode",))
        return
    if mode != "replace":
        raise ValueError("invalid speech credential mode")
    # Plaintext credentials are intentionally forbidden at the relay boundary.
    fields(value, ("mode", "sealed_value"))
    sealed = value["sealed_value"]
    fields(sealed, ("key_id", "algorithm", "ephemeral_public_key", "nonce", "ciphertext"))
    if (sealed["algorithm"] != ALGORITHM or not isinstance(sealed["key_id"], str)
            or not re.fullmatch(r"[0-9a-f]{64}", sealed["key_id"])):
        raise ValueError("invalid sealed speech credential")
    encoded(sealed["ephemeral_public_key"], 1, 128)
    encoded(sealed["nonce"], 12, 12)
    encoded(sealed["ciphertext"], 17, 4112)


def validate(operation, payload, request_id):
    if operation == "speech_config":
        fields(payload)
        return
    if operation == "speech_config_status":
        fields(payload, ("operation_id", "expected_machine_id"))
        identity(payload["operation_id"])
    elif operation in MUTATIONS:
        common = ("request_id", "expected_machine_id")
        if operation == "speech_profile_upsert":
            fields(payload, common + ("expected_revision", "profile", "credential"))
            revision(payload["expected_revision"])
            profile(payload["profile"])
            credential(payload["credential"])
        elif operation == "speech_profile_remove":
            fields(payload, common + ("expected_revision", "profile_id"))
            revision(payload["expected_revision"])
            profile_id(payload["profile_id"])
        else:
            fields(payload, common + ("expected_profile_revision", "profile_id"))
            revision(payload["expected_profile_revision"])
            profile_id(payload["profile_id"])
        identity(payload["request_id"])
        if payload["request_id"] != request_id:
            raise ValueError("speech configuration request identity does not match")
    else:
        raise ValueError("unsupported speech configuration operation")
    identity(payload["expected_machine_id"])


def result_valid(operation, result, payload):
    """Reject accidental secret/raw-body fields before durable connector receipts."""
    try:
        if not isinstance(result, dict) or type(result.get("schema")) is not int or result["schema"] != 1:
            return False
        identity(result.get("machine_id"))
        if operation == "speech_config":
            fields(result, ("schema", "machine_id", "revision", "service", "adapters", "profiles", "credential_key"))
            revision(result["revision"])
            fields(result["service"], ("available",), ("reason",))
            if type(result["service"]["available"]) is not bool:
                return False
            if "reason" in result["service"]:
                bounded(result["service"]["reason"], 512, empty=True)
            if not isinstance(result["profiles"], list) or len(result["profiles"]) > 32:
                return False
            for item in result["profiles"]:
                fields(item, ("id", "revision", "name", "adapter", "model", "language", "credential", "available", "max_duration_ms"),
                       ("device", "base_url", "allow_insecure_http", "reason"))
                profile_id(item["id"])
                revision(item["revision"])
                for key, maximum in (("name", 128), ("adapter", 32), ("model", 128), ("language", 32)):
                    bounded(item[key], maximum, empty=key == "language")
                fields(item["credential"], ("configured",))
                if type(item["credential"]["configured"]) is not bool or type(item["available"]) is not bool:
                    return False
                if type(item["max_duration_ms"]) is not int or not 1 <= item["max_duration_ms"] <= 120000:
                    return False
                for key, maximum in (("device", 16), ("base_url", 2048), ("reason", 512)):
                    if key in item:
                        bounded(item[key], maximum, empty=key == "reason")
                if "allow_insecure_http" in item and type(item["allow_insecure_http"]) is not bool:
                    return False
            if not isinstance(result["adapters"], list) or len(result["adapters"]) > 8:
                return False
            for adapter in result["adapters"]:
                fields(adapter, ("id",), ("models", "devices"))
                bounded(adapter["id"], 32)
                for key in ("models", "devices"):
                    if key in adapter:
                        if not isinstance(adapter[key], list) or len(adapter[key]) > 32:
                            return False
                        for item in adapter[key]:
                            bounded(item, 128)
            key = result["credential_key"]
            fields(key, ("key_id", "algorithm", "public_key_spki"))
            if key["algorithm"] != ALGORITHM or not isinstance(key["key_id"], str) or not re.fullmatch(r"[0-9a-f]{64}", key["key_id"]):
                return False
            encoded(key["public_key_spki"], 1, 128)
        else:
            fields(result, ("schema", "machine_id", "request_id", "state"),
                   ("revision", "profile_id", "profile_revision", "error_code", "error"))
            if result["machine_id"] != payload["expected_machine_id"]:
                return False
            if result["request_id"] != payload.get("operation_id", payload.get("request_id")):
                return False
            if result["state"] not in {"completed", "failed", "running", "uncertain"}:
                return False
            for key in ("revision", "profile_revision"):
                if key in result:
                    revision(result[key])
            if "profile_id" in result:
                profile_id(result["profile_id"])
            for key in ("error_code", "error"):
                if key in result:
                    bounded(result[key], 512, empty=True)
        return True
    except (ValueError, TypeError, KeyError, UnicodeError):
        return False
