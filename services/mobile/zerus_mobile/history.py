"""Bound public inspection history without cropping control disclosures."""
import json
import math

STREAMS = ("events", "message_events", "provider_messages", "attachment_messages")
USER_TYPES = {"UserPromptSubmit", "UserPromptQueued", "UserMessage", "TurnStarted", "QuestionAnswered"}


def encoded(value):
    return json.dumps(value, allow_nan=False).encode()


def completed_size(result):
    return len(encoded({"state": "completed", "result": result, "error": None}))


def bound_inspection(result, maximum):
    """Drop whole oldest history rows, preserving the newest own message first.

    All other fields retain their exact JSON values. ASCII escaping is counted
    as it is on the actual node-result HTTP upload. Run this CPU work off-loop.
    """
    if completed_size(result) <= maximum:
        return result
    value = dict(result)
    rows, latest = [], None
    for stream in STREAMS:
        if not isinstance(value.get(stream), list):
            continue
        for index, row in enumerate(value[stream]):
            at = row.get("at", 0) if isinstance(row, dict) else 0
            at = at if isinstance(at, (int, float)) and not isinstance(at, bool) and math.isfinite(at) else 0
            item = (at, stream, index, len(encoded(row)))
            rows.append(item)
            if (isinstance(row, dict) and row.get("type") in USER_TYPES
                    and row.get("agent_id", "") in ("", "main")
                    and (latest is None or (at, stream == "message_events") > (latest[0], latest[1] == "message_events"))):
                latest = item
    omitted = {stream: 0 for stream in STREAMS}
    value["mobile_history_truncated"] = True
    value["mobile_history_omitted"] = omitted
    size = completed_size(value)
    removed = {stream: set() for stream in STREAMS}
    remaining = {stream: len(value[stream]) for stream in STREAMS if isinstance(value.get(stream), list)}
    # Preserve one exact newest user row preferentially, then its loss is also
    # disclosed if mandatory metadata leaves no room even for that whole row.
    for item in sorted(rows, key=lambda item: (item == latest, item[0], item[1], item[2])):
        if size <= maximum:
            break
        _, stream, index, length = item
        before = len(str(omitted[stream]))
        omitted[stream] += 1
        size += len(str(omitted[stream])) - before
        size -= length + (2 if remaining[stream] > 1 else 0)
        remaining[stream] -= 1
        removed[stream].add(index)
    for stream, indices in removed.items():
        if indices:
            value[stream] = [row for index, row in enumerate(value[stream]) if index not in indices]
    if completed_size(value) > maximum:
        raise ValueError("native inspection metadata exceeds relay size limit; required disclosures were not cropped")
    return value


"""Exact-target read-only public history paging; native cursors remain opaque."""
import uuid
from .context import IDENTITY_FIELDS, bounded


def validate_history(payload, request_id):
    extras = {'archive_id','agent_id','limit','before','after','around','around_incoming_seq','history_epoch'}
    if not isinstance(payload,dict) or not IDENTITY_FIELDS <= set(payload) or set(payload)-IDENTITY_FIELDS-extras or payload.get('request_id') != request_id:
        raise ValueError('invalid exact history fields')
    bounded(payload['expected_run_id'],128)
    bounded(payload['expected_conversation_id'],256)
    if 'limit' in payload and (type(payload['limit']) is not int or not 1 <= payload['limit'] <= 100):
        raise ValueError('invalid history page limit')
    selectors = {'before','after','around','around_incoming_seq'} & set(payload)
    if len(selectors)>1: raise ValueError('choose one history selector')
    for field in ('before','after','around'):
        if field in payload: bounded(payload[field],2048)
    if ('around_incoming_seq' in payload) != ('history_epoch' in payload):
        raise ValueError('incoming anchor requires exact history epoch')
    if 'around_incoming_seq' in payload:
        if type(payload['around_incoming_seq']) is not int or not 0 <= payload['around_incoming_seq'] <= 2**63-1:
            raise ValueError('invalid incoming sequence')
        bounded(payload['history_epoch'],128)
    if 'agent_id' in payload:
        bounded(payload['agent_id'],160)
        if any(c in payload['agent_id'] for c in '/\\'): raise ValueError('invalid child identity')
    if 'archive_id' in payload:
        value=bounded(payload['archive_id'],36)
        if str(uuid.UUID(value)) != value or uuid.UUID(value).int == 0: raise ValueError('invalid archive identity')


PROJECTION_BYTES = 880 * 1024
PROJECTION_REQUEST_ERROR = "invalid history projection request"
PROJECTION_RESULT_ERROR = "native history projection identity or envelope did not match"
_PUBLIC_TYPES = {"UserPromptSubmit", "UserPromptQueued", "UserMessage", "TurnStarted", "QuestionAnswered", "AgentMessage", "Stop"}


def _projection_text(value, maximum):
    if not isinstance(value, str) or not value or any(ord(c) < 32 or 127 <= ord(c) <= 159 for c in value):
        return False
    try:
        return len(value.encode("utf-8")) <= maximum
    except UnicodeError:
        return False


def _projection_uuid(value):
    if not isinstance(value, str):
        return False
    try:
        parsed = uuid.UUID(value)
        return parsed.int != 0 and str(parsed) == value
    except ValueError:
        return False


