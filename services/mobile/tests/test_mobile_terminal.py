"""Exact Terminal dispatch, short expiry and sustained read retention."""
import tempfile
import json
import os
from pathlib import Path
import time
import unittest
from unittest.mock import AsyncMock, patch
import uuid

from zerus_mobile.connector import Connector, Journal
from relay_oracle import validate_request
from zerus_mobile.server import AUTHENTICATED_REQUEST_LIMIT, GLOBAL_REQUEST_LIMIT, IP_REQUEST_LIMIT, RateLimits
from zerus_mobile.store import Store


class TerminalTransport(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.connector = Connector({'server_url':'https://relay.example.invalid','node_token':'fixture','state_dir':self.temp.name})
        self.connector.sessions={'codex/example'}
        self.connector.terminal_supported=True

    async def asyncTearDown(self):
        self.connector.journal.close(); self.temp.cleanup()

    def request(self, operation='terminal_input', **fields):
        identity=str(uuid.uuid4())
        payload={'request_id':identity,'expected_run_id':'run','expected_conversation_id':'conversation'}
        if operation=='terminal_input': payload.update(terminal_binding_id='a'*64,text='echo synthetic',enter=True)
        payload.update(fields)
        result={'request_id':identity,'computer_id':str(uuid.uuid4()),'operation':operation,'session':'codex/example','payload':payload}
        if operation=='terminal_input': result['expires_at']=time.time()+5
        return result

    def receipt(self, request, **fields):
        return dict(request_id=request['request_id'],name=request['session'],run_id='run',conversation_id='conversation',status='submitted',terminal_binding_id='a'*64,**fields)

    async def test_fixed_raw_dispatch_and_duplicate_never_repeat(self):
        request=self.request()
        snapshot=self.receipt(request,input_supported=True,multiline_supported=False);snapshot['status']='snapshot'
        with patch.object(self.connector,'native',AsyncMock(side_effect=[snapshot,self.receipt(request)])) as native:
            result=await self.connector.execute(request)
            self.assertEqual(result['state'],'completed')
            self.assertEqual(await self.connector.execute(request),result)
            self.assertEqual(native.await_count,2)
            self.assertEqual(native.call_args.args,(['terminal','codex/example','--json'],{**request['payload'],'action':'input','expires_at':request['expires_at']}))
            self.assertEqual(native.call_args.kwargs,{'timeout':3})

    async def test_expired_wrong_binding_and_old_cli_fail_before_handoff(self):
        request=self.request();request['expires_at']=time.time()-1
        with patch.object(self.connector,'native',AsyncMock()) as native:
            self.assertEqual((await self.connector.execute(request))['state'],'failed');native.assert_not_awaited()
        request=self.request()
        snapshot=self.receipt(request,input_supported=True);snapshot.update(status='snapshot',terminal_binding_id='b'*64)
        with patch.object(self.connector,'native',AsyncMock(return_value=snapshot)) as native:
            self.assertEqual((await self.connector.execute(request))['state'],'failed');self.assertEqual(native.await_count,1)
        self.connector.terminal_supported=False
        with patch.object(self.connector,'native',AsyncMock()) as native:
            self.assertEqual((await self.connector.execute(self.request()))['state'],'failed');native.assert_not_awaited()

    async def test_ambiguous_native_ack_never_replays(self):
        request=self.request()
        snapshot=self.receipt(request,input_supported=True);snapshot['status']='snapshot'
        bad=self.receipt(request);bad['terminal_binding_id']='b'*64
        with patch.object(self.connector,'native',AsyncMock(side_effect=[snapshot,bad])) as native:
            result=await self.connector.execute(request);self.assertEqual(result['state'],'uncertain')
            self.assertEqual(await self.connector.execute(request),result);self.assertEqual(native.await_count,2)

    async def test_payload_is_literal_bounded_and_rejects_hidden_control(self):
        request=self.request(text='"\\\t'*21000)
        validate_request({key:value for key,value in request.items() if key!='expires_at'})
        for fields in ({'text':'\x1bsecret'},{'key':'C-z'},{'archive_id':str(uuid.uuid4())},{'action':'input'},{'expires_at':time.time()+5}):
            request=self.request(**fields)
            with self.assertRaises(Exception): validate_request({key:value for key,value in request.items() if key!='expires_at'})


class TerminalFixture(unittest.IsolatedAsyncioTestCase):
    async def test_real_fixture_subprocess_roundtrip_does_not_execute_text(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(__file__).with_name("fixture_hgs.py").resolve()
            with patch.dict(os.environ, {"ZERUS_MOBILE_FIXTURE_DIR": directory}):
                connector = Connector({"server_url": "https://relay.example.invalid", "node_token": "fixture", "state_dir": str(Path(directory)/"connector"), "hgs_path": str(fixture)})
                try:
                    snapshot = await connector.snapshot()
                    self.assertIn("terminal_input", snapshot["mobile_capabilities"]["operations"])
                    row = snapshot["sessions"][0]
                    def request(operation, **fields):
                        identity = str(uuid.uuid4())
                        return {"request_id": identity, "operation": operation, "session": row["name"],
                                "payload": {"request_id": identity, "expected_run_id": row["run_id"], "expected_conversation_id": row["conversation_id"], **fields}}
                    screen = await connector.execute(request("terminal_snapshot"))
                    self.assertEqual(screen["state"], "completed")
                    command = request("terminal_input", terminal_binding_id=screen["result"]["terminal_binding_id"], text="synthetic ; $(never executed)", enter=True)
                    command["expires_at"] = time.time()+5
                    answer = await connector.execute(command)
                    self.assertEqual(answer["state"], "completed")
                    self.assertEqual(await connector.execute(command), answer)
                    mutations = [json.loads(line) for line in (Path(directory)/"mutations.jsonl").read_text().splitlines()]
                    self.assertEqual([row["operation"] for row in mutations], ["terminal-input"])
                finally: connector.journal.close()


class TerminalRateBudget(unittest.TestCase):
    def test_four_frames_per_second_receipts_and_node_uplink_for_two_minutes(self):
        limits = RateLimits()
        for frame in range(480):
            with patch("zerus_mobile.server.time.monotonic", return_value=frame/4):
                # Phone submission + three fast receipt reads; node delivery
                # and result upload share the same administrator's IP budget.
                for role in ("phone", "phone", "phone", "phone", "node", "node"):
                    limits.check(("global",), limit=GLOBAL_REQUEST_LIMIT)
                    limits.check(("ip", "loopback"), limit=IP_REQUEST_LIMIT)
                    limits.check((role, "fixture"), limit=AUTHENTICATED_REQUEST_LIMIT)
        with patch("zerus_mobile.server.time.monotonic", return_value=120):
            for _ in range(10): limits.check(("pair", "loopback"), limit=10)
            with self.assertRaises(Exception): limits.check(("pair", "loopback"), limit=10)


class TerminalJournalRetention(unittest.TestCase):
    """The connector (Journal) half of the sustained Terminal retention case."""
    def test_sustained_screens_leave_the_journal_and_inputs_stay(self):
        with tempfile.TemporaryDirectory() as directory:
            journal=Journal(Path(directory)/'connector', 'fixture-binding')
            try:
                for index in range(5100):
                    now=10000+index*.25
                    with patch('time.time',return_value=now):
                        identity=str(uuid.uuid4())
                        self.assertTrue(journal.claim(identity,'terminal_snapshot'))
                        journal.finish(identity,{'state':'completed','result':{},'error':None});journal.delivered(identity)
                self.assertLess(journal.db.execute('SELECT count(*) FROM requests').fetchone()[0],500)
                with patch('time.time',return_value=now+121): journal.prune()
                self.assertEqual(journal.db.execute('SELECT count(*) FROM requests').fetchone()[0],0)
                with patch('time.time',return_value=now+200):
                    identity=str(uuid.uuid4())
                    journal.claim(identity,'terminal_input');journal.finish(identity,{'state':'uncertain','result':None,'error':'fixture'});journal.delivered(identity)
                with patch('time.time',return_value=now+1000): journal.prune()
                self.assertFalse(journal.claim(identity,'terminal_input'))
            finally: journal.close()


class TerminalRetention(unittest.TestCase):
    """The relay half; its Rust contract is listed in relay/tests/ORACLE.md."""
    def test_sustained_reads_and_abandoned_results_expire_without_mutation_eviction(self):
        with tempfile.TemporaryDirectory() as directory:
            store=Store(Path(directory)/'relay.sqlite3')
            workspace=store.workspace('Example'); credentials=store.node(workspace,'Computer')
            phone=store.pair(store.invite(workspace)['pair_code'],'Phone')
            node=store.authenticate(credentials['node_token'],'nodes');device=store.authenticate(phone['device_token'],'devices')
            try:
                for index in range(5100):
                    now=10000+index*.25
                    with patch('time.time',return_value=now):
                        store.maintain()
                        store.heartbeat(node,{'sessions':[],'mobile_capabilities':{'protocol_version':1,'operations':['terminal_snapshot','terminal_input']}})
                        identity=str(uuid.uuid4())
                        body={'request_id':identity,'computer_id':node['id'],'operation':'terminal_snapshot','session':'codex/example','payload':{'request_id':identity,'expected_run_id':'run','expected_conversation_id':'conversation'}}
                        status,_=store.submit(device,body,200);self.assertEqual(status,202,index)
                        self.assertEqual(len(store.claim(node)),1)
                        self.assertEqual(store.result(node,identity,{'state':'completed','result':{'screen':'synthetic'},'error':None}),200)
                self.assertLess(store.db.execute('SELECT count(*) FROM requests').fetchone()[0],500)
                with patch('time.time',return_value=now+121): store.maintain();store.maintain()
                self.assertEqual(store.db.execute('SELECT count(*) FROM requests').fetchone()[0],0)
                with patch('time.time',return_value=now+200):
                    store.heartbeat(node,{'sessions':[],'mobile_capabilities':{'protocol_version':1,'operations':['terminal_input']}})
                    body['request_id']=str(uuid.uuid4());body['operation']='terminal_input';body['payload'].update(request_id=body['request_id'],terminal_binding_id='a'*64,text='literal',enter=True)
                    self.assertEqual(store.submit(device,body,200)[0],202)
                    claim=store.claim(node)[0];self.assertEqual(claim['expires_at'],now+205)
                    store.result(node,body['request_id'],{'state':'uncertain','result':None,'error':'fixture'})
                with patch('time.time',return_value=now+1000):store.maintain();store.maintain()
                self.assertEqual(store.db.execute('SELECT operation FROM requests').fetchone()[0],'terminal_input')
            finally: store.db.close()
