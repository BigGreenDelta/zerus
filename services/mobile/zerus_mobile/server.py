"""Python API v1 reference for compatibility tests and historical load comparisons.

Production HTTP runs in services/mobile/relay. The supported zerus-mobile CLI
executes that Rust binary; this module remains an independent protocol oracle.
"""
from __future__ import annotations
from . import file_reads, hook_health, session_projects

import asyncio
from collections import OrderedDict, deque
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
import json
import logging
import time
import uuid

from aiohttp import web

from .attachments import LIMITS as ATTACHMENT_LIMITS, MAX_REQUEST_BYTES, MAX_QUEUE_BYTES, validate_read, validate_send
from . import analytics, archives, speech, speech_config, voice, session_notifications
from .recovery import validate_recovery
from .history import validate_history, validate_history_projection
from .context import CLEAR_CONTEXT_DISMISS_FEATURE, validate_clear_context
from .context import COMPUTER_OPERATIONS, validate_mobile_delivery, CONTEXT_COMMANDS, LIFECYCLE_OPERATIONS, OPERATIONS, FEATURES, validate_context, validate_core, validate_inspect, validate_lifecycle, validate_mark_read
from .launch import OPERATIONS as LAUNCH_OPERATIONS, validate as validate_launch
from .routes import RouteError, MAX_PEERS
from .store import Store, digest
from .async_store import adapt_store, StoreBusy
from .work import EXECUTOR
from .ingress import Capacity, blocking, busy, client_identity, parse_json, stream_json, encoded_size
from .terminal import OPERATIONS as TERMINAL_OPERATIONS, validate as validate_terminal


@dataclass
class Config:
    queue_ttl: int = 120
    claim_ttl: int = 90
    retention: int = 7 * 86400
    max_queue: int = 200
    max_queue_bytes: int = MAX_QUEUE_BYTES
    online_timeout: int = 45
    fcm_credentials: str | None = None
    background: bool = True
    trusted_proxy_cidrs: tuple[str, ...] = ()
    max_forwarded_hops: int = 8
    max_anonymous_requests: int = 16
    max_authenticated_requests: int = 112
    max_body_bytes: int = 8 * 1024 * 1024
    large_payload_slots: int = 1
    max_json_depth: int = 32
    max_json_values: int = 50000
    max_polls_per_credential: int = 2
    max_polls_per_workspace: int = 32
    max_polls_global: int = 256
    max_small_responses: int = 8
    max_catalog_bytes: int = 1024 * 1024
    max_rate_entries_authenticated: int = 100000
    max_rate_entries_anonymous: int = 10000
    maintenance_interval: float = 1
    maintenance_batch: int = 256


STORE = web.AppKey("store", Store)
CONFIG = web.AppKey("config", Config)
LIMITS = web.AppKey("limits", object)
ANONYMOUS = web.AppKey("anonymous", object)
AUTH_LOOKUPS = web.AppKey("auth_lookups", object)
AUTHENTICATED = web.AppKey("authenticated", object)
BODY_BUDGET = web.AppKey("body_budget", object)
POLLS = web.AppKey("polls", object)
RESPONSES = web.AppKey("responses", object)
SPOOLS = web.AppKey("spools", object)
READINESS = web.AppKey("readiness", object)
SMALL_EXECUTOR = web.AppKey("small_executor", object)
LARGE_EXECUTOR = web.AppKey("large_executor", object)
PUSH_WORKER = web.AppKey("push_worker", object)
UPLOADS = web.AppKey("uploads", object)


ADMISSION = web.RequestKey("admission", object)
BODY_RESERVED = web.RequestKey("body_reserved", object)
LARGE_PAYLOAD = web.RequestKey("large_payload", object)
SMALL_RESPONSE = web.RequestKey("small_response", object)
CLIENT_IDENTITY = web.RequestKey("client_identity", object)

AUTHENTICATED_REQUEST_LIMIT = 2400
IP_REQUEST_LIMIT = 2400
GLOBAL_REQUEST_LIMIT = 6000


