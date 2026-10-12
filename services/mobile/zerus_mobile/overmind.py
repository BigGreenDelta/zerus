"""Gateway-scoped Overmind requests: the relay envelope, the connector's native
commands and the small `mobile_overmind` digest that decision alerts follow.

`hgs overmind` on the gateway validates every request field and gate. The relay
checks only the envelope, so new request fields need no relay deployment.
"""
from __future__ import annotations

import base64
import json
import os
import re
import stat
import unicodedata

from .attachments import MAX_FILE_BYTES, MAX_READ_BYTES, MAX_REQUEST_BYTES
from .fleet import canonical_uuid

READ_ACTIONS = ("projects", "board", "task", "decisions", "attachment")
WRITE_ACTIONS = ("create", "edit", "move", "reorder", "pin", "unpin", "label", "link", "comment", "send",
                 "review-send", "rework-send", "answer", "resolve-delivery", "done", "reopen", "delete")
# The phone's `task` read is the CLI's `show`; every other action is its own command.
COMMANDS = {**{action: action for action in READ_ACTIONS + WRITE_ACTIONS}, "task": "show"}
HELP_MARKER = "overmind <command> --json (mobile ABI overmind-v1)"
READ_MAX = 64 * 1024
DEVICE_MAX = 64
DIGEST_MAX = 64 * 1024
DIGEST_SECONDS = 15
DECISIONS_MAX = 50
DECISION_SLOT = "overmind:"
# Escaped (canonical ASCII JSON) bytes a digest decision may spend per field, so all 50
# always fit in 64 KiB; hgs project ids have at most 128 UTF-8 bytes, 384 escaped.
ENTRY_BOUNDS = {"task": 64, "project": 384, "signature": 128}
TITLE_CHARS, TITLE_BYTES = 120, 600
# Reads the connector pages into one relay result: the window field and its first size.
PAGES = {"task": ("journal_limit", 100), "board": ("limit", 200)}
PAGE_FIELDS = frozenset({"journal_limit", "journal_before", "limit", "after", "stage"})
# An applied write's task as a compact card: the board fields of hgs `view(full=false)`
# plus the transition head and a renamed pin, so its answer always fits one result.
CARD_KEYS = frozenset({"uuid", "ref", "project", "title", "stage", "priority", "rank", "labels", "pinned", "assignee",
                       "reviewer", "flags", "parent", "follows", "created_at", "updated_at", "read_only", "deleted",
                       "journal_count", "head", "pinned_current"})


def valid_device(value) -> bool:
    """The `hgs overmind --device` rule: 1-64 characters, no C0, DEL or C1 control, no leading - or @."""
    return (isinstance(value, str) and 1 <= len(value) <= DEVICE_MAX and not value.startswith(("-", "@"))
            and not any(ord(c) < 0x20 or 0x7f <= ord(c) <= 0x9f for c in value))


def validate(operation: str, payload, request_id: str, check_device: bool = True) -> int:
    """The exact envelope of an `overmind_read` or `overmind_write`; returns its canonical size limit.
    The connector passes `check_device=False`: it normalizes the informational label instead."""
    read = operation == "overmind_read"
    required = {"action", "request"} if read else {"request_id", "action", "request"}
    if not isinstance(payload, dict) or not required <= set(payload) or set(payload) - required - {"device"}:
        raise ValueError("invalid Overmind request fields")
    # A write's identity is the Overmind request_id (and the created task's UUID).
    if not read and payload["request_id"] != request_id:
        raise ValueError("the Overmind request_id is the request identity")
    if not isinstance(payload["action"], str) or payload["action"] not in (READ_ACTIONS if read else WRITE_ACTIONS):
        raise ValueError("unsupported Overmind action")
    if not isinstance(payload["request"], dict):
        raise ValueError("an Overmind request is an object")
    if check_device and "device" in payload and not valid_device(payload["device"]):
        raise ValueError("invalid device label")
    if read and payload["action"] == "attachment":
        offset, length = payload["request"].get("offset"), payload["request"].get("length")
        if (type(offset) is not int or type(length) is not int or not 0 <= offset < MAX_FILE_BYTES
                or not 1 <= length <= MAX_READ_BYTES or offset + length > MAX_FILE_BYTES):
            raise ValueError("invalid attachment range")
    return READ_MAX if read else MAX_REQUEST_BYTES


def device_label(value) -> str | None:
    """The phone's label as `--device` takes it, or None to omit the flag: controls
    dropped, leading space, `-` and `@` stripped, at most 64 characters."""
    if not isinstance(value, str):
        return None
    label = "".join(c for c in value if unicodedata.category(c) not in ("Cc", "Cs"))
    return re.sub(r"^[\s@-]+", "", label)[:DEVICE_MAX].rstrip() or None


def escaped(text: str) -> int:
    return len(json.dumps(text)) - 2


