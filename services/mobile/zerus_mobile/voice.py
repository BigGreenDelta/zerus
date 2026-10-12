"""Conversation-scoped durable voice messages; native hgs owns their lifetime."""
from . import speech

OPERATIONS = frozenset({"voice_profiles", "voice_submit", "voice_status", "voice_cancel"})
READS = frozenset({"voice_profiles", "voice_status"})
READ_SQL = "('voice_profiles','voice_status')"
HELP_MARKER = "voice <session> --json"
ACTIONS = {operation: operation.removeprefix("voice_") for operation in OPERATIONS}
STATES = frozenset({"queued", "transcribing", "ready", "waiting_agent", "delivering",
                    "submitted", "failed", "cancelled", "uncertain"})


def validate(operation, payload, request_id):
    common = {"request_id", "expected_run_id", "expected_conversation_id"}
    extra = {"speech", "audio"} if operation == "voice_submit" else (
        {"message_id"} if operation in {"voice_status", "voice_cancel"} else set())
    if (operation not in OPERATIONS or not isinstance(payload, dict)
            or set(payload) != common | extra or payload.get("request_id") != request_id):
        raise ValueError("invalid voice message fields")
    speech.identity(request_id)
    for field, limit in (("expected_run_id", 128), ("expected_conversation_id", 256)):
        value = payload[field]
        if (not isinstance(value, str) or len(value) > limit
                or (not value and (field == "expected_run_id" or operation != "voice_profiles"))
                or any(ord(c) < 32 or ord(c) == 127 for c in value)):
            raise ValueError("invalid voice conversation identity")
    if operation == "voice_submit":
        route = payload["speech"]
        if not isinstance(route, dict) or not {"machine_id", "profile_id"} <= set(route) or set(route) - {"machine_id", "profile_id", "profile_revision"}:
            raise ValueError("invalid voice speech route")
        speech.identity(route["machine_id"])
        speech.validate("speech_submit", {"request_id": request_id,
                        "profile_id": route["profile_id"], "audio": payload["audio"],
                        **({"profile_revision": route["profile_revision"]} if "profile_revision" in route else {})}, request_id)
    elif "message_id" in extra:
        speech.identity(payload["message_id"])


def result_valid(operation, result, payload):
    if not isinstance(result, dict) or result.get("schema") != 1:
        return False
    if operation == "voice_profiles":
        return isinstance(result.get("computers"), list) and len(result["computers"]) <= 128
    message = result.get("message")
    expected = payload["request_id"] if operation == "voice_submit" else payload["message_id"]
    return (isinstance(message, dict) and message.get("id") == expected
            and message.get("run_id") == payload["expected_run_id"]
            and message.get("conversation_id") == payload["expected_conversation_id"]
            and message.get("state") in STATES
            and type(message.get("revision")) is int
            and ("transcript" not in message or isinstance(message["transcript"], str)
                 and len(message["transcript"].encode()) <= 65536))
