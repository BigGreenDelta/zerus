"""Private SQLite route-failover contracts; no native/network/device actions."""
import copy
import json
from pathlib import Path
import tempfile
import unittest
import uuid

from zerus_mobile import notification_highwater as highwater
from zerus_mobile.routes import RouteError, drive_sync
from zerus_mobile.store import Store


def session(revision=0, muted=False, conversation='conversation', provider='codex'):
    return {'name': provider+'/example/session', 'run_id': 'run', 'conversation_id': conversation,
            'phase': 'input', 'attention_id': 'question', 'reply_id': 'reply',
            'notification_preferences': {'conversation_id': conversation, 'muted': muted,
                'revision': revision, 'suppressed': {'reply_id': None, 'attention_signature': None,
                    'optional_question_ids': [], 'main_attention': None, 'child_attention': {}}}}


class ComputerNotificationHighwater(unittest.TestCase):
    def test_projection_growth_then_shrink_fits_independently_of_row_order(self):
        grow = session(1, True, conversation='grow')
        grow['notification_preferences']['suppressed']['child_attention'] = {
            f'child-{i}': {'phase': 'input', 'request_id': 'x' * 65536, 'started_at': None} for i in range(5)}
        shrink = session(1, False, conversation='shrink')
        self.peer(0, grow)
        self.peer(0, shrink)
        old_grow = session(conversation='grow')
        old_shrink = copy.deepcopy(grow)
        old_shrink['conversation_id'] = 'shrink'
        old_shrink['notification_preferences']['conversation_id'] = 'shrink'
        old_shrink['notification_preferences']['revision'] = 0
        for rows in ([old_grow, old_shrink], [old_shrink, old_grow]):
            value = {'sessions': copy.deepcopy(rows), 'padding': 'p' * 500000}
            projected = drive_sync(self.store.db, highwater.overlay(self.computer, value))
            self.assertLessEqual(len(highwater.canonical(projected).encode()), highwater.MAX_BYTES)
            by_id = {r['conversation_id']: r['notification_preferences'] for r in projected['sessions']}
            self.assertEqual(by_id, {'grow': grow['notification_preferences'], 'shrink': shrink['notification_preferences']})

    def test_repeated_scope_projection_refuses_before_any_row_replacement(self):
        large = session(1, True)
        large['notification_preferences']['suppressed']['child_attention'] = {
            f'child-{i}': {'phase': 'input', 'request_id': 'x' * 65536, 'started_at': None} for i in range(5)}
        self.peer(0, large)
        for absent in (False, True):
            original = session()
            if absent:
                del original['notification_preferences']
            snapshot = {'sessions': [copy.deepcopy(original) for _ in range(10)]}
            before = copy.deepcopy(snapshot)
            with self.subTest(absent=absent), self.assertRaises(RouteError) as error:
                drive_sync(self.store.db, highwater.overlay(self.computer, snapshot))
            self.assertEqual(error.exception.status, 413)
            self.assertEqual(snapshot, before)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='mute-highwater-')
        self.path = Path(self.temp.name)/'relay.sqlite3'
        self.store = Store(self.path)
        self.workspace = self.store.workspace('Synthetic workspace')
        self.nodes = []
        for name in ('Gateway A', 'Gateway B'):
            issued = self.store.node(self.workspace, name)
            self.nodes.append(dict(self.store.authenticate(issued['node_token'], 'nodes')))
        self.machines = [str(uuid.uuid4()), str(uuid.uuid4())]
        self.physical = str(uuid.uuid4())
        self.routes = [str(uuid.uuid4()), str(uuid.uuid4())]
        self.base = {'sessions': [], 'mobile_capabilities': {'protocol_version': 1, 'features': ['gateway_one_hop']}}
        for n in (0, 1):
            self.advertise(n)
            self.peer(n, session())
        self.computer = self.store.db.execute('SELECT computer_id FROM native_computers WHERE workspace_id=? AND machine_id=?',
                                              (self.workspace, self.physical)).fetchone()[0]

    def tearDown(self):
        self.store.close()
        self.temp.cleanup()

    def advertise(self, n, online=True, removed=False):
        peers = [] if removed else [{'route_id': self.routes[n], 'machine_id': self.physical,
                                    'name': 'Synthetic peer', 'online': online}]
        self.store.heartbeat(self.nodes[n], self.base, self.machines[n], peers)

    def peer(self, n, row):
        return self.store.peer_heartbeat(self.nodes[n], self.routes[n], self.physical, {'sessions': [row]})

    def current(self):
        rows = self.store.computers(self.workspace)
        return json.loads(next(r for r in rows if r['id']==self.computer)['snapshot'])['sessions'][0]

    def test_offline_removed_route_cold_catalog_and_select_keep_highwater_after_restart(self):
        self.peer(0, session(1, True))
        self.advertise(0, online=False)
        self.assertTrue(self.current()['notification_preferences']['muted'])
        raw = self.store.db.execute('SELECT snapshot FROM computer_routes WHERE gateway_id=? AND route_id=?',
                                    (self.nodes[1]['id'], self.routes[1])).fetchone()[0]
        self.assertFalse(json.loads(raw)['sessions'][0]['notification_preferences']['muted'])
        with self.store.db:
            selected = drive_sync(self.store.db, self.store.registry.select(self.workspace, self.computer))
        self.assertEqual(selected['gateway_id'], self.nodes[1]['id'])
        self.assertTrue(json.loads(selected['snapshot'])['sessions'][0]['notification_preferences']['muted'])
        self.advertise(0, removed=True)
        self.store.close(); self.store=Store(self.path)
        self.assertEqual(self.current()['notification_preferences']['revision'], 1)
        stored = self.store.db.execute('SELECT sum(payload_bytes) FROM computer_notification_preferences WHERE workspace_id=?', (self.workspace,)).fetchone()[0]
        self.assertEqual(self.store.db.execute('SELECT payload_bytes FROM workspace_usage WHERE workspace_id=?', (self.workspace,)).fetchone()[0], stored)

    def test_stale_or_conflicting_other_route_is_atomic_and_never_changes_native_fields(self):
        self.peer(0, session(1, True))
        before = self.store.db.execute('SELECT snapshot,last_seen FROM computer_routes WHERE gateway_id=? AND route_id=?', (self.nodes[1]['id'], self.routes[1])).fetchone()
        for row in (session(), session(1, False)):
            with self.assertRaises(RouteError):
                self.peer(1, row)
            after = self.store.db.execute('SELECT snapshot,last_seen FROM computer_routes WHERE gateway_id=? AND route_id=?', (self.nodes[1]['id'], self.routes[1])).fetchone()
            self.assertEqual(tuple(after), tuple(before))
        self.advertise(0, online=False)
        current=self.current()
        self.assertEqual(current['run_id'], 'run'); self.assertEqual(current['attention_id'], 'question')
        self.assertEqual(current['reply_id'], 'reply'); self.assertNotIn('read', current)

    def test_rename_resume_provider_new_conversation_and_missing_rows_are_separate(self):
        self.peer(0, session(1, True))
        renamed=session(1, True); renamed.update(name='codex/other/renamed',run_id='resumed')
        self.peer(1, renamed)
        self.advertise(0, removed=True)
        self.assertTrue(self.current()['notification_preferences']['muted'])
        self.peer(1, session(0, False, provider='kimi'))
        self.assertFalse(self.current()['notification_preferences']['muted'])
        self.peer(1, session(0, False, conversation='new'))
        self.assertFalse(self.current()['notification_preferences']['muted'])
        self.store.peer_heartbeat(self.nodes[1], self.routes[1], self.physical, {'sessions': []})
        self.peer(1, renamed)
        self.assertTrue(self.current()['notification_preferences']['muted'])

    def test_quota_failure_rolls_back_route_highwater_and_counters(self):
        before = tuple(self.store.db.execute('SELECT preferences,payload_bytes FROM computer_notification_preferences WHERE computer_id=?', (self.computer,)).fetchone())
        with self.store.db:
            self.store.db.execute('UPDATE workspace_usage SET budget_bytes=payload_bytes WHERE workspace_id=?', (self.workspace,))
        larger=session(1, True); larger['notification_preferences']['suppressed']['optional_question_ids']=['another-question']
        with self.assertRaises(RouteError):
            self.peer(0, larger)
        self.assertEqual(tuple(self.store.db.execute('SELECT preferences,payload_bytes FROM computer_notification_preferences WHERE computer_id=?', (self.computer,)).fetchone()), before)
        raw=json.loads(self.store.db.execute('SELECT snapshot FROM computer_routes WHERE gateway_id=? AND route_id=?',(self.nodes[0]['id'],self.routes[0])).fetchone()[0])
        self.assertEqual(raw['sessions'][0]['notification_preferences']['revision'],0)
        self.assertEqual(self.store.db.execute('SELECT payload_bytes FROM workspace_usage WHERE workspace_id=?',(self.workspace,)).fetchone()[0],before[1])

    def test_overlay_enforces_projected_size_before_returning_catalog(self):
        row=session(1, True)
        row['notification_preferences']['suppressed']['optional_question_ids']=sorted('q'*250+str(i) for i in range(64))
        self.peer(0,row)
        self.advertise(0,online=False)
        with self.assertRaises(RouteError):
            self.store.computers(self.workspace,max_bytes=12000)

    def test_unexposed_identity_merge_preserves_both_ledgers_and_exact_accounting(self):
        target=self.computer
        issued=self.store.node(self.workspace,'Unexposed direct enrollment')
        credential=dict(self.store.authenticate(issued['node_token'],'nodes'));source=credential['id']
        snapshot={'sessions':[session(3,True,conversation='other')]}
        self.store.heartbeat(credential,snapshot)
        self.assertEqual(self.store.db.execute('SELECT exposed FROM computers WHERE id=?',(source,)).fetchone()[0],0)
        # Learning the native machine UUID performs the actual registry merge.
        self.store.heartbeat(credential,snapshot,machine_id=self.physical)
        self.assertEqual(self.store.db.execute('SELECT computer_id FROM nodes WHERE id=?',(source,)).fetchone()[0],target)
        self.assertIsNone(self.store.db.execute('SELECT 1 FROM computer_notification_preferences WHERE computer_id=?',(source,)).fetchone())
        projected=drive_sync(self.store.db,highwater.overlay(target,{'sessions':[session(0,False,conversation='other'),session()]}))
        self.assertTrue(projected['sessions'][0]['notification_preferences']['muted'])
        retained=self.store.db.execute('SELECT sum(payload_bytes) FROM computer_notification_preferences WHERE workspace_id=?',(self.workspace,)).fetchone()[0]
        self.assertEqual(self.store.db.execute('SELECT payload_bytes FROM workspace_usage WHERE workspace_id=?',(self.workspace,)).fetchone()[0],retained)

    def test_retained_bound_never_evicts_or_resets_an_old_revision(self):
        # Fill by valid bounded preferences; the first over-cap transaction must
        # leave both its older scope and the complete stored map untouched.
        for n in range(3):
            row=session(0,False,conversation='extra-'+str(n))
            row['notification_preferences']['suppressed']['child_attention']={
                'child-'+str(i):{'phase':'input','request_id':'x'*1000,'started_at':None} for i in range(256)}
            with self.store.db:
                self.store.db.execute('BEGIN IMMEDIATE')
                drive_sync(self.store.db,highwater.accept(self.workspace,self.computer,{'sessions':[row]}))
        old=self.store.db.execute('SELECT preferences FROM computer_notification_preferences WHERE computer_id=?',(self.computer,)).fetchone()[0]
        row=copy.deepcopy(row);row['conversation_id']='overflow';row['notification_preferences']['conversation_id']='overflow'
        with self.assertRaises(RouteError), self.store.db:
            self.store.db.execute('BEGIN IMMEDIATE')
            drive_sync(self.store.db,highwater.accept(self.workspace,self.computer,{'sessions':[row]}))
        self.assertEqual(self.store.db.execute('SELECT preferences FROM computer_notification_preferences WHERE computer_id=?',(self.computer,)).fetchone()[0],old)
