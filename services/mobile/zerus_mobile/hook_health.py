"""Content-free read-only hook health. Repairs are never exposed to phones."""
from .context import validate_context
import math

OPERATION = "hooks_status"
HELP_MARKER = "hooks-health-v1"
UNAVAILABLE = "native hook health ABI is unavailable"


def validate(payload, request_id):
    validate_context(payload, request_id)


def metadata_valid(result):
    configuration=result.get("configuration")
    if not isinstance(configuration,dict) or set(configuration)-{"state","compatibility","sha256","path_id","expected_events","registered_events","missing_events","duplicate_events","conditional_matchers","answer_channel_registered"}: return False
    if configuration.get("state") not in {"configured","missing","partial","invalid","unreadable","not_applicable"}: return False
    compatibility=configuration.get("compatibility")
    if not isinstance(compatibility,dict) or set(compatibility)-{"state","basis","native_version","unsupported_registered_events"} or compatibility.get("state") not in {"compatible","incompatible","unknown","not_applicable"}: return False
    for key in ("sha256","path_id"):
        if key in configuration and (not isinstance(configuration[key],str) or len(configuration[key])!=64 or any(c not in "0123456789abcdef" for c in configuration[key])):return False
    for key in ("expected_events","registered_events","missing_events","duplicate_events"):
        if key in configuration and (not isinstance(configuration[key],list) or len(configuration[key])>32 or any(not isinstance(e,str) or not e.isascii() or not e.isalnum() or len(e)>64 for e in configuration[key])):return False
    for key in ("conditional_matchers","answer_channel_registered"):
        if key in configuration and type(configuration[key]) is not bool:return False
    for key in ("basis","native_version"):
        if key in compatibility and compatibility[key] is not None and (not isinstance(compatibility[key],str) or len(compatibility[key])>128):return False
    if "unsupported_registered_events" in compatibility:
        events=compatibility["unsupported_registered_events"]
        if not isinstance(events,list) or len(events)>32 or any(not isinstance(e,str) or not e or not e.isascii() or not e.isalnum() or len(e)>64 for e in events):return False
    for key in ("effective","trust"):
        value=result.get(key)
        if not isinstance(value,dict) or set(value)!={"state","reason"} or value.get("state")!="unknown" or not isinstance(value.get("reason"),str) or len(value["reason"])>512:return False
    observed=result.get("observed")
    if not isinstance(observed,dict) or set(observed)!={"state","events"} or observed.get("state") not in {"unknown","observed"} or not isinstance(observed.get("events"),list) or len(observed["events"])>32:return False
    for event in observed["events"]:
        if (not isinstance(event,dict) or set(event)!={"event","count","last_at"} or not isinstance(event.get("event"),str) or not event["event"].isascii() or not event["event"].isalnum() or len(event["event"])>64
                or type(event.get("count")) is not int or not 0<event["count"]<=2**64-1 or type(event.get("last_at")) not in (int,float) or not 0<event["last_at"]<1e12 or not math.isfinite(event["last_at"])):return False
    return True


def result_valid(result, payload, session):
    return (isinstance(result, dict) and set(result) == {
        "schema", "provider", "account_id", "configuration", "effective", "trust", "observed",
        "running_agents_retrofitted", "request_id", "name", "run_id", "conversation_id"}
        and type(result.get("schema")) is int and result.get("schema") == 1 and result.get("request_id") == payload["request_id"]
        and result.get("name") == session and result.get("run_id") == payload["expected_run_id"]
        and result.get("conversation_id") == payload["expected_conversation_id"]
        and result.get("running_agents_retrofitted") is False
        and isinstance(result.get("provider"), str) and isinstance(result.get("account_id"), str)
        and metadata_valid(result)
        and isinstance(result.get("configuration"), dict)
        and isinstance(result.get("effective"), dict) and result["effective"].get("state") == "unknown"
        and isinstance(result.get("trust"), dict) and result["trust"].get("state") == "unknown"
        and isinstance(result.get("observed"), dict))