class RateLimits:
    """Legacy standalone limiter; live HTTP uses shared backend counters."""
    def __init__(self):
        self.entries = OrderedDict()

    def check(self, key, limit=120, seconds=60):
        now = time.monotonic()
        values = self.entries.setdefault(key, deque())
        self.entries.move_to_end(key)
        while values and values[0] < now - seconds:
            values.popleft()
        if len(values) >= limit:
            raise busy("rate limit exceeded")
        values.append(now)
        while len(self.entries) > 10000:
            self.entries.popitem(last=False)


async def rate(request, key, limit=120, seconds=60):
    cfg = request.app[CONFIG]
    maximum = cfg.max_rate_entries_anonymous if key[0] in ("login", "pair") else cfg.max_rate_entries_authenticated
    if not await request.app[STORE].rate_limit(key, limit, seconds, max_entries=maximum):
        raise busy("rate limit exceeded")


@web.middleware
async def protect(request, handler):
    executor_token = EXECUTOR.set(request.app[SMALL_EXECUTOR])
    request[ADMISSION] = None
    request[BODY_RESERVED] = 0
    request[LARGE_PAYLOAD] = False
    request[SMALL_RESPONSE] = False
    try:
        # Liveness is independent of the DB and all transport/rate admission.
        if request.path == "/healthz":
            return await handler(request)
        request[CLIENT_IDENTITY] = client_identity(request, request.app[CONFIG])
        if request.headers.get("Content-Encoding", "identity") != "identity":
            raise web.HTTPUnsupportedMediaType()
        # GET handlers authenticate before entering their separate capacity lane.
        # Pairing has no credential and takes anonymous capacity before its body.
        if request.path == "/v1/pair":
            request.app[ANONYMOUS].take()
            request[ADMISSION] = ANONYMOUS
        deadline = 75 if request.method == "POST" and request.path == "/v1/requests" else 40
        response = await asyncio.wait_for(handler(request), timeout=deadline)
    except web.HTTPException as exc:
        if exc.content_type == "application/json":
            response = web.Response(body=exc.body, status=exc.status, headers=exc.headers)
        else:
            response = web.json_response({"error": exc.reason}, status=exc.status, headers={k: v for k, v in exc.headers.items() if k.lower() not in ("content-type", "content-length")})
    except RouteError as exc:
        response=web.json_response({"error":exc.message},status=exc.status)
    except (ValueError, TypeError, KeyError, RecursionError):
        response = web.json_response({"error": "invalid JSON request"}, status=400)
    except asyncio.TimeoutError:
        response = web.json_response({"error": "request timed out"}, status=408)
    except StoreBusy:
        response = web.json_response({"error": "storage capacity is busy"}, status=429, headers={"Retry-After": "1"})
    except Exception:
        # Driver errors can contain SQL parameters. Keep all diagnostics generic.
        logging.getLogger(__name__).warning("relay request deferred after internal failure")
        response = web.json_response({"error": "relay temporarily unavailable"}, status=503, headers={"Retry-After": "1"})
    finally:
        if request[ADMISSION] is not None:
            request.app[request[ADMISSION]].release()
        if request[BODY_RESERVED]:
            request.app[BODY_BUDGET].release(request[BODY_RESERVED])
        if request[LARGE_PAYLOAD]:
            request.app[UPLOADS].release()
        if request[SMALL_RESPONSE]:
            request.app[RESPONSES].release()
        EXECUTOR.reset(executor_token)
    response.headers["Cache-Control"] = "no-store"
    response.headers["X-Content-Type-Options"] = "nosniff"
    return response


async def auth(request, role):
    # Bound credential lookups separately: anonymous traffic cannot occupy the
    # authenticated lane, and rate counters use only verified identities.
    request.app[AUTH_LOOKUPS].take()
    request[ADMISSION] = AUTH_LOOKUPS
    authorization = request.headers.get("Authorization", "")
    if not authorization.startswith("Bearer ") or len(authorization) > 256:
        await rate(request, ("login", request[CLIENT_IDENTITY]), limit=20)
        raise web.HTTPUnauthorized()
    secret = authorization[7:]
    row = await request.app[STORE].authenticate(secret, role)
    if row is None:
        await rate(request, ("login", request[CLIENT_IDENTITY]), limit=20)
        raise web.HTTPUnauthorized()
    request.app[AUTHENTICATED].take()
    request.app[AUTH_LOOKUPS].release()
    request[ADMISSION] = AUTHENTICATED
    await rate(request, (role, row["workspace_id"], row["id"]), limit=AUTHENTICATED_REQUEST_LIMIT)
    return row


