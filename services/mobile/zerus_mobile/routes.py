"""Bounded one-hop registry for the SQLite reference store."""
from dataclasses import dataclass
import json
import time
import uuid
import hashlib

MAX_PEERS = 32
MAX_PEER_SNAPSHOT_BYTES = 1024 * 1024
MAX_COMPUTERS = 2048
MAX_ALIASES = 4096
MAX_ROUTES = 4096


class RouteError(ValueError):
    def __init__(self, status, message):
        self.status, self.message = status, message
        super().__init__(message)


@dataclass
class Query:
    sql: str
    args: tuple = ()
    result: str = 'one'


def query(sql, *args, result='one'):
    return (yield Query(sql, args, result))


def drive_sync(connection, generator):
    value = None
    while True:
        try:
            q = generator.send(value)
        except StopIteration as complete:
            return complete.value
        sql = q.sql.replace('BYTES(', 'length(CAST(')
        # Explicit marker avoids decoding snapshots merely to measure them.
        sql = sql.replace(' AS_BYTES)', ' AS BLOB))')
        cursor = connection.execute(sql, q.args)
        value = ([dict(r) for r in cursor.fetchall()] if q.result == 'all' else
                 (dict(r) if (r := cursor.fetchone()) is not None else None) if q.result == 'one' else None)


