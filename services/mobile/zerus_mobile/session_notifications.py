"""Shared session notification preferences; the native host is the only writer."""

OPERATION = "session_notifications"
HELP_MARKER = "session-notifications-v1"
MAX_REVISION = (1 << 63) - 1
FIELDS = frozenset({"run_id", "conversation_id", "muted", "expected_revision"})


def validate(payload):
    if not isinstance(payload, dict) or set(payload) != FIELDS:
        raise ValueError("session_notifications requires exact run, conversation, mute and revision fields")
    for field, maximum in (("run_id", 128), ("conversation_id", 256)):
        value = payload[field]
        if (not isinstance(value, str) or not value or len(value.encode("utf-8")) > maximum
                or any(ord(c) < 32 or ord(c) == 127 for c in value)):
            raise ValueError("invalid shared notification identity")
    if type(payload["muted"]) is not bool or type(payload["expected_revision"]) is not int or not 0 <= payload["expected_revision"] <= MAX_REVISION:
        raise ValueError("invalid shared notification preference")


def result_valid(result, payload):
    if not isinstance(result, dict) or type(result.get("ok")) is not bool:
        return False
    if result["ok"] is False:
        return "applied" not in result and result.get("error") in {"not_found", "stale_identity", "conflict", "baseline_unavailable", "capacity", "revision_exhausted"}
    preference = result.get("preferences")
    applied = result.get("applied")
    return (type(applied) is bool
            and result.get("run_id") == payload["run_id"]
            and result.get("conversation_id") == payload["conversation_id"]
            and isinstance(preference, dict)
            and preference.get("conversation_id") == payload["conversation_id"]
            and type(preference.get("muted")) is bool and preference["muted"] == payload["muted"]
            and type(preference.get("revision")) is int
            and preference["revision"] == payload["expected_revision"] + int(applied)
            and 0 <= preference["revision"] <= MAX_REVISION
            and isinstance(preference.get("suppressed"), dict))


def revisions_current(previous, snapshot):
    """Reject a late host poll; preferences never follow mutable session names."""
    def preference(row):
        if not isinstance(row, dict):
            return None
        p = row.get("notification_preferences")
        return p if (isinstance(p, dict) and isinstance(p.get("conversation_id"), str) and p["conversation_id"]
                     and p["conversation_id"] == row.get("conversation_id") and type(p.get("muted")) is bool
                     and type(p.get("revision")) is int and 0 <= p["revision"] <= MAX_REVISION
                     and isinstance(p.get("suppressed"), dict)) else None
    known = {}
    for row in previous.get("sessions", []):
        p = preference(row)
        if p is not None:
            key = (str(row.get("name", "")).split("/", 1)[0], row["conversation_id"])
            if key not in known or p["revision"] > known[key]["revision"]:
                known[key] = p
    for row in snapshot.get("sessions", []):
        p = preference(row)
        old = known.get((str(row.get("name", "")).split("/", 1)[0], row.get("conversation_id", "")))
        if old is not None and (p is None or p["revision"] < old["revision"] or p["revision"] == old["revision"] and p != old):
            return False
    return True