async def still_authorized(request, row, role):
    if not await request.app[STORE].authorized(row["id"], role):
        raise web.HTTPUnauthorized()


def large_payload(request):
    if not request[LARGE_PAYLOAD]:
        request.app[UPLOADS].take()
        request[LARGE_PAYLOAD] = True
        EXECUTOR.set(request.app[LARGE_EXECUTOR])


async def body(request, required, optional=(), maximum=1024 * 1024, timeout=10):
    if request.content_type != "application/json":
        raise web.HTTPUnsupportedMediaType()
    if request.content_length is not None and request.content_length > maximum:
        raise web.HTTPRequestEntityTooLarge(max_size=maximum, actual_size=request.content_length)
    large = request.content_length is None or request.content_length > 1024 * 1024
    if large:
        large_payload(request)
    else:
        reserve = request.content_length or 0
        request.app[BODY_BUDGET].take(reserve)
        request[BODY_RESERVED] += reserve
    raw = bytearray()
    async with asyncio.timeout(timeout):
        async for chunk in request.content.iter_chunked(65536):
            if len(raw) + len(chunk) > maximum:
                raise web.HTTPRequestEntityTooLarge(max_size=maximum, actual_size=len(raw) + len(chunk))
            raw.extend(chunk)
    cfg = request.app[CONFIG]
    try:
        value = await blocking(parse_json, raw, cfg.max_json_depth, cfg.max_json_values)
    finally:
        raw = None
    if not isinstance(value, dict) or set(value) - set(required) - set(optional) or not set(required) <= set(value):
        raise web.HTTPBadRequest()
    return value


def text(value, maximum=1024, empty=False):
    if not isinstance(value, str) or len(value) > maximum or (not empty and not value) or any(ord(c) < 32 for c in value):
        raise web.HTTPBadRequest()
    return value


def wait_seconds(request):
    value = float(request.query.get("wait", "0"))
    if not 0 <= value <= 25:
        raise web.HTTPBadRequest()
    return value


async def health(request):
    return web.json_response({"ok": True, "protocol_version": 1})


async def pair(request):
    await rate(request, ("pair", request[CLIENT_IDENTITY]), limit=10)
    value = await body(request, ("code", "device_name"))
    result = await request.app[STORE].pair(text(value["code"], 128), text(value["device_name"], 128))
    if result is None:
        raise web.HTTPUnauthorized()
    return web.json_response(result)


async def capabilities(request):
    await auth(request, "devices")
    providers = ["fcm"] if request.app[PUSH_WORKER].fcm is not None else []
    return web.json_response({"protocol_version": 1, "push_providers": providers, "attachment_limits": ATTACHMENT_LIMITS, "operations": sorted(OPERATIONS), "features": sorted(FEATURES | {voice.video.FEATURE, CLEAR_CONTEXT_DISMISS_FEATURE, session_projects.FEATURE})})


def small_response(request):
    if not request[SMALL_RESPONSE]:
        request.app[RESPONSES].take()
        request[SMALL_RESPONSE] = True


async def computers(request):
    device = await auth(request, "devices")
    small_response(request)
    now = time.time()
    rows = await request.app[STORE].computers(device["workspace_id"], max_bytes=request.app[CONFIG].max_catalog_bytes)
    if rows is None:
        raise web.HTTPRequestEntityTooLarge(max_size=request.app[CONFIG].max_catalog_bytes, actual_size=request.app[CONFIG].max_catalog_bytes + 1)
    result = {"computers": [{"id": row["id"], "name": row["name"], "online": row.get("available",True) and row["last_seen"] is not None and now - row["last_seen"] < request.app[CONFIG].online_timeout, "last_seen_at": row["last_seen_at"], "snapshot": await blocking(json.loads, row["snapshot"]) if row["snapshot"] else None, "machine_id":row.get("machine_id"),"aliases":row.get("aliases",[]),"via":row.get("via")} for row in rows]}
    return await stream_json(request, result, budget=request.app[SPOOLS], maximum=1024 * 1024)


