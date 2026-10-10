"""Fixed context-control transport; native hgs remains the final authority."""
import uuid
import re

CONTEXT_COMMANDS = {"compact_context": "compact-context", "clear_context": "clear-context"}
LIFECYCLE_OPERATIONS = frozenset({"pause", "resume", "archive", "rename", "fork", "terminate", "restore", "forget"})
TERMINAL_OPERATIONS = frozenset({"terminal_snapshot", "terminal_input"})
LAUNCH_OPERATIONS = frozenset({"catalog", "dirs", "launch", "worktrees", "worktree_create"})
OPERATIONS = LAUNCH_OPERATIONS | LIFECYCLE_OPERATIONS | TERMINAL_OPERATIONS | frozenset({"inspect", "send", "answer", "interrupt", "send_now", "settings", "process_output", "process_stop", "history", "recovery_action", "mark_read", *CONTEXT_COMMANDS})
FEATURES = frozenset({"inspect_after", "inspect_agent", "send_agent"})
READ_OPERATIONS = frozenset({"inspect", "process_output", "terminal_snapshot", "catalog", "dirs", "worktrees", "history"})
READ_SQL = "('inspect','process_output','terminal_snapshot','catalog','dirs','worktrees','history')"
CAPABILITY_OPERATIONS = OPERATIONS - {"inspect", "send", "answer", "interrupt"}
IDENTITY_FIELDS = frozenset({"request_id", "expected_run_id", "expected_conversation_id"})


def validate_context(payload, request_id):
    if not isinstance(payload, dict) or set(payload) != IDENTITY_FIELDS or payload.get("request_id") != request_id:
        raise ValueError("context control requires exact request/run/conversation identity")
    for field, maximum in (("expected_run_id", 128), ("expected_conversation_id", 256)):
        value = payload[field]
        if not isinstance(value, str) or not value or len(value) > maximum or any(ord(c) < 32 or ord(c) == 127 for c in value):
            raise ValueError("context control requires nonempty bounded native identity")


def bounded(value, maximum, empty=False):
    if not isinstance(value, str) or len(value) > maximum or (not empty and not value) or any(ord(c) < 32 or ord(c) == 127 for c in value):
        raise ValueError("invalid bounded native identity")
    return value


def token(value, maximum=160):
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9._:-]{1," + str(maximum) + r"}", value):
        raise ValueError("invalid native token")
    return value


def inspect_features(payload):
    return ({"inspect_after"} if "after" in payload else set()) | ({"inspect_agent"} if "agent_id" in payload else set())


def validate_inspect(payload):
    if not isinstance(payload, dict) or set(payload) - {"archive_id", "after", "agent_id", "expected_run_id", "expected_conversation_id"}:
        raise ValueError("invalid inspect fields")
    if inspect_features(payload) or {"expected_run_id", "expected_conversation_id"} & set(payload):
        bounded(payload.get("expected_run_id"), 128)
        bounded(payload.get("expected_conversation_id"), 256)
    if "after" in payload and (type(payload["after"]) is not int or not 0 <= payload["after"] <= 2**63 - 1):
        raise ValueError("invalid native history cursor")
    if "agent_id" in payload:
        bounded(payload["agent_id"], 160)
        if any(char in payload["agent_id"] for char in "/\\") or "after" in payload:
            raise ValueError("invalid or unsupported subagent history cursor")


