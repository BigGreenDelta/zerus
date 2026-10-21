"""A test-only copy of the Python reference relay's submission validation.

`text` and `validate_request` are verbatim copies from `zerus_mobile.server`,
and `_encoded` and `encoded_size` from `zerus_mobile.ingress`, except that
aiohttp's HTTPBadRequest is a ValueError of the same name here. They delegate
only to connector validators, which stay when the reference relay is deleted,
so the frozen relay-oracle column of services/mobile/vectors/validation/*.jsonl
stays reproducible without it. test_vectors.py compares the copies with
server.py and ingress.py while those exist; the Rust relay's own column comes
from services/mobile/relay/tests/vectors.rs.
"""
from __future__ import annotations

import json
import uuid

from zerus_mobile import analytics, archives, hook_health, file_reads, speech, speech_config, voice, session_notifications, session_projects
from zerus_mobile.attachments import validate_read, validate_send
from zerus_mobile.context import validate_clear_context
from zerus_mobile.context import (COMPUTER_OPERATIONS, CONTEXT_COMMANDS, LIFECYCLE_OPERATIONS, OPERATIONS,
                                  validate_context, validate_core, validate_inspect, validate_lifecycle,
                                  validate_mark_read, validate_mobile_delivery)
from zerus_mobile.history import validate_history, validate_history_projection
from zerus_mobile.launch import OPERATIONS as LAUNCH_OPERATIONS, validate as validate_launch
from zerus_mobile.recovery import validate_recovery
from zerus_mobile.terminal import OPERATIONS as TERMINAL_OPERATIONS, validate as validate_terminal

FIELDS = ("request_id", "computer_id", "operation", "session", "payload")


class HTTPBadRequest(ValueError):
    """Stands for aiohttp's exception, a 400 without a message, which the
    vectors record by this name."""
    status = 400


def decision(submission) -> dict:
    """The reference relay's decision on one submission, as the vectors record
    it: the body fields, validate_request, then the attachment or size limit."""
    if not isinstance(submission, dict) or set(submission) - set(FIELDS) or not set(FIELDS) <= set(submission):
        return {"decision": "reject", "status": 400, "raised": "HTTPBadRequest", "stage": "fields"}
    try:
        validate_request(submission)
        operation = submission["operation"]
        if operation == "send":
            validate_send(submission["payload"])
        elif operation == "voice_submit" and "video" in submission["payload"]:
            if encoded_size(submission, voice.video.MAX_REQUEST_BYTES) > voice.video.MAX_REQUEST_BYTES:
                return {"decision": "reject", "status": 413, "raised": "HTTPRequestEntityTooLarge", "stage": "size"}
        elif operation in {"speech_submit", "voice_submit"}:
            if encoded_size(submission, 6 * 1024 * 1024) > 6 * 1024 * 1024:
                return {"decision": "reject", "status": 413, "raised": "HTTPRequestEntityTooLarge", "stage": "size"}
        elif encoded_size(submission, 1024 * 1024) > 1024 * 1024:
            return {"decision": "reject", "status": 413, "raised": "HTTPRequestEntityTooLarge", "stage": "size"}
    except HTTPBadRequest as error:
        return {"decision": "reject", "status": error.status, "raised": type(error).__name__, "stage": "validate"}
    except ValueError as error:
        return {"decision": "reject", "status": 400, "raised": "ValueError", "message": str(error), "stage": "validate"}
    except (TypeError, KeyError, RecursionError) as error:  # Interpreter wording differs between Python versions.
        return {"decision": "reject", "status": 400, "raised": type(error).__name__, "stage": "validate"}
    return {"decision": "accept"}


# Verbatim from zerus_mobile/server.py.

def text(value, maximum=1024, empty=False):
    if not isinstance(value, str) or len(value) > maximum or (not empty and not value) or any(ord(c) < 32 for c in value):
        raise HTTPBadRequest()
    return value