def validate_request(value):
    request_id = text(value["request_id"], 36)
    if str(uuid.UUID(request_id)) != request_id:
        raise web.HTTPBadRequest()
    text(value["computer_id"], 36)
    text(value["session"], 1024, empty=value["operation"] in analytics.OPERATIONS or value["operation"] == "archive_list" or value["operation"] in LAUNCH_OPERATIONS or value["operation"] in COMPUTER_OPERATIONS or value["operation"] in speech.OPERATIONS or value["operation"] in speech_config.OPERATIONS)
    op, payload = value["operation"], value["payload"]
    if not isinstance(op, str) or op not in OPERATIONS or not isinstance(payload, dict):
        raise web.HTTPBadRequest()
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
                raise web.HTTPBadRequest()
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
        raise web.HTTPBadRequest()
    text(payload.get("expected_run_id"), 128)
    text(payload.get("expected_conversation_id"), 256, empty=True)
    base = {"request_id", "expected_run_id", "expected_conversation_id"}
    extras = {"send": {"text", "attachments", "expected_compaction_id", "agent_id"}, "answer": {"question_id", "expected_question_hash", "answers"}, "interrupt": {"expected_turn_started"}}[op]
    if set(payload) - base - extras:
        raise web.HTTPBadRequest()
    required = {"send": {"text"}, "answer": {"question_id", "expected_question_hash", "answers"}, "interrupt": {"expected_turn_started"}}[op]
    if not required <= set(payload):
        raise web.HTTPBadRequest()


async def submit(request):
    device = await auth(request, "devices")
    small_response(request)
    value = await body(request, ("request_id", "computer_id", "operation", "session", "payload"), maximum=MAX_REQUEST_BYTES, timeout=60)
    await blocking(validate_request, value)
    if value["operation"] == "send":
        await blocking(validate_send, value["payload"])
    elif value["operation"] == "voice_submit" and "video" in value["payload"]:
        maximum = voice.video.MAX_REQUEST_BYTES
        if await blocking(encoded_size, value, maximum) > maximum:
            raise web.HTTPRequestEntityTooLarge(max_size=maximum, actual_size=maximum + 1)
    elif value["operation"] in {"speech_submit", "voice_submit"}:
        if await blocking(encoded_size, value, 6 * 1024 * 1024) > 6 * 1024 * 1024:
            raise web.HTTPRequestEntityTooLarge(max_size=6 * 1024 * 1024, actual_size=6 * 1024 * 1024 + 1)
    elif await blocking(encoded_size, value, 1024 * 1024) > 1024 * 1024:
        raise web.HTTPRequestEntityTooLarge(max_size=1024 * 1024, actual_size=1024 * 1024 + 1)
    await still_authorized(request, device, "devices")
    status, result = await request.app[STORE].submit(device, value, request.app[CONFIG].max_queue, request.app[CONFIG].max_queue_bytes)
    return await stream_json(request, result, status=status, budget=request.app[SPOOLS], maximum=1024 * 1024)


async def get_request(request):
    device = await auth(request, "devices")
    small_response(request)
    cfg = request.app[CONFIG]
    row = await request.app[STORE].get_request(device, request.match_info["id"], queue_ttl=cfg.queue_ttl, claim_ttl=cfg.claim_ttl)
    if row is None:
        raise web.HTTPNotFound()
    return await stream_json(request, row, budget=request.app[SPOOLS], maximum=1024 * 1024)


def native_uuid(value):
    if not isinstance(value,str) or len(value)!=36 or str(uuid.UUID(value))!=value:
        raise web.HTTPBadRequest()
    return value


def validate_snapshot(snap):
    if not isinstance(snap,dict) or not isinstance(snap.get('sessions'),list) or len(snap['sessions'])>5000:
        raise web.HTTPBadRequest()
    for session in snap['sessions']:
        if not isinstance(session,dict):raise web.HTTPBadRequest()
        if 'name' in session:text(session['name'],1024)