def validate_core(operation, payload, request_id):
    extras = {"send_now": {"queue_id"}, "settings": {"model", "effort", "expected_pending_id"},
              "process_output": {"process_id", "generation", "archive_id"}, "process_stop": {"process_id", "generation"}}[operation]
    if not isinstance(payload, dict) or not IDENTITY_FIELDS <= set(payload) or set(payload) - IDENTITY_FIELDS - extras or payload.get("request_id") != request_id:
        raise ValueError("invalid scoped native operation fields")
    bounded(payload["expected_run_id"], 128)
    bounded(payload["expected_conversation_id"], 256, empty=operation != "send_now")
    if operation == "send_now":
        if not isinstance(payload.get("queue_id"), str) or not re.fullmatch(r"[0-9a-f]{64}", payload["queue_id"]):
            raise ValueError("invalid native queue identity")
    elif operation == "settings":
        if not {"model", "effort"} & set(payload):
            raise ValueError("select a model or effort")
        if "model" in payload:
            bounded(payload["model"], 120)
        if "effort" in payload and payload["effort"] not in ("", "off", "none", "minimal", "low", "medium", "high", "xhigh", "max", "ultra", "on"):
            raise ValueError("invalid native effort")
        if "expected_pending_id" in payload:
            value = bounded(payload["expected_pending_id"], 36)
            if str(uuid.UUID(value)) != value:
                raise ValueError("invalid pending setting identity")
    else:
        token(payload.get("process_id"))
        if "generation" in payload:
            token(payload["generation"], 256)
        if "archive_id" in payload:
            value = bounded(payload["archive_id"], 36)
            if str(uuid.UUID(value)) != value:
                raise ValueError("invalid archive identity")


def validate_compaction_id(payload):
    value = payload.get("expected_compaction_id", "")
    # The native ABI defaults to an empty string; old ordinary sends retain it.
    if not isinstance(value, str):
        raise ValueError("invalid compaction continuation identity")
    if value and (len(value) != 36 or str(uuid.UUID(value)) != value):
        raise ValueError("invalid compaction continuation identity")


def supports(snapshot, operation, field="operations"):
    caps = snapshot.get("mobile_capabilities") if isinstance(snapshot, dict) else None
    return (isinstance(caps, dict) and type(caps.get("protocol_version")) is int and caps["protocol_version"] == 1
            and isinstance(caps.get(field), list) and operation in caps[field])


def validate_lifecycle(operation, payload, request_id):
    extras = {"new_name"} if operation == "rename" else {"tag"} if operation == "fork" else set()
    if operation in {"restore", "forget", "rename", "fork"}:
        extras.add("archive_id")
    if not isinstance(payload, dict) or not IDENTITY_FIELDS <= set(payload) or set(payload) - IDENTITY_FIELDS - extras or payload.get("request_id") != request_id:
        raise ValueError("invalid scoped lifecycle fields")
    bounded(payload["expected_run_id"], 128)
    bounded(payload["expected_conversation_id"], 256, empty=True)
    if operation == "restore" and "archive_id" not in payload:
        raise ValueError("restore requires an immutable archive identity")
    if "archive_id" in payload:
        value = bounded(payload["archive_id"], 36)
        if str(uuid.UUID(value)) != value or uuid.UUID(value).int == 0:
            raise ValueError("invalid archive identity")
    if operation == "rename":
        value = bounded(payload.get("new_name"), 512)
        if len(value.encode()) > 512 or value.strip() != value:
            raise ValueError("invalid native session name")
    if "tag" in payload:
        value = bounded(payload["tag"], 120)
        if len(value.encode()) > 120 or value.strip() != value or any(c in value for c in "/:."):
            raise ValueError("invalid native fork tag")


MARK_READ_FIELDS = frozenset({"run_id", "conversation_id", "reply_id", "attention_signature"})


def validate_mark_read(payload):
    """C4: exact identity and two nullable marks; native hgs decides what is current."""
    if not isinstance(payload, dict) or set(payload) != MARK_READ_FIELDS:
        raise ValueError("mark_read requires exactly run, conversation, reply and attention fields")
    bounded(payload["run_id"], 128)
    bounded(payload["conversation_id"], 256)
    if payload["reply_id"] is not None:
        bounded(payload["reply_id"], 256)
    signature = payload["attention_signature"]
    if signature is not None and (not isinstance(signature, str) or not re.fullmatch(r"[0-9a-f]{64}", signature)):
        raise ValueError("invalid attention signature")


def mark_read_result(result, payload):
    """Only a native `hgs read` answer for this conversation is a receipt."""
    if result == {"ok": False, "error": "not_found"}:
        return True
    if not isinstance(result, dict) or result.get("ok") is not True:
        return False
    applied, read = result.get("applied"), result.get("read")
    return (isinstance(applied, dict) and set(applied) == {"reply_id", "attention_signature"}
            and all(type(value) is bool for value in applied.values())
            and (read is None or isinstance(read, dict) and read.get("conversation_id") == payload["conversation_id"]))
