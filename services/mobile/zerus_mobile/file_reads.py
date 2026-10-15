"""Explicit, bounded snapshots of documents on the selected conversation computer."""
from __future__ import annotations

import base64
import hashlib
import re
import uuid
from . import attachment_reads
from .attachments import MAX_FILE_BYTES, MAX_READ_BYTES

OPERATION = "file_read"
HELP_MARKER = "file-read <session> --json"


def validate(payload: object, request_id: str) -> None:
    required = {"request_id", "expected_run_id", "expected_conversation_id", "path", "offset", "length"}
    optional = {"sha256", "archive_id", "agent_id"}
    if not isinstance(payload, dict) or set(payload) - required - optional or not required <= set(payload) or payload["request_id"] != request_id:
        raise ValueError("invalid file read fields")
    for key, maximum in (("expected_run_id", 128), ("expected_conversation_id", 256), ("path", 8192), ("agent_id", 160)):
        if key not in payload:
            continue
        text = payload[key]
        if not isinstance(text, str) or not text or len(text.encode()) > maximum or any(ord(c) < 32 or 127 <= ord(c) <= 159 for c in text):
            raise ValueError("invalid file read identity or path")
    if "agent_id" in payload and any(c in payload["agent_id"] for c in "/\\"):
        raise ValueError("invalid child identity")
    if "archive_id" in payload:
        try:
            archive = uuid.UUID(payload["archive_id"]) if isinstance(payload["archive_id"], str) else None
        except ValueError:
            archive = None
        if archive is None or archive.int == 0 or str(archive) != payload["archive_id"]:
            raise ValueError("invalid archive identity")
    if "sha256" in payload and (not isinstance(payload["sha256"], str) or not re.fullmatch(r"[a-f0-9]{64}", payload["sha256"])):
        raise ValueError("invalid file digest")
    offset, length = payload["offset"], payload["length"]
    if type(offset) is not int or type(length) is not int or offset < 0 or not 1 <= length <= MAX_READ_BYTES or offset + length > MAX_FILE_BYTES:
        raise ValueError("invalid file range")
    if offset and "sha256" not in payload:
        raise ValueError("later file ranges require the original digest")


def native_payload(payload: dict) -> dict:
    return {k: v for k, v in payload.items() if k not in {"offset", "length"}}


def key(session: str, payload: dict, sha: str) -> tuple:
    return ("file", session, payload["expected_run_id"], payload["expected_conversation_id"], payload.get("archive_id", ""),
            payload.get("agent_id", ""), payload["path"], sha)


def decode(raw: object, session: str, payload: dict) -> dict:
    if not isinstance(raw, dict) or any(raw.get(k, "") != v for k, v in (
        ("request_id", payload["request_id"]), ("name", session), ("run_id", payload["expected_run_id"]),
        ("conversation_id", payload["expected_conversation_id"]), ("archive_id", payload.get("archive_id", "")),
        ("agent_id", payload.get("agent_id", "")), ("requested_path", payload["path"]))):
        raise attachment_reads.AttachmentReadError("native file identity did not match")
    path, name, mime, encoded, size, sha = (raw.get(k) for k in ("path", "file_name", "mime", "data_base64", "bytes", "sha256"))
    if (not isinstance(path, str) or not path.startswith("/") or path.startswith("//") or len(path.encode()) > 8192
            or any(ord(c) < 32 or 127 <= ord(c) <= 159 for c in path)
            or not isinstance(name, str) or not name or len(name.encode()) > 255 or name != path.rsplit("/", 1)[-1]
            or "\\" in name or any(ord(c) < 32 or 127 <= ord(c) <= 159 for c in name)
            or not isinstance(mime, str) or not re.fullmatch(r"[a-z0-9!#$&^_.+\-]+/[a-z0-9!#$&^_.+\-]+", mime) or len(mime) > 100
            or type(size) is not int or not 1 <= size <= MAX_FILE_BYTES
            or not isinstance(encoded, str) or len(encoded) > ((MAX_FILE_BYTES + 2) // 3) * 4
            or not isinstance(sha, str) or not re.fullmatch(r"[a-f0-9]{64}", sha)):
        raise attachment_reads.AttachmentReadError("native file details are invalid")
    try:
        data = base64.b64decode(encoded, validate=True)
    except ValueError:
        raise attachment_reads.AttachmentReadError("native file data is invalid") from None
    if len(data) != size or hashlib.sha256(data).hexdigest() != sha or payload.get("sha256", sha) != sha:
        raise attachment_reads.AttachmentReadError("file changed during download; open it again")
    return dict(path=path, name=name, mime=mime, bytes=size, sha256=sha, data=data)


def chunk(entry: dict, session: str, payload: dict, request_id: str) -> dict:
    offset, length = payload["offset"], payload["length"]
    if offset >= entry["bytes"] or payload.get("sha256", entry["sha256"]) != entry["sha256"]:
        raise attachment_reads.AttachmentReadError("file changed during download; open it again")
    data = entry["data"][offset:offset + length]
    result = dict(request_id=request_id, name=session, run_id=payload["expected_run_id"],
                  conversation_id=payload["expected_conversation_id"], requested_path=payload["path"], path=entry["path"],
                  file_name=entry["name"], mime=entry["mime"], bytes=entry["bytes"], sha256=entry["sha256"],
                  offset=offset, length=len(data), data_base64=base64.b64encode(data).decode("ascii"))
    for field in ("archive_id", "agent_id"):
        if field in payload:
            result[field] = payload[field]
    return result