async def heartbeat(request):
    node=await auth(request,'nodes')
    value=await body(request,('snapshot',),('machine_id','peers'))
    validate_snapshot(value['snapshot'])
    machine=native_uuid(value['machine_id']) if 'machine_id' in value else None
    peers=value.get('peers',[])
    if not isinstance(peers,list) or len(peers)>MAX_PEERS:raise web.HTTPBadRequest()
    seen=set()
    for peer in peers:
        if not isinstance(peer,dict) or set(peer)!={'route_id','machine_id','name','online'}:raise web.HTTPBadRequest()
        native_uuid(peer['route_id']);native_uuid(peer['machine_id']);text(peer['name'],128)
        if type(peer['online']) is not bool or peer['route_id'] in seen:raise web.HTTPBadRequest()
        seen.add(peer['route_id'])
    if peers and machine is None:raise web.HTTPBadRequest()
    await still_authorized(request,node,'nodes')
    await request.app[STORE].heartbeat(node,value['snapshot'],machine_id=machine,peers=peers)
    return web.json_response({'ok':True})


async def peer_heartbeat(request):
    node=await auth(request,'nodes')
    route=native_uuid(request.match_info['route_id'])
    value=await body(request,('machine_id','snapshot'))
    machine=native_uuid(value['machine_id']);validate_snapshot(value['snapshot'])
    await still_authorized(request,node,'nodes')
    await request.app[STORE].peer_heartbeat(node,route,machine,value['snapshot'])
    return web.json_response({'ok':True})


async def poll_lease(request, row, role, wait):
    if not wait:
        return None
    cfg = request.app[CONFIG]
    request.app[POLLS].take()
    try:
        lease = await request.app[STORE].acquire_poll(role, row["id"], row["workspace_id"], wait + 10, cfg.max_polls_per_credential, cfg.max_polls_per_workspace)
        if lease is None:
            raise busy("long poll capacity is busy")
        # Idle waits own dedicated poll capacity, leaving ordinary authenticated
        # admission available for heartbeats, acknowledgements and submissions.
        request.app[request[ADMISSION]].release()
        request[ADMISSION] = None
        return lease
    except BaseException:
        request.app[POLLS].release()
        raise


async def release_poll(request, lease):
    if lease is not None:
        task = None
        try:
            task = asyncio.create_task(request.app[STORE].release_poll(lease))
            await asyncio.shield(task)
        except asyncio.CancelledError:
            # The local slot budgets the durable cleanup as well as the wait.
            # Repeated cancellation cannot permit a replacement poll while
            # the previous SQL release still consumes cleanup capacity.
            while not task.done():
                try:
                    await asyncio.shield(task)
                except asyncio.CancelledError:
                    continue
                except Exception:
                    break
            if task.done() and not task.cancelled():
                task.exception()
            raise
        finally:
            request.app[POLLS].release()
            task = None


async def node_requests(request):
    node = await auth(request, "nodes")
    optin=request.query.get('gateway_one_hop','0')
    if optin not in ('0','1'):raise web.HTTPBadRequest()
    allow_gateway=optin=='1'
    wait = wait_seconds(request)
    lease = await poll_lease(request, node, "nodes", wait)
    deadline = time.monotonic() + wait
    topic = "node:" + node["id"]
    cfg = request.app[CONFIG]
    try:
        while True:
            generation = request.app[STORE].generation(topic)
            await still_authorized(request, node, "nodes")
            # The payload lane covers retrieval and serialization, but never an
            # idle long poll. A busy lane leaves the command safely queued.
            rows = []
            if await request.app[STORE].has_pending(node, queue_ttl=cfg.queue_ttl, allow_gateway=allow_gateway):
                large_payload(request)
                rows = await request.app[STORE].claim(node, queue_ttl=cfg.queue_ttl, claim_ttl=cfg.claim_ttl, allow_gateway=allow_gateway)
            if rows:
                return await stream_json(request, {"requests": rows}, budget=request.app[SPOOLS], maximum=32 * 1024 * 1024)
            if request[LARGE_PAYLOAD]:
                request.app[UPLOADS].release()
                request[LARGE_PAYLOAD] = False
            if time.monotonic() >= deadline:
                return web.json_response({"requests": []})
            await request.app[STORE].wait(topic, generation, min(5, max(0, deadline - time.monotonic())))
    finally:
        await release_poll(request, lease)


