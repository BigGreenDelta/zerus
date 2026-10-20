"""Fixed context-control transport; native hgs remains the final authority."""
import uuid
import re
from .analytics import OPERATIONS as ANALYTICS_READS
from .speech import OPERATIONS as SPEECH_OPERATIONS, READS as SPEECH_READS
from .speech_config import OPERATIONS as SPEECH_CONFIG_OPERATIONS, READS as SPEECH_CONFIG_READS
from .voice import OPERATIONS as VOICE_OPERATIONS, READS as VOICE_READS

CONTEXT_COMMANDS = {"compact_context": "compact-context", "clear_context": "clear-context"}
CLEAR_CONTEXT_DISMISS_FEATURE = "clear_context_dismiss"
CLEAR_CONTEXT_DISMISS_HELP = "Mobile clear-context ABI: dismiss-prompt-v1"
LIFECYCLE_OPERATIONS = frozenset({"pause", "resume", "archive", "rename", "fork", "terminate", "restore", "forget"})
TERMINAL_OPERATIONS = frozenset({"terminal_snapshot", "terminal_input"})
LAUNCH_OPERATIONS = frozenset({"catalog", "dirs", "directory_create", "launch", "worktrees", "worktree_create"})
COMPUTER_OPERATIONS = frozenset({"set_mobile_delivery"})
MOBILE_DELIVERY = ("immediate", "away", "away_5", "away_10", "away_15", "away_30")
ATTACHMENT_OPERATIONS = frozenset({"attachment_read"})
FILE_OPERATIONS = frozenset({"file_read"})
OPERATIONS = ANALYTICS_READS | SPEECH_CONFIG_OPERATIONS | VOICE_OPERATIONS | SPEECH_OPERATIONS | LAUNCH_OPERATIONS | LIFECYCLE_OPERATIONS | TERMINAL_OPERATIONS | COMPUTER_OPERATIONS | ATTACHMENT_OPERATIONS | FILE_OPERATIONS | frozenset({"archive_list","hooks_status","inspect", "send", "answer", "interrupt", "send_now", "settings", "process_output", "process_stop", "history", "recovery_action", "mark_read", "session_notifications", *CONTEXT_COMMANDS})
FEATURES = frozenset({"inspect_after", "inspect_agent", "send_agent"})
READ_OPERATIONS = ANALYTICS_READS | SPEECH_CONFIG_READS | VOICE_READS | SPEECH_READS | frozenset({"archive_list", "hooks_status", "inspect", "process_output", "terminal_snapshot", "catalog", "dirs", "worktrees", "history", "mark_read", "attachment_read", "file_read"})
# Predicate of the persisted read_history_v3 index and the one-hour read class.
# attachment_read is a read with its own short class, so this text stays fixed.
READ_SQL = "('inspect','process_output','terminal_snapshot','catalog','dirs','worktrees','history','mark_read')"
# Every read operation, for counters and tombstone exclusion (never an index predicate).
READ_CLASS_SQL = "(" + ",".join(f"'{operation}'" for operation in sorted(READ_OPERATIONS)) + ")"
# Attachment bytes are carried, never kept: removed this long after completion,
# or this long after the phone has read the terminal receipt.
ATTACHMENT_RESULT_TTL = 60
ATTACHMENT_READ_GRACE = 5
CAPABILITY_OPERATIONS = OPERATIONS - {"inspect", "send", "answer", "interrupt"}
IDENTITY_FIELDS = frozenset({"request_id", "expected_run_id", "expected_conversation_id"})


def validate_mobile_delivery(payload):
    """The shared delivery choice targets the gateway computer and carries only its value."""
    if not isinstance(payload, dict) or set(payload) != {"value"} or payload["value"] not in MOBILE_DELIVERY:
        raise ValueError("invalid phone notification timing")


def validate_context(payload, request_id):
    if not isinstance(payload, dict) or set(payload) != IDENTITY_FIELDS or payload.get("request_id") != request_id:
        raise ValueError("context control requires exact request/run/conversation identity")
    for field, maximum in (("expected_run_id", 128), ("expected_conversation_id", 256)):
        value = payload[field]
        if not isinstance(value, str) or not value or len(value) > maximum or any(ord(c) < 32 or ord(c) == 127 for c in value):
            raise ValueError("context control requires nonempty bounded native identity")


def validate_clear_context(payload, request_id):
    """Keep ordinary context validation unchanged; only clear accepts this opt-in."""
    ordinary = payload
    if isinstance(payload, dict) and "dismiss_prompt" in payload:
        if type(payload["dismiss_prompt"]) is not bool:
            raise ValueError("dismiss_prompt must be a boolean")
        ordinary = {key: value for key, value in payload.items() if key != "dismiss_prompt"}
    validate_context(ordinary, request_id)


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
            model = bounded(payload["model"], 256)
            if len(model.encode("utf-8")) > 256:
                raise ValueError("native model identifier exceeds UTF-8 byte limit")
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