def _projection_integer(value):
    return type(value) is int and 0 <= value <= 2**63 - 1


def validate_history_projection(payload, request_id):
    required = IDENTITY_FIELDS | {"history_epoch", "projection_after", "projection_until"}
    if (not isinstance(payload, dict) or not required <= set(payload)
            or set(payload) - required - {"archive_id", "agent_id"}
            or payload.get("request_id") != request_id or not _projection_uuid(request_id)
            or not _projection_text(payload["expected_run_id"], 128)
            or not _projection_text(payload["expected_conversation_id"], 256)
            or not _projection_text(payload["history_epoch"], 128)
            or not _projection_integer(payload["projection_after"])
            or not _projection_integer(payload["projection_until"])
            or payload["projection_after"] > payload["projection_until"]):
        raise ValueError(PROJECTION_REQUEST_ERROR)
    if "archive_id" in payload and not _projection_uuid(payload["archive_id"]):
        raise ValueError(PROJECTION_REQUEST_ERROR)
    if "agent_id" in payload and (not _projection_text(payload["agent_id"], 160)
                                  or any(c in payload["agent_id"] for c in "/\\")):
        raise ValueError(PROJECTION_REQUEST_ERROR)


def _projection_id(value):
    return isinstance(value, str) and len(value) == 64 and all(c in "0123456789abcdef" for c in value)


def _projection_row(row, payload):
    if (not isinstance(row, dict) or not _projection_id(row.get("history_id"))
            or not _projection_text(row.get("history_cursor"), 8192)
            or row.get("history_stream") not in ("journal", "provider", "attachment", "voice")
            or not isinstance(row.get("type"), str) or row["type"] not in _PUBLIC_TYPES
            or "native_match_hash" in row
            or not isinstance(row.get("original_ids"), list) or len(row["original_ids"]) > 8
            or not all(isinstance(v, str) and v for v in row["original_ids"])):
        return False
    agent = row.get("agent_id")
    if agent is not None and (not isinstance(agent, str) or agent not in ("", payload.get("agent_id", "main"))):
        return False
    sequence = row.get("incoming_seq")
    if sequence is not None and not _projection_integer(sequence):
        return False
    attachments = row.get("attachments", [])
    if attachments is None:
        attachments = []
    if not isinstance(attachments, list) or not all(isinstance(v, dict) for v in attachments):
        return False
    detail = row.get("detail")
    return (isinstance(detail, str) and bool(detail.strip())) or bool(attachments)


def validate_projection_result(result, payload, session, request_id):
    """Validate one intact read result. Completion never acknowledges a display prefix."""
    def refuse():
        raise ValueError(PROJECTION_RESULT_ERROR)
    if not isinstance(result, dict):
        refuse()
    child = payload.get("agent_id")
    expected_conversation = payload["expected_conversation_id"]
    if (result.get("request_id") != request_id or result.get("name") != session
            or result.get("run_id") != payload["expected_run_id"]
            or result.get("archive_id", "") != payload.get("archive_id", "")
            or result.get("agent_id", "") != payload.get("agent_id", "")
            or result.get("conversation_id") != expected_conversation + ("/" + child if child else "")
            or result.get("parent_conversation_id", "") != (expected_conversation if child else "")
            or result.get("history_epoch") != payload["history_epoch"]
            or not _projection_integer(result.get("projection_generation"))
            or result["projection_generation"] < payload["projection_until"]
            or type(result.get("complete")) is not bool
            or not _projection_integer(result.get("next_projection_after"))
            or not isinstance(result.get("projection_changes"), list)
            or len(result["projection_changes"]) > 256):
        refuse()
    try:
        # Compact ASCII is the native envelope budget; default receipt separators
        # have their separate existing 1 MiB guard in the connector.
        wire = json.dumps(result, ensure_ascii=True, separators=(",", ":"), allow_nan=False)
        json.dumps(result, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode("utf-8")
        if len(wire.encode("ascii")) > PROJECTION_BYTES:
            refuse()
    except (TypeError, ValueError, UnicodeError, RecursionError):
        refuse()
    previous = payload["projection_after"]
    for entry in result["projection_changes"]:
        required = {"generation", "history_id", "change", "resolution", "successor"}
        if (not isinstance(entry, dict) or not required <= set(entry)
                or set(entry) - required - {"reason"} or not _projection_integer(entry.get("generation"))
                or not previous < entry["generation"] <= payload["projection_until"]
                or not _projection_id(entry.get("history_id")) or entry.get("change") not in ("retired", "refreshed")):
            refuse()
        previous = entry["generation"]
        if entry["resolution"] == "unresolved":
            if entry["successor"] is not None or ("reason" in entry and entry["reason"] not in ("no_successor", "unexpected_successor", "oversize")):
                refuse()
        elif entry["resolution"] == "resolved" and "reason" not in entry:
            if entry["change"] == "retired":
                if entry["successor"] is not None:
                    refuse()
            elif not _projection_row(entry["successor"], payload):
                refuse()
        else:
            refuse()
    if result["complete"]:
        if result["next_projection_after"] != payload["projection_until"]:
            refuse()
    elif not result["projection_changes"] or result["next_projection_after"] != previous or previous >= payload["projection_until"]:
        refuse()