async def node_result(request):
    node = await auth(request, "nodes")
    value = await body(request, ("state", "result", "error"))
    if value["state"] not in ("completed", "failed", "uncertain"):
        raise web.HTTPBadRequest()
    if value["error"] is not None:
        # Errors are data, never placed in logs.
        if not isinstance(value["error"], str) or len(value["error"]) > 4096:
            raise web.HTTPBadRequest()
    receipt = {"request_id": request.match_info["id"], "state": value["state"], "result": value["result"], "error": value["error"]}
    if await blocking(encoded_size, receipt, 1024 * 1024) > 1024 * 1024:
        raise web.HTTPRequestEntityTooLarge(max_size=1024 * 1024, actual_size=1024 * 1024 + 1)
    await still_authorized(request, node, "nodes")
    status = await request.app[STORE].result(node, request.match_info["id"], value, claim_ttl=request.app[CONFIG].claim_ttl)
    return web.json_response({"ok": status == 200}, status=status)


def event_page(rows, after):
    # Reserve the longest possible int8 cursor; each returned prefix fits the
    # installed phone's 1 MiB total response cap even with Unicode escaping.
    maximum = 1024 * 1024
    used = encoded_size({"events": [], "cursor": 9223372036854775807}, maximum)
    events = []
    for row in rows:
        event = {"id": row["id"], "computer_id": row["node_id"], "session": row["session"], "kind": row["kind"], "created_at": row["created"]}
        size = encoded_size(event, maximum) + (1 if events else 0)
        if used + size > maximum:
            if not events:
                raise web.HTTPRequestEntityTooLarge(max_size=maximum, actual_size=used + size)
            break
        events.append(event)
        used += size
    return {"events": events, "cursor": events[-1]["id"] if events else after}


async def events(request):
    device = await auth(request, "devices")
    after = int(request.query.get("after", "0"))
    if not 0 <= after <= 9223372036854775807:
        raise web.HTTPBadRequest()
    wait = wait_seconds(request)
    lease = await poll_lease(request, device, "devices", wait)
    deadline = time.monotonic() + wait
    topic = "workspace:" + device["workspace_id"]
    try:
        while True:
            generation = request.app[STORE].generation(topic)
            await still_authorized(request, device, "devices")
            small_response(request)
            rows = await request.app[STORE].events(device["workspace_id"], after, max_bytes=1024 * 1024)
            if rows is None:
                raise web.HTTPRequestEntityTooLarge(max_size=1024 * 1024, actual_size=1024 * 1024 + 1)
            if rows or time.monotonic() >= deadline:
                return await stream_json(request, await blocking(event_page, rows, after), budget=request.app[SPOOLS], maximum=1024 * 1024)
            request.app[RESPONSES].release()
            request[SMALL_RESPONSE] = False
            await request.app[STORE].wait(topic, generation, min(5, max(0, deadline - time.monotonic())))
    finally:
        await release_poll(request, lease)


async def push_register(request):
    device = await auth(request, "devices")
    value = await body(request, ("provider",), ("endpoint", "token"))
    worker = request.app[PUSH_WORKER]
    if value["provider"] == "fcm":
        if set(value) != {"provider", "token"}:
            raise web.HTTPBadRequest()
        if worker.fcm is None:
            raise web.HTTPServiceUnavailable(text='{"error":"FCM is not configured"}', content_type="application/json")
        target = text(value["token"], 4096)
    else:
        raise web.HTTPBadRequest()
    await still_authorized(request, device, "devices")
    await request.app[STORE].register_push(device["id"], value["provider"], target)
    return web.json_response({"ok": True})


async def push_delete(request):
    device = await auth(request, "devices")
    await request.app[STORE].delete_push(device["id"])
    return web.json_response({"ok": True})


async def device_delete(request):
    device = await auth(request, "devices")
    await request.app[STORE].revoke("devices", device["id"])
    return web.json_response({"ok": True})


async def maintenance(app):
    cfg = app[CONFIG]
    while True:
        try:
            await app[STORE].maintain(queue_ttl=cfg.queue_ttl, claim_ttl=cfg.claim_ttl, retention=cfg.retention, batch_size=cfg.maintenance_batch)
        except Exception:
            logging.getLogger(__name__).warning("relay maintenance deferred after storage failure")
        await asyncio.sleep(cfg.maintenance_interval)


async def push_background(app):
    while True:
        try:
            await app[PUSH_WORKER].once()
        except Exception:
            logging.getLogger(__name__).warning("relay push delivery deferred after worker failure")
        await asyncio.sleep(1)