def validate_request(value):
    request_id = text(value["request_id"], 36)
    if str(uuid.UUID(request_id)) != request_id:
        raise HTTPBadRequest()
    text(value["computer_id"], 36)
    text(value["session"], 1024, empty=value["operation"] in analytics.OPERATIONS or value["operation"] == "archive_list" or value["operation"] in LAUNCH_OPERATIONS or value["operation"] in COMPUTER_OPERATIONS or value["operation"] in speech.OPERATIONS or value["operation"] in speech_config.OPERATIONS)
    op, payload = value["operation"], value["payload"]
    if not isinstance(op, str) or op not in OPERATIONS or not isinstance(payload, dict):
        raise HTTPBadRequest()
    if op in analytics.OPERATIONS:
        if value["session"] != "": raise ValueError("analytics reads use no session target")
        analytics.validate(payload)
        return
    if op == "archive_list":
        if value["session"] != "": raise ValueError("archive browsing uses no session target")
        archives.validate(payload)
        return
    if op in LAUNCH_OPERATIONS:
        if value["session"] != "": raise ValueError("catalog/launch use no existing session target")
        validate_launch(op,payload,request_id)
        return
    if op in speech_config.OPERATIONS:
        if value["session"] != "": raise ValueError("speech configuration uses no session target")
        speech_config.validate(op, payload, request_id)
        return
    if op in speech.OPERATIONS:
        if value["session"] != "": raise ValueError("speech uses no session target")
        speech.validate(op, payload, request_id)
        return
    if op in voice.OPERATIONS:
        voice.validate(op, payload, request_id)
        return
    if op in COMPUTER_OPERATIONS:
        if value["session"] != "": raise ValueError("the delivery setting uses no session target")
        validate_mobile_delivery(payload)
        return
    if op == "inspect":
        validate_inspect(payload)
        if "archive_id" in payload:
            archive_id = text(payload["archive_id"], 36)
            if str(uuid.UUID(archive_id)) != archive_id:
                raise HTTPBadRequest()
        return
    if op == "session_notifications":
        session_notifications.validate(payload)
        return
    if op == "mark_read":
        validate_mark_read(payload)
        return
    if op == "recovery_action":
        validate_recovery(payload,request_id)
        return
    if op in session_projects.OPERATIONS:
        session_projects.validate(op,payload,request_id)
        return
    if op == "hooks_status":
        hook_health.validate(payload,request_id)
        return
    if op == "history":
        validate_history(payload,request_id)
        return
    if op == "history_projection":
        validate_history_projection(payload, request_id)
        return
    if op == "file_read":
        file_reads.validate(payload, request_id)
        return
    if op == "attachment_read":
        validate_read(payload,request_id)
        return
    if op in TERMINAL_OPERATIONS:
        validate_terminal(op,payload,request_id)
        return
    if op in LIFECYCLE_OPERATIONS:
        validate_lifecycle(op, payload, request_id)
        return
    if op in CONTEXT_COMMANDS:
        (validate_clear_context if op == "clear_context" else validate_context)(payload, request_id)
        return
    if op in {"send_now", "settings", "process_output", "process_stop"}:
        validate_core(op, payload, request_id)
        return
    if payload.get("request_id") != request_id:
        raise HTTPBadRequest()
    text(payload.get("expected_run_id"), 128)
    text(payload.get("expected_conversation_id"), 256, empty=True)
    base = {"request_id", "expected_run_id", "expected_conversation_id"}
    extras = {"send": {"text", "attachments", "expected_compaction_id", "agent_id"}, "answer": {"question_id", "expected_question_hash", "answers"}, "interrupt": {"expected_turn_started"}}[op]
    if set(payload) - base - extras:
        raise HTTPBadRequest()
    required = {"send": {"text"}, "answer": {"question_id", "expected_question_hash", "answers"}, "interrupt": {"expected_turn_started"}}[op]
    if not required <= set(payload):
        raise HTTPBadRequest()


# Verbatim from zerus_mobile/ingress.py.

def _encoded(value):
    """An encoder whose largest temporary string is bounded even for base64."""
    if isinstance(value, str):
        yield b'"'
        for start in range(0, len(value), 8192):
            yield json.dumps(value[start:start + 8192], ensure_ascii=True)[1:-1].encode("ascii")
        yield b'"'
    elif isinstance(value, dict):
        yield b"{"
        for index, (key, item) in enumerate(value.items()):
            if index: yield b","
            yield from _encoded(key)
            yield b":"
            yield from _encoded(item)
        yield b"}"
    elif isinstance(value, (list, tuple)):
        yield b"["
        for index, item in enumerate(value):
            if index: yield b","
            yield from _encoded(item)
        yield b"]"
    else:
        yield json.dumps(value, allow_nan=False).encode("ascii")


def encoded_size(value, maximum):
    """Stop counting at the limit without creating a whole serialized copy."""
    total = 0
    for chunk in _encoded(value):
        total += len(chunk)
        if total > maximum:
            return total
    return total