class Registry:
    def create(self, workspace, name, computer_id=None, exposed=False):
        count = yield from query('SELECT count(*) AS n FROM computers WHERE workspace_id=?', workspace)
        if count['n'] >= MAX_COMPUTERS:
            raise RouteError(429, 'workspace computer registry is full')
        computer_id = computer_id or str(uuid.uuid4())
        yield from query('INSERT INTO computers(id,workspace_id,name,exposed) VALUES(?,?,?,?)', computer_id, workspace, name, int(exposed), result='none')
        return computer_id

    def alias(self, workspace, alias, computer):
        existing = yield from query('SELECT computer_id FROM computer_aliases WHERE workspace_id=? AND alias=?', workspace, alias)
        if existing:
            if existing['computer_id'] != computer:
                yield from query('UPDATE computer_aliases SET computer_id=? WHERE workspace_id=? AND alias=?', computer, workspace, alias, result='none')
            return
        count = yield from query('SELECT count(*) AS n FROM computer_aliases WHERE workspace_id=?', workspace)
        if count['n'] >= MAX_ALIASES:
            raise RouteError(429, 'workspace computer aliases are full')
        yield from query('INSERT INTO computer_aliases(workspace_id,alias,computer_id) VALUES(?,?,?)', workspace, alias, computer, result='none')

    def route_capacity(self,workspace,additional=1):
        count=yield from query('SELECT count(*) AS n FROM computer_routes r JOIN nodes n ON n.id=r.gateway_id WHERE n.workspace_id=?',workspace)
        if count['n']+additional>MAX_ROUTES:raise RouteError(429,'workspace route registry is full')

    def enroll(self, node):
        yield from self.route_capacity(node['workspace_id'])
        computer = yield from self.create(node['workspace_id'], node['name'], node['id'])
        yield from self.alias(node['workspace_id'], node['id'], computer)
        yield from query('UPDATE nodes SET computer_id=? WHERE id=?', computer, node['id'], result='none')
        yield from query('INSERT INTO computer_routes(gateway_id,route_id,computer_id,local,active,online,guarded,name) VALUES(?,?,?,1,1,1,0,?)', node['id'], node['id'], computer, node['name'], result='none')

    def native(self, workspace, machine, name):
        row = yield from query('SELECT c.* FROM native_computers m JOIN computers c ON c.id=m.computer_id WHERE m.workspace_id=? AND m.machine_id=?', workspace, machine)
        if row:
            if row['revoked']:
                return None
            return row['id']
        computer = yield from self.create(workspace, name)
        yield from query('UPDATE computers SET machine_id=? WHERE id=?', machine, computer, result='none')
        yield from query('INSERT INTO native_computers(workspace_id,machine_id,computer_id) VALUES(?,?,?)', workspace, machine, computer, result='none')
        return computer

    def invalidate(self, predicate, args, error='gateway route withdrawn before delivery'):
        rows = yield from query('SELECT id,workspace_id,state,result_bytes,reserved_bytes FROM requests WHERE ('+predicate+") AND state IN ('queued','claimed')", *args, result='all')
        now = time.time()
        for row in rows:
            state = 'failed' if row['state'] == 'queued' else 'uncertain'
            message = error if state == 'failed' else 'gateway route withdrawn after claim'
            size = len(message.encode())
            yield from query('UPDATE requests SET state=?,error=?,result_bytes=?,reserved_bytes=0,updated=? WHERE id=?', state, message, size, now, row['id'], result='none')
        return len(rows)

    def gateway(self, credential, machine, peers, guarded):
        node = yield from query('SELECT * FROM nodes WHERE id=? AND workspace_id=? AND revoked=0', credential['id'], credential['workspace_id'])
        if node is None:
            raise RouteError(401, 'credential revoked')
        if machine is not None and node['machine_id'] not in (None, machine):
            raise RouteError(409, 'gateway machine identity changed')
        machine = machine or node['machine_id']
        computer = node['computer_id']
        if machine:
            own = yield from query('SELECT * FROM computers WHERE id=?', computer)
            mapped = yield from query('SELECT c.* FROM native_computers m JOIN computers c ON c.id=m.computer_id WHERE m.workspace_id=? AND m.machine_id=?', node['workspace_id'], machine)
            root_revoked=bool(own['revoked'] or (mapped and mapped['revoked']))
            if mapped and mapped['id'] != computer and not own['exposed']:
                from . import notification_highwater
                yield from notification_highwater.merge(node['workspace_id'], computer, mapped['id'])
                computer = mapped['id']
                yield from self.alias(node['workspace_id'], node['id'], computer)
            elif not mapped:
                yield from query('INSERT INTO native_computers(workspace_id,machine_id,computer_id) VALUES(?,?,?)', node['workspace_id'], machine, computer, result='none')
            yield from query('UPDATE computers SET machine_id=?,revoked=? WHERE id=?', machine,int(root_revoked),computer,result='none')
        else:
            own=yield from query('SELECT revoked FROM computers WHERE id=?',computer)
            root_revoked=bool(own['revoked'])
        yield from query('UPDATE nodes SET computer_id=?,machine_id=? WHERE id=?', computer, machine, node['id'], result='none')
        yield from query('UPDATE computer_routes SET computer_id=?,machine_id=?,guarded=?,active=?,online=? WHERE gateway_id=? AND local=1', computer, machine, int(guarded and not root_revoked),int(not root_revoked),int(not root_revoked), node['id'], result='none')
        if not guarded:
            yield from self.invalidate('node_id=? AND target_machine_id IS NOT NULL', (node['id'],))
        existing = yield from query('SELECT route_id,machine_id,online FROM computer_routes WHERE gateway_id=? AND local=0', node['id'], result='all')
        by_id = {p['route_id']:p for p in peers}
        for old in existing:
            new = by_id.get(old['route_id'])
            if new is None or not new['online'] or new['machine_id'] != old['machine_id'] or not guarded:
                yield from self.invalidate('node_id=? AND route_id=?', (node['id'],old['route_id']))
            if new is None or new['machine_id'] != old['machine_id']:
                yield from query('DELETE FROM computer_routes WHERE gateway_id=? AND route_id=?', node['id'], old['route_id'], result='none')
        retained=yield from query('SELECT route_id FROM computer_routes WHERE gateway_id=? AND local=0',node['id'],result='all')
        retained_ids={r['route_id'] for r in retained}
        authorized=[]
        for peer in peers:
            if peer['machine_id'] == machine or peer['route_id'] == node['id']:
                raise RouteError(400, 'peer route cannot target its gateway')
            target = yield from self.native(node['workspace_id'], peer['machine_id'], peer['name'])
            if target is None:
                # A configured edge may outlive an operator's physical revoke.
                # Suppress only that edge; unrelated heartbeat work remains valid.
                yield from self.invalidate('node_id=? AND route_id=?',(node['id'],peer['route_id']),'computer revoked before delivery')
                yield from query('DELETE FROM computer_routes WHERE gateway_id=? AND route_id=? AND local=0',node['id'],peer['route_id'],result='none')
                continue
            authorized.append((peer,target))
        yield from self.route_capacity(node['workspace_id'],sum(peer['route_id'] not in retained_ids for peer,target in authorized))
        for peer,target in authorized:
            yield from query('INSERT INTO computer_routes(gateway_id,route_id,computer_id,machine_id,local,active,online,guarded,name) VALUES(?,?,?,?,0,1,?,?,?) ON CONFLICT(gateway_id,route_id) DO UPDATE SET online=excluded.online,guarded=excluded.guarded,name=excluded.name', node['id'], peer['route_id'], target, peer['machine_id'], int(peer['online']), int(guarded), peer['name'], result='none')
            if not peer['online']:
                # Keep last_seen for display; clearing the snapshot withdraws freshness evidence.
                yield from query('UPDATE computer_routes SET snapshot=NULL,snapshot_hash=NULL WHERE gateway_id=? AND route_id=?', node['id'], peer['route_id'], result='none')
        return computer

    @staticmethod
    def available(row, now):
        if row['gateway_revoked'] or row['revoked'] or not row['active']:
            return False
        if row['local'] and row['machine_id'] is None:
            return True
        return bool(row['online'] and row['guarded'] and row['gateway_seen'] is not None and row['last_seen'] is not None and now-row['gateway_seen']<45 and now-row['last_seen']<45)

    def candidates(self, workspace, computer=None, payload=True):
        # last_seen is freshness evidence: a peer route's time counts only while it
        # stores a snapshot accepted since the gateway last marked it online.
        # last_seen_at is the display time, which an offline peer route retains.
        where = 'c.workspace_id=? AND c.revoked=0 AND n.revoked=0 AND r.active=1'
        args = (workspace,)
        if computer is not None:
            where += ' AND c.id=?';args += (computer,)
        snapshot = "CASE WHEN r.local=1 THEN n.snapshot ELSE r.snapshot END"
        fields = (snapshot+' AS snapshot') if payload else ('COALESCE(BYTES('+snapshot+' AS_BYTES),0) AS snapshot_bytes')
        rows = yield from query('SELECT c.id,c.name,c.exposed,c.revoked,r.gateway_id,r.route_id,r.machine_id,r.local,r.active,r.online,r.guarded,CASE WHEN r.local=1 THEN n.last_seen WHEN r.snapshot_hash IS NOT NULL THEN r.last_seen END AS last_seen,CASE WHEN r.local=1 THEN n.last_seen ELSE r.last_seen END AS last_seen_at,n.last_seen AS gateway_seen,n.revoked AS gateway_revoked,n.name AS gateway_name,'+fields+' FROM computers c JOIN computer_routes r ON r.computer_id=c.id JOIN nodes n ON n.id=r.gateway_id WHERE '+where, *args, result='all')
        return rows

    def resolve(self, workspace, alias):
        row = yield from query('SELECT computer_id FROM computer_aliases WHERE workspace_id=? AND alias=?', workspace, alias)
        if row:return row['computer_id']
        row = yield from query('SELECT id FROM computers WHERE workspace_id=? AND id=?', workspace, alias)
        return row['id'] if row else None

    def select(self, workspace, alias):
        computer = yield from self.resolve(workspace, alias)
        if computer is None:return None
        candidates = yield from self.candidates(workspace, computer, payload=False)
        rows = [r for r in candidates if self.available(r,time.time())]
        if not rows:return None
        row=min(rows,key=lambda r:(not r['local'],r['gateway_id'],r['route_id']))
        fetched=yield from query('SELECT CASE WHEN r.local=1 THEN n.snapshot ELSE r.snapshot END AS snapshot FROM computer_routes r JOIN nodes n ON n.id=r.gateway_id WHERE r.gateway_id=? AND r.route_id=?',row['gateway_id'],row['route_id'])
        from . import notification_highwater
        raw = fetched['snapshot']
        if raw is not None:
            projected = yield from notification_highwater.overlay(computer, json.loads(raw))
            raw = notification_highwater.canonical(projected)
        row['snapshot']=raw
        return row

    def frozen(self, request, allow_gateway):
        if request['target_machine_id'] is not None and not allow_gateway:return False
        if request['target_computer_id'] is None:return True
        rows = yield from self.candidates(request['workspace_id'],request['target_computer_id'],payload=False)
        for route in rows:
            if route['gateway_id']==request['node_id'] and (None if route['local'] else route['route_id'])==request['route_id'] and route['machine_id']==request['target_machine_id']:
                return self.available(route,time.time())
        return False

    def catalog(self, workspace, maximum):
        rows = yield from self.candidates(workspace,payload=False)
        chosen = {}
        now=time.time()
        for route in rows:
            key=(not self.available(route,now),not route['local'],route['gateway_id'],route['route_id'])
            if route['id'] not in chosen or key<chosen[route['id']][0]:chosen[route['id']]=(key,route)
        alias_size=yield from query('SELECT COALESCE(sum(BYTES(alias AS_BYTES)+4),0) AS n FROM computer_aliases WHERE workspace_id=?',workspace)
        if alias_size['n']+sum(row['snapshot_bytes']+len(row['name'].encode())+512 for _,row in chosen.values())>maximum:return None
        if not chosen:return []
        selected=[row for _,row in chosen.values()]
        predicate=','.join('(?,?)' for row in selected)
        args=tuple(value for row in selected for value in (row['gateway_id'],row['route_id']))
        snapshots=yield from query('WITH selected(gateway_id,route_id) AS (VALUES '+predicate+') SELECT r.gateway_id,r.route_id,CASE WHEN r.local=1 THEN n.snapshot ELSE r.snapshot END AS snapshot FROM computer_routes r JOIN nodes n ON n.id=r.gateway_id JOIN selected s ON s.gateway_id=r.gateway_id AND s.route_id=r.route_id',*args,result='all')
        by_route={(r['gateway_id'],r['route_id']):r['snapshot'] for r in snapshots}
        aliases=yield from query('SELECT alias,computer_id FROM computer_aliases WHERE workspace_id=? ORDER BY alias',workspace,result='all')
        by_computer={}
        for alias in aliases:by_computer.setdefault(alias['computer_id'],[]).append(alias['alias'])
        result=[]
        projected_bytes=2
        for row in sorted(selected,key=lambda row:(row['name'],row['id'])):
            from . import notification_highwater
            raw = by_route[(row['gateway_id'],row['route_id'])]
            if raw is not None:
                projected = yield from notification_highwater.overlay(row['id'],json.loads(raw))
                raw = notification_highwater.canonical(projected)
            row.update(snapshot=raw,aliases=by_computer.get(row['id'],[]),via=None if row['local'] else {'gateway_id':row['gateway_id'],'gateway_name':row['gateway_name']},available=self.available(row,now))
            projected_bytes += len(notification_highwater.canonical(row).encode()) + 1
            if projected_bytes>maximum:raise RouteError(413, 'projected catalog is too large')
            result.append(row)
        unexposed=[row['id'] for row in selected if not row['exposed']]
        if unexposed:
            yield from query('UPDATE computers SET exposed=1 WHERE id IN ('+','.join('?' for _ in unexposed)+')',*unexposed,result='none')
        if len(json.dumps(result,sort_keys=True,separators=(',',':'),allow_nan=False).encode())>maximum:
            raise RouteError(413, 'projected catalog is too large')
        return result

    def peer_heartbeat(self, node, route_id, machine_id, encoded):
        row = yield from query('SELECT r.*,c.revoked,n.revoked AS gateway_revoked,n.workspace_id FROM computer_routes r JOIN nodes n ON n.id=r.gateway_id JOIN computers c ON c.id=r.computer_id WHERE r.gateway_id=? AND r.route_id=? AND r.local=0',node['id'],route_id)
        if row is None or row['workspace_id'] != node['workspace_id']:
            raise RouteError(404, 'peer route not found')
        if row['gateway_revoked'] or row['revoked'] or not row['active'] or not row['online'] or not row['guarded']:
            raise RouteError(409, 'peer route is unavailable')
        if row['machine_id'] != machine_id:
            raise RouteError(409, 'peer machine identity changed')
        snapshot_hash=hashlib.sha256(encoded.encode()).hexdigest()
        yield from query('UPDATE computer_routes SET snapshot=?,snapshot_hash=?,last_seen=? WHERE gateway_id=? AND route_id=?',encoded,snapshot_hash,time.time(),node['id'],route_id,result='none')
        return {'id':row['computer_id'],'previous':row['snapshot'],'changed':snapshot_hash!=row['snapshot_hash']}

    def revoke_computer(self, value):
        computer=yield from query('SELECT c.* FROM computer_aliases a JOIN computers c ON c.id=a.computer_id WHERE a.alias=? LIMIT 1',value)
        if computer is None:computer=yield from query('SELECT * FROM computers WHERE id=?',value)
        if computer is None:return False
        if computer['machine_id'] is not None:
            rows=yield from query('SELECT id FROM computers WHERE workspace_id=? AND machine_id=?',computer['workspace_id'],computer['machine_id'],result='all')
        else:rows=[computer]
        for row in rows:
            yield from self.invalidate('target_computer_id=? OR (target_computer_id IS NULL AND node_id IN (SELECT gateway_id FROM computer_routes WHERE computer_id=? AND local=1))',(row['id'],row['id']),'computer revoked before delivery')
            yield from query('UPDATE nodes SET manifest_hash=NULL WHERE id IN (SELECT gateway_id FROM computer_routes WHERE computer_id=?)',row['id'],result='none')
            yield from query('UPDATE computers SET revoked=1 WHERE id=?',row['id'],result='none')
            yield from query('UPDATE computer_routes SET active=0,snapshot=NULL,snapshot_hash=NULL,last_seen=NULL WHERE computer_id=?',row['id'],result='none')
        return True