async def readiness(request):
    state = request.app[READINESS]
    now = time.monotonic()
    if state["task"] is None and now >= state["expires"]:
        async def probe():
            try:
                state["ok"] = bool(await asyncio.wait_for(request.app[STORE].ready(), 2))
            except Exception:
                state["ok"] = False
            finally:
                state["expires"] = time.monotonic() + 1
                state["task"] = None
        state["task"] = asyncio.create_task(probe())
        # Only the initiating request awaits the probe. Other callers use the
        # cached state, initially unavailable, without reserving a DB waiter.
        await asyncio.shield(state["task"])
    return web.json_response({"ok": state["ok"]}, status=200 if state["ok"] else 503)


async def lifecycle(app):
    from .push import PushWorker
    app[PUSH_WORKER] = PushWorker(app[STORE], app[CONFIG])
    cfg = app[CONFIG]
    await app[STORE].maintain(restart=True, queue_ttl=cfg.queue_ttl, claim_ttl=cfg.claim_ttl, retention=cfg.retention, batch_size=cfg.maintenance_batch)
    tasks = [asyncio.create_task(maintenance(app)), asyncio.create_task(push_background(app))] if cfg.background else []
    try:
        yield
    finally:
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        readiness_task = app[READINESS]["task"]
        if readiness_task is not None:
            await readiness_task
        await app[PUSH_WORKER].close()
        await app[STORE].close()
        await asyncio.to_thread(app[LARGE_EXECUTOR].shutdown, wait=True)
        await asyncio.to_thread(app[SMALL_EXECUTOR].shutdown, wait=True)


def create_app(store: Store, config: Config | None = None) -> web.Application:
    app = web.Application(client_max_size=1024 * 1024, middlewares=[protect])
    app[STORE], app[CONFIG] = adapt_store(store), config or Config()
    cfg = app[CONFIG]
    # Validate trust configuration once before serving requests.
    import ipaddress
    for cidr in cfg.trusted_proxy_cidrs:
        ipaddress.ip_network(cidr)
    for setting in (cfg.max_anonymous_requests, cfg.max_authenticated_requests, cfg.max_body_bytes, cfg.large_payload_slots, cfg.max_polls_global, cfg.max_polls_per_credential, cfg.max_polls_per_workspace, cfg.max_small_responses, cfg.max_catalog_bytes):
        if setting < 1: raise ValueError("admission limits must be positive")
    configure_cleanup = getattr(app[STORE], "configure_cleanup", None)
    if configure_cleanup is not None:
        configure_cleanup(cfg.max_polls_global + cfg.maintenance_batch)
    app[ANONYMOUS] = Capacity(cfg.max_anonymous_requests)
    app[AUTHENTICATED] = Capacity(cfg.max_authenticated_requests)
    app[AUTH_LOOKUPS] = Capacity(cfg.max_authenticated_requests)
    app[BODY_BUDGET] = Capacity(cfg.max_body_bytes)
    app[POLLS] = Capacity(cfg.max_polls_global)
    app[UPLOADS] = Capacity(cfg.large_payload_slots)
    app[RESPONSES] = Capacity(cfg.max_small_responses)
    app[SPOOLS] = Capacity(64 * 1024 * 1024)
    app[READINESS] = {"task": None, "expires": 0, "ok": False}
    app[LARGE_EXECUTOR] = ThreadPoolExecutor(max_workers=cfg.large_payload_slots, thread_name_prefix="relay-large")
    app[SMALL_EXECUTOR] = ThreadPoolExecutor(max_workers=4, thread_name_prefix="relay-small")
    app.cleanup_ctx.append(lifecycle)
    app.add_routes([
        web.get("/healthz", health), web.get("/readyz", readiness), web.post("/v1/pair", pair),
        web.get("/v1/capabilities", capabilities), web.get("/v1/computers", computers),
        web.post("/v1/requests", submit), web.get("/v1/requests/{id}", get_request),
        web.post("/v1/node/heartbeat", heartbeat), web.post("/v1/node/peers/{route_id}/heartbeat",peer_heartbeat), web.get("/v1/node/requests", node_requests),
        web.post("/v1/node/requests/{id}/result", node_result), web.get("/v1/events", events),
        web.post("/v1/push", push_register), web.delete("/v1/push", push_delete),
        web.delete("/v1/device", device_delete),
    ])
    return app