def card(result: dict) -> dict:
    """An applied write's answer: every top-level field, and its task as a compact card."""
    if result.get("ok") is True and isinstance(result.get("task"), dict):
        return {**result, "task": {key: value for key, value in result["task"].items() if key in CARD_KEYS}}
    return result


def too_large(field: str) -> dict:
    """The answer for one journal entry or task that cannot fit one relay result."""
    return {"ok": False, "error": {"code": "too_large", "field": field, "state": "invalid",
                                   "message": "too large to show on the phone; open it on the computer"}}


def digest(projects, decisions) -> dict:
    """`mobile_overmind` from native `projects` and `decisions` answers, at most 64 KiB:
    `{schema, rev, projects: {id: {role: count}}, decisions: [{task, project, title, signature}]}`.
    Every Your Call entry fits (bounded fields, titles clipped to 120 characters and 600
    escaped bytes); project counts fill the remaining room in hgs order."""
    if not (isinstance(projects, dict) and projects.get("ok") is True and isinstance(projects.get("projects"), list)
            and isinstance(decisions, dict) and decisions.get("ok") is True and isinstance(decisions.get("decisions"), list)):
        raise ValueError("native Overmind digest is invalid")
    rev = decisions.get("cursor")
    value = {"schema": 1, "rev": rev if type(rev) is int else None, "projects": {}, "decisions": []}
    for row in decisions["decisions"][:DECISIONS_MAX]:
        if (isinstance(row, dict) and all(isinstance(row.get(key), str) for key in ("task", "project", "title", "signature"))
                and all(escaped(row[key]) <= bound for key, bound in ENTRY_BOUNDS.items())):
            title = row["title"][:TITLE_CHARS]
            while escaped(title) > TITLE_BYTES:
                title = title[:-1]
            value["decisions"].append({"task": row["task"], "project": row["project"], "title": title, "signature": row["signature"]})
    room = DIGEST_MAX - len(json.dumps(value))
    for row in projects["projects"]:
        if room >= 0 and isinstance(row, dict) and isinstance(row.get("project"), str) and isinstance(row.get("roles"), dict):
            roles = {role: count for role, count in row["roles"].items() if type(count) is int}
            room -= len(json.dumps({row["project"]: roles})) + 2
            if room >= 0:
                value["projects"][row["project"]] = roles
    return value


def decisions(snapshot) -> dict | None:
    """Your Call entries of a snapshot's digest as {`overmind:<task>`: signature} in digest order.

    None: no `mobile_overmind.decisions` array, which is missing data, not an empty
    list. Of the first 50 entries, those with a canonical UUID task and a 1-128
    character signature without controls count, the first entry per task.
    """
    value = snapshot.get("mobile_overmind") if isinstance(snapshot, dict) else None
    items = value.get("decisions") if isinstance(value, dict) else None
    if not isinstance(items, list):
        return None
    found = {}
    for item in items[:DECISIONS_MAX]:
        task, signature = (item.get("task"), item.get("signature")) if isinstance(item, dict) else (None, None)
        if (canonical_uuid(task) and isinstance(signature, str) and 1 <= len(signature) <= 128
                and not any(ord(c) < 32 or ord(c) == 127 for c in signature)):
            found.setdefault(DECISION_SLOT + task, signature)
    return found


def decision_changes(previous, snapshot) -> list[tuple[str, str]]:
    """New or re-signed entries of the gateway's own digest. A digest missing on
    either side raises nothing; leaving the list raises nothing."""
    before, current = decisions(previous), decisions(snapshot)
    if before is None or current is None:
        return []
    return [(slot, "decision") for slot, signature in current.items() if before.get(slot) != signature]


def read_range(answer: dict, request: dict) -> dict:
    """One range of the local file `hgs overmind attachment` named, as `{size, data_base64}` with its identity."""
    path, size, offset, length = answer.get("path"), answer.get("bytes"), request["offset"], request["length"]
    if not isinstance(path, str) or not os.path.isabs(path) or type(size) is not int or not 0 < size <= MAX_FILE_BYTES:
        raise ValueError("native attachment answer is invalid")
    if os.path.basename(path) != request.get("sha256"):  # hgs blobs are content-addressed.
        raise ValueError("native attachment path does not name the requested image")
    if offset >= size:
        raise ValueError("attachment range is outside the file")
    try:
        descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
    except OSError:
        raise ValueError("attachment is not available on the gateway computer") from None
    with os.fdopen(descriptor, "rb") as file:
        info = os.fstat(file.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_size != size:
            raise ValueError("attachment changed on the gateway computer")
        file.seek(offset)
        data = file.read(length)
    return {"ok": True, "task": request.get("task"), "sha256": request.get("sha256"), "name": answer.get("name"),
            "mime": answer.get("mime"), "size": size, "offset": offset, "length": len(data),
            "data_base64": base64.b64encode(data).decode("ascii")}
