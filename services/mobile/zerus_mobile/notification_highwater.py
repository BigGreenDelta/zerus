"""Permanent physical-computer preferences, independent of route snapshots.

The caller owns its SQLite immediate transaction; no network or native calls.
"""
import json
from .routes import query, RouteError

MAX_BYTES = 1024 * 1024
MAX_SCOPES = 4096


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False)


def key(row):
    if not isinstance(row, dict):
        return None
    name, conversation = row.get('name'), row.get('conversation_id')
    if not isinstance(name, str) or not isinstance(conversation, str):
        return None
    provider = name.split('/', 1)[0]
    if any(not v or len(v.encode()) > 256 or any(ord(c) < 32 or ord(c) == 127 for c in v)
           for v in (provider, conversation)):
        return None
    return provider, conversation


def preference(row):
    if key(row) is None:
        return None
    p = row.get('notification_preferences')
    return p if (isinstance(p, dict) and p.get('conversation_id') == row['conversation_id']
                 and type(p.get('muted')) is bool and type(p.get('revision')) is int
                 and 0 <= p['revision'] <= (1 << 63)-1 and isinstance(p.get('suppressed'), dict)) else None


def insert(ledger, scope, pref, reject_older=False):
    if len(canonical(pref).encode()) > 384 * 1024:
        raise RouteError(413, 'notification preference is too large')
    old = ledger.get(scope)
    if old:
        if old['revision'] == pref['revision'] and old != pref:
            raise RouteError(409, 'conflicting notification revision')
        if pref['revision'] < old['revision']:
            if reject_older:
                raise RouteError(409, 'stale notification revision')
            return
    ledger[scope] = pref
    if len(ledger) > MAX_SCOPES:
        raise RouteError(413, 'notification highwater is full')


def bounded_values(value):
    pending, count = [(value, 1)], 0
    while pending:
        value, depth = pending.pop()
        count += 1
        if count > 50000 or (isinstance(value, (dict, list)) and depth > 32):
            raise RouteError(413, 'notification highwater JSON bound exceeded')
        if isinstance(value, dict):
            count += len(value)  # Object keys also consume the shared codec value budget.
            pending.extend((v, depth + 1) for v in value.values())
        elif isinstance(value, list):
            pending.extend((v, depth + 1) for v in value)
    if count > 50000:
        raise RouteError(413, 'notification highwater JSON bound exceeded')


def load(computer):
    row = yield from query('SELECT CASE WHEN payload_bytes<=1049088 THEN preferences END AS preferences,payload_bytes FROM computer_notification_preferences WHERE computer_id=?', computer)
    if row is None:
        return {}, 0
    raw = row['preferences']
    if not isinstance(raw, str) or len(raw.encode()) > MAX_BYTES:
        raise RouteError(413, 'notification highwater is too large')
    stored_bytes = row['payload_bytes']
    entries = json.loads(raw)
    bounded_values(entries)
    if not isinstance(entries, list) or len(entries) > MAX_SCOPES:
        raise RouteError(409, 'invalid notification highwater')
    ledger = {}
    for entry in entries:
        if not isinstance(entry, dict):
            raise RouteError(409, 'invalid notification highwater')
        row = {'name': entry.get('provider'), 'conversation_id': entry.get('conversation_id'),
               'notification_preferences': entry.get('preferences')}
        scope, pref = key(row), preference(row)
        if scope is None or pref is None or scope in ledger:
            raise RouteError(409, 'invalid notification highwater')
        insert(ledger, scope, pref, True)
    return ledger, stored_bytes


def save(workspace, computer, ledger, old_bytes, credit=0):
    entries = [{'provider': k[0], 'conversation_id': k[1], 'preferences': v}
               for k, v in sorted(ledger.items())]
    bounded_values(entries)
    encoded = canonical(entries)
    if len(encoded.encode()) > MAX_BYTES:
        raise RouteError(413, 'notification highwater is too large')
    size = len(encoded.encode()) + len(workspace.encode()) + len(computer.encode())
    delta = size - old_bytes - credit
    usage = yield from query('SELECT payload_bytes,budget_bytes FROM workspace_usage WHERE workspace_id=?', workspace)
    if usage is None or (delta > 0 and usage['payload_bytes'] + delta > usage['budget_bytes']):
        raise RouteError(429, 'notification highwater quota exceeded')
    yield from query('INSERT INTO computer_notification_preferences(computer_id,workspace_id,preferences,payload_bytes) VALUES(?,?,?,?) ON CONFLICT(computer_id) DO UPDATE SET preferences=excluded.preferences,payload_bytes=excluded.payload_bytes', computer, workspace, encoded, size, result='none')
    yield from query('UPDATE workspace_usage SET payload_bytes=payload_bytes+? WHERE workspace_id=?', delta, workspace, result='none')


def accept(workspace, computer, snapshot):
    ledger, size = yield from load(computer)
    before = dict(ledger)
    incoming = {}
    for row in snapshot.get('sessions', []):
        scope, pref = key(row), preference(row)
        if scope is None:
            continue
        if pref is None:
            if scope in ledger:
                raise RouteError(409, 'missing confirmed notification preference')
        else:
            insert(incoming, scope, pref)
    for scope, pref in incoming.items():
        insert(ledger, scope, pref, True)
    if ledger != before:
        yield from save(workspace, computer, ledger, size)


def overlay(computer, snapshot):
    ledger, _ = yield from load(computer)
    if not isinstance(snapshot, dict):
        return snapshot
    projected = len(canonical(snapshot).encode())
    if projected > MAX_BYTES:
        raise RouteError(413, 'projected snapshot is too large')
    sizes = {scope: len(canonical(pref).encode()) for scope, pref in ledger.items()}
    for row in snapshot.get('sessions', []):
        replacement = sizes.get(key(row))
        if replacement is not None:
            if 'notification_preferences' in row:
                projected += replacement - len(canonical(row['notification_preferences']).encode())
            else:
                projected += len('"notification_preferences":') + replacement + 1
    if projected > MAX_BYTES:
        raise RouteError(413, 'projected snapshot is too large')
    for row in snapshot.get('sessions', []):
        pref = ledger.get(key(row))
        if pref is not None:
            row['notification_preferences'] = pref
    if len(canonical(snapshot).encode()) > MAX_BYTES:
        raise RouteError(413, 'projected snapshot is too large')
    bounded_values(snapshot)
    return snapshot


def merge(workspace, old, target):
    if old == target:
        return
    source, credit = yield from load(old)
    if not source:
        return
    ledger, size = yield from load(target)
    for scope, pref in source.items():
        insert(ledger, scope, pref)
    yield from save(workspace, target, ledger, size, credit)
    yield from query('DELETE FROM computer_notification_preferences WHERE computer_id=?', old, result='none')
