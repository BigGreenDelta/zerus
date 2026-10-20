"""Logical project placement only; no directory/launch/provider operation."""
import json
import unicodedata
import uuid

OPERATIONS = frozenset({"project_assignment", "project_assign"})
FEATURE = "session_project_assignment_v1"
HELP_MARKER = "Mobile project assignment ABI: session-project-v1"
UNAVAILABLE = "native session project assignment ABI is unavailable"
MAX_RESULT = 128 * 1024
BASE = frozenset({"request_id", "expected_run_id", "expected_conversation_id", "expected_machine_id"})
ASSIGNMENT = frozenset({"swarm_id", "membership_token", "catalog_epoch", "project_id"})


def text(value, maximum, empty=False):
    return (isinstance(value, str) and (empty or bool(value)) and len(value.encode("utf-8")) <= maximum
            and not any(unicodedata.category(c) == "Cc" for c in value))


def canonical_uuid(value):
    try:
        return isinstance(value, str) and uuid.UUID(value).int != 0 and str(uuid.UUID(value)) == value
    except (ValueError, TypeError, AttributeError):
        return False


def digest(value):
    return isinstance(value, str) and len(value) == 64 and all(c in "0123456789abcdef" for c in value)


def from_help(help_text):
    return (isinstance(help_text, str) and HELP_MARKER in {line.strip() for line in help_text.splitlines()}
            and "swarm session-project --json" in help_text and "swarm assign-session --json" in help_text)


def validate(operation, payload, request_id):
    mutation = operation == "project_assign"
    allowed = BASE | (ASSIGNMENT if mutation else {"cursor"})
    required = BASE | (ASSIGNMENT if mutation else set())
    if (operation not in OPERATIONS or not isinstance(payload, dict) or not required <= set(payload) <= allowed
            or payload.get("request_id") != request_id or not canonical_uuid(payload["request_id"])
            or not canonical_uuid(payload["expected_machine_id"]) or not text(payload["expected_run_id"], 128)
            or not text(payload["expected_conversation_id"], 2048, True)):
        raise ValueError("invalid scoped session project fields")
    if mutation:
        if not canonical_uuid(payload["swarm_id"]) or not digest(payload["membership_token"]) or not digest(payload["catalog_epoch"]) or not text(payload["project_id"],128):
            raise ValueError("invalid scoped session project fields")
    elif "cursor" in payload and not text(payload["cursor"],512):
        raise ValueError("invalid scoped session project fields")


def result_valid(operation, result, payload, session):
    allowed = {"schema", "request_id", "status", "name", "run_id", "conversation_id", "machine_id", "swarm_id", "membership_token", "catalog_epoch", "project_id", "current_project_id", "projects", "next_cursor", "error"}
    if (not isinstance(result,dict) or not set(result) <= allowed or type(result.get("schema")) is not int or result["schema"] != 1
            or result.get("request_id") != payload["request_id"] or result.get("name") != session
            or result.get("run_id") != payload["expected_run_id"] or result.get("conversation_id") != payload["expected_conversation_id"]
            or result.get("machine_id") != payload["expected_machine_id"]
            or len(json.dumps(result,ensure_ascii=False,separators=(",",":")).encode()) > MAX_RESULT
            or ("error" in result and not text(result["error"],2048))):
        return False
    mutation = operation == "project_assign"
    if mutation and any(result.get(key) != payload[key] for key in ASSIGNMENT):
        return False
    if result.get("status") == "failed" or (mutation and result.get("status") == "uncertain"):
        return "error" in result and "projects" not in result and "next_cursor" not in result
    if mutation:
        return (result.get("status") == "assigned" and result.get("current_project_id") == payload["project_id"]
                and "projects" not in result and "next_cursor" not in result and "error" not in result)
    if (result.get("status") != "ok" or not canonical_uuid(result.get("swarm_id")) or not digest(result.get("membership_token"))
            or not digest(result.get("catalog_epoch")) or not text(result.get("current_project_id"),128)
            or "error" in result or "project_id" in result):
        return False
    projects = result.get("projects")
    if not isinstance(projects,list) or len(projects)>100:
        return False
    ids=set()
    for project in projects:
        if (not isinstance(project,dict) or set(project)!={"id","name","color"} or not text(project["id"],128)
                or project["id"] in ids or not text(project["name"],320) or not isinstance(project["color"],str)
                or len(project["color"])!=7 or not project["color"].startswith("#") or any(c not in "0123456789abcdefABCDEF" for c in project["color"][1:])):
            return False
        ids.add(project["id"])
    return "next_cursor" in result and (result["next_cursor"] is None or text(result["next_cursor"],512))
