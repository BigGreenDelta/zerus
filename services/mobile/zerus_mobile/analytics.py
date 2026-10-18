"""Bounded computer-owned aggregate reads; no remote collection controls."""
import datetime
import math
import re
import uuid

OPERATIONS = frozenset({"analytics_read"})
HELP_MARKER = "hgs [@host] analytics --json"
MAX_OUTPUT_BYTES = 1024 * 1024


def validate(payload):
    if (not isinstance(payload, dict) or set(payload) != {"schema", "from", "through"}
            or type(payload["schema"]) is not int or payload["schema"] != 1):
        raise ValueError("invalid analytics read fields")
    days = []
    for key in ("from", "through"):
        value = payload[key]
        if not isinstance(value, str) or not re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}", value):
            raise ValueError("invalid analytics UTC date")
        try:
            days.append(datetime.date.fromisoformat(value))
        except ValueError:
            raise ValueError("invalid analytics UTC date") from None
    if not 0 <= (days[1] - days[0]).days < 90:
        raise ValueError("analytics reads require one to ninety UTC days")


def result_valid(result, payload, machine=None):
    if not isinstance(result, dict) or type(result.get("schema")) is not int or result["schema"] != 1:
        return False
    for key in ("machine_id", "source_epoch"):
        value = result.get(key)
        try:
            identity = uuid.UUID(value) if isinstance(value, str) else None
        except ValueError:
            return False
        if identity is None or (key == "machine_id" and identity.int == 0) or str(identity) != value:
            return False
    return (result.get("source") == "agent"
            and (machine is None or result["machine_id"] == machine)
            and result.get("range") == {key: payload[key] for key in ("from", "through")}
            and type(result.get("revision")) is int and result["revision"] >= 0
            and type(result.get("generated_at")) in (int, float)
            and math.isfinite(result["generated_at"]) and result["generated_at"] >= 0
            and isinstance(result.get("collection"), dict) and isinstance(result.get("coverage"), dict)
            and isinstance(result.get("buckets"), list))
