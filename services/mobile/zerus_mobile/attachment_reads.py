"""Read-only phone access to files of the exact conversation's own receipts.

The phone names a delivery receipt (``message_id``) and file position
(``index``); it never supplies a path. Native ``hgs attachment`` resolves the
receipt for the exact conversation, refuses symlinks and paths outside its
private attachment store, and bounds the file. This module verifies that
answer and serves one bounded byte range at a time, because one relay result
is limited to 1 MiB. Decoded files stay briefly in connector memory so the
chunks of one file do not each re-read it; nothing is written to disk here.
"""
from __future__ import annotations

import base64
import binascii
import hashlib
import time
from collections import OrderedDict

from .attachments import MAX_FILE_BYTES, MAX_READ_BYTES

OPERATION = "attachment_read"
# The native answer carries the whole file as base64 plus small metadata.
NATIVE_OUTPUT_BYTES = ((MAX_FILE_BYTES + 2) // 3) * 4 + 64 * 1024
HELP_MARKER = "attachment <session> --request ID --index N"


class AttachmentReadError(Exception):
    pass


def native_argv(session: str, payload: dict) -> list[str]:
    argv = ["attachment", session, "--request", payload["message_id"], "--index", str(payload["index"]),
            "--conversation", payload["expected_conversation_id"]]
    if "archive_id" in payload:
        argv.extend(["--archive", payload["archive_id"]])
    if "agent_id" in payload:
        argv.extend(["--agent", payload["agent_id"]])
    return argv


def check_identity(detail: object, session: str, payload: dict) -> None:
    """Exact run/conversation (and archive) identity, as neighbouring reads check it."""
    archived = "archive_id" in payload
    if (not isinstance(detail, dict) or detail.get("name") != session
            or detail.get("run_id") != payload["expected_run_id"]
            or detail.get("conversation_id") != payload["expected_conversation_id"]
            or detail.get("archive_id", "") != payload.get("archive_id", "")
            or (not archived and (detail.get("state") == "archived" or detail.get("archive_id")))):
        raise AttachmentReadError("native attachment conversation identity did not match")
    if "agent_id" in payload and (not isinstance(detail.get("subagents"), dict)
                                  or payload["agent_id"] not in detail["subagents"]):
        raise AttachmentReadError("child does not belong to the exact selected parent")


def decode(raw: object, payload: dict) -> dict:
    """Verify the native answer names the requested receipt file; return its exact bytes."""
    if (not isinstance(raw, dict) or raw.get("request_id") != payload["message_id"]
            or type(raw.get("index")) is not int or raw["index"] != payload["index"]):
        raise AttachmentReadError("native attachment identity did not match")
    name, mime, data = raw.get("name"), raw.get("mime"), raw.get("data_base64")
    if not isinstance(name, str) or not name or len(name) > 1024:
        raise AttachmentReadError("native attachment name is invalid")
    if not isinstance(mime, str) or len(mime) > 255:
        raise AttachmentReadError("native attachment type is invalid")
    if not isinstance(data, str) or not data or len(data) > ((MAX_FILE_BYTES + 2) // 3) * 4:
        raise AttachmentReadError("attachment exceeds 10 MiB")
    try:
        decoded = base64.b64decode(data, validate=True)
    except (binascii.Error, ValueError):
        raise AttachmentReadError("native attachment data is invalid") from None
    if not decoded or len(decoded) > MAX_FILE_BYTES:
        raise AttachmentReadError("attachment must be nonempty and at most 10 MiB")
    declared = raw.get("bytes")
    if type(declared) is int and declared != len(decoded):
        raise AttachmentReadError("attachment size changed since it was sent")
    return {"name": name, "mime": mime or "application/octet-stream", "bytes": len(decoded),
            "sha256": hashlib.sha256(decoded).hexdigest(), "data": decoded}


def chunk(entry: dict, session: str, payload: dict, request_id: str) -> dict:
    offset, length = payload["offset"], payload["length"]
    if not 1 <= length <= MAX_READ_BYTES or offset < 0 or offset >= entry["bytes"]:
        raise AttachmentReadError("attachment range is outside the file")
    data = entry["data"][offset:offset + length]
    result = {"request_id": request_id, "name": session, "run_id": payload["expected_run_id"],
              "conversation_id": payload["expected_conversation_id"], "message_id": payload["message_id"],
              "index": payload["index"], "file_name": entry["name"], "mime": entry["mime"],
              "bytes": entry["bytes"], "sha256": entry["sha256"], "offset": offset, "length": len(data),
              "data_base64": base64.b64encode(data).decode("ascii")}
    for field in ("archive_id", "agent_id"):
        if field in payload:
            result[field] = payload[field]
    return result


class AttachmentReads:
    """Bounded in-memory LRU of verified files for one machine context."""

    def __init__(self, max_bytes: int = 3 * MAX_FILE_BYTES, ttl: float = 120, clock=time.monotonic):
        self.max_bytes, self.ttl, self.clock = max_bytes, ttl, clock
        self.entries: OrderedDict[tuple, tuple[float, dict]] = OrderedDict()
        self.size = 0

    @staticmethod
    def key(session: str, payload: dict) -> tuple:
        return (session, payload.get("archive_id", ""), payload.get("agent_id", ""), payload["expected_run_id"],
                payload["expected_conversation_id"], payload["message_id"], payload["index"])

    def _drop(self, key: tuple) -> None:
        _, entry = self.entries.pop(key)
        self.size -= entry["bytes"]

    def get(self, key: tuple) -> dict | None:
        now = self.clock()
        for old in [k for k, (at, _) in self.entries.items() if at <= now - self.ttl]:
            self._drop(old)
        if key not in self.entries:
            return None
        self.entries.move_to_end(key)
        return self.entries[key][1]

    def put(self, key: tuple, entry: dict) -> None:
        if key in self.entries:
            self._drop(key)
        if entry["bytes"] > self.max_bytes:
            return
        self.entries[key] = (self.clock(), entry)
        self.size += entry["bytes"]
        while self.size > self.max_bytes:
            self._drop(next(iter(self.entries)))