SCHEMA = '''
CREATE TABLE IF NOT EXISTS computers(id TEXT PRIMARY KEY,workspace_id TEXT NOT NULL REFERENCES workspaces(id),name TEXT NOT NULL,machine_id TEXT,revoked INTEGER NOT NULL DEFAULT 0,exposed INTEGER NOT NULL DEFAULT 0);
CREATE INDEX IF NOT EXISTS computer_workspace ON computers(workspace_id,id);
CREATE TABLE IF NOT EXISTS computer_notification_preferences(computer_id TEXT PRIMARY KEY REFERENCES computers(id),workspace_id TEXT NOT NULL REFERENCES workspaces(id),preferences TEXT NOT NULL,payload_bytes INTEGER NOT NULL);
CREATE INDEX IF NOT EXISTS computer_notification_workspace ON computer_notification_preferences(workspace_id,computer_id);
CREATE TABLE IF NOT EXISTS native_computers(workspace_id TEXT NOT NULL REFERENCES workspaces(id),machine_id TEXT NOT NULL,computer_id TEXT NOT NULL REFERENCES computers(id),PRIMARY KEY(workspace_id,machine_id));
CREATE TABLE IF NOT EXISTS computer_aliases(workspace_id TEXT NOT NULL REFERENCES workspaces(id),alias TEXT NOT NULL,computer_id TEXT NOT NULL REFERENCES computers(id),PRIMARY KEY(workspace_id,alias));
CREATE INDEX IF NOT EXISTS computer_alias_target ON computer_aliases(computer_id);
CREATE TABLE IF NOT EXISTS computer_routes(gateway_id TEXT NOT NULL REFERENCES nodes(id),route_id TEXT NOT NULL,computer_id TEXT NOT NULL REFERENCES computers(id),machine_id TEXT,local INTEGER NOT NULL DEFAULT 0,active INTEGER NOT NULL DEFAULT 1,online INTEGER NOT NULL DEFAULT 0,guarded INTEGER NOT NULL DEFAULT 0,name TEXT NOT NULL,snapshot TEXT,snapshot_hash TEXT,last_seen DOUBLE PRECISION,PRIMARY KEY(gateway_id,route_id));
CREATE INDEX IF NOT EXISTS computer_route_target ON computer_routes(computer_id,gateway_id,route_id);
INSERT INTO computers(id,workspace_id,name,revoked,exposed) SELECT id,workspace_id,name,revoked,1 FROM nodes WHERE computer_id IS NULL ON CONFLICT(id) DO NOTHING;
INSERT INTO computer_aliases(workspace_id,alias,computer_id) SELECT workspace_id,id,id FROM nodes WHERE computer_id IS NULL ON CONFLICT(workspace_id,alias) DO NOTHING;
INSERT INTO computer_routes(gateway_id,route_id,computer_id,local,active,online,name) SELECT id,id,id,1,1,1,name FROM nodes WHERE computer_id IS NULL ON CONFLICT(gateway_id,route_id) DO NOTHING;
UPDATE nodes SET computer_id=id WHERE computer_id IS NULL;
'''
