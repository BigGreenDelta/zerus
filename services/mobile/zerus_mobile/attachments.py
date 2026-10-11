"""Validate native inline attachments; a mobile client never supplies a path."""
from __future__ import annotations

import base64
import binascii
import re
import unicodedata
import uuid
from .context import validate_compaction_id, bounded

MAX_COUNT = 8
MAX_FILE_BYTES = 10 * 1024 * 1024
MAX_TOTAL_BYTES = 20 * 1024 * 1024
MAX_TEXT_BYTES = 64 * 1024
MAX_REQUEST_BYTES = 29 * 1024 * 1024
MAX_QUEUE_BYTES = 100 * 1024 * 1024
LIMITS = {"max_count": MAX_COUNT, "max_file_bytes": MAX_FILE_BYTES,
          "max_total_bytes": MAX_TOTAL_BYTES, "max_text_bytes": MAX_TEXT_BYTES,
          "max_request_bytes": MAX_REQUEST_BYTES}
REFERENCE = re.compile(r"\[(?:Image|File) #[1-9][0-9]{0,8}\]")


MAX_READ_BYTES = 512 * 1024  # One attachment_read chunk; base64 stays below the 1 MiB result limit.
READ_FIELDS = frozenset({"request_id", "expected_run_id", "expected_conversation_id", "message_id", "index", "offset", "length"})


def control(value: str) -> bool:
    return any(unicodedata.category(char) == "Cc" for char in value)


def validate_read(payload: dict, request_id: str) -> None:
    """One bounded byte range of a file named by its own receipt and position.

    The phone never supplies a path; native hgs resolves the receipt for the
    exact conversation and reads only its private attachment store.
    """
    if (not isinstance(payload, dict) or not READ_FIELDS <= set(payload)
            or set(payload) - READ_FIELDS - {"archive_id", "agent_id"} or payload.get("request_id") != request_id):
        raise ValueError("invalid attachment read fields")
    bounded(payload["expected_run_id"], 128)
    bounded(payload["expected_conversation_id"], 256)
    message = bounded(payload["message_id"], 36)
    try:
        canonical = str(uuid.UUID(message))
    except ValueError:
        canonical = ""
    if canonical != message:
        raise ValueError("invalid attachment message identity")
    index, offset, length = payload["index"], payload["offset"], payload["length"]
    if (type(index) is not int or not 0 <= index < MAX_COUNT or type(offset) is not int or not 0 <= offset < MAX_FILE_BYTES
            or type(length) is not int or not 1 <= length <= MAX_READ_BYTES or offset + length > MAX_FILE_BYTES):
        raise ValueError("invalid attachment position or range")
    if "agent_id" in payload:
        agent = bounded(payload["agent_id"], 160)
        if any(c in agent for c in "/\\"):
            raise ValueError("invalid child identity")
    if "archive_id" in payload:
        archive = bounded(payload["archive_id"], 36)
        try:
            parsed = uuid.UUID(archive)
        except ValueError:
            parsed = None
        if parsed is None or str(parsed) != archive or parsed.int == 0:
            raise ValueError("invalid archive identity")


def validate_send(payload: dict) -> None:
    """Bound decoded bytes and reject paths, URLs and unexpected metadata."""
    if not isinstance(payload, dict) or set(payload) - {"request_id", "text", "expected_run_id", "expected_conversation_id", "attachments", "expected_compaction_id", "agent_id"}:
        raise ValueError("invalid native send fields")
    validate_compaction_id(payload)
    if "agent_id" in payload:
        agent = bounded(payload["agent_id"], 160)
        bounded(payload.get("expected_conversation_id"), 256)
        if any(c in agent for c in "/\\") or payload.get("expected_compaction_id"):
            raise ValueError("invalid child identity or child compaction continuation")
    text = payload.get("text")
    if not isinstance(text, str) or len(text.encode("utf-8")) > MAX_TEXT_BYTES:
        raise ValueError("message exceeds 64 KiB")
    if any(unicodedata.category(char) == "Cc" and char not in "\n\t" for char in text):
        raise ValueError("message contains control characters")
    if text.lstrip().startswith(("/", "!")):
        raise ValueError("use native Terminal for provider commands")
    files = payload.get("attachments", [])
    if not isinstance(files, list) or len(files) > MAX_COUNT:
        raise ValueError("at most eight attachments are allowed")
    if not text.strip() and not files:
        raise ValueError("message is empty")
    total, references = 0, set()
    for file in files:
        if not isinstance(file, dict) or not {"name", "mime", "data_base64"} <= set(file) or set(file) - {"name", "mime", "data_base64", "reference"}:
            raise ValueError("invalid attachment fields")
        name, mime, data = file["name"], file["mime"], file["data_base64"]
        if (not isinstance(name, str) or not name or name in (".", "..")
                or len(name.encode("utf-8")) > 255 or control(name)
                or "/" in name or "\\" in name):
            raise ValueError("attachment name must be a filename without a path")
        if not isinstance(mime, str) or not mime or len(mime.encode("utf-8")) > 100 or control(mime):
            raise ValueError("invalid attachment MIME type")
        reference = file.get("reference", "")
        if not isinstance(reference, str) or (reference and (not REFERENCE.fullmatch(reference) or reference in references)):
            raise ValueError("invalid or duplicate attachment reference")
        if reference:
            references.add(reference)
        if not isinstance(data, str) or len(data) > ((MAX_FILE_BYTES + 2) // 3) * 4:
            raise ValueError("attachment exceeds 10 MiB")
        # Aligned 64 KiB chunks avoid a decoded file and re-encoded full copy.
        decoded_size = 0
        for offset in range(0, len(data), 65536):
            chunk = data[offset:offset + 65536]
            if offset + len(chunk) < len(data) and "=" in chunk:
                raise ValueError("attachment base64 must be canonical")
            try:
                decoded = base64.b64decode(chunk, validate=True)
            except (binascii.Error, ValueError):
                raise ValueError("invalid attachment base64") from None
            if base64.b64encode(decoded).decode("ascii") != chunk:
                raise ValueError("attachment base64 must be canonical")
            decoded_size += len(decoded)
        if not decoded_size or decoded_size > MAX_FILE_BYTES:
            raise ValueError("attachment must be nonempty and at most 10 MiB")
        total += decoded_size
        if total > MAX_TOTAL_BYTES:
            raise ValueError("attachments exceed 20 MiB in total")
