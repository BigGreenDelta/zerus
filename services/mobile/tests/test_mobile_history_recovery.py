"""Scoped paging/controls preserve identity, read retention and no-replay."""
import json
import os
from pathlib import Path
import tempfile
import time
import unittest
import uuid
from unittest.mock import AsyncMock,patch
from zerus_mobile.connector import Connector
from relay_oracle import validate_request
from zerus_mobile.context import READ_OPERATIONS

class HistoryRecovery(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp=tempfile.TemporaryDirectory();self.c=Connector({'server_url':'https://relay.example.invalid','node_token':'fixture','state_dir':self.temp.name})
        self.c.sessions={'codex/example/mobile'};self.c.history_supported=self.c.recovery_supported=True
    async def asyncTearDown(self):self.c.journal.close();self.temp.cleanup()
    def request(self,op='history',**extras):
        identity=str(uuid.uuid4());payload={'request_id':identity,'expected_run_id':'fixture-run','expected_conversation_id':'fixture-conversation',**extras}
        return {'request_id':identity,'computer_id':str(uuid.uuid4()),'session':'codex/example/mobile','operation':op,'payload':payload}
    async def test_history_is_read_only_scoped_and_old_nodes_deny(self):
        self.assertIn('history',READ_OPERATIONS)
        self.c.history_supported=False
        with patch.object(self.c,'native',AsyncMock()) as native:
            self.assertEqual((await self.c.execute(self.request()))['state'],'failed');native.assert_not_awaited()
        for extras in ({'path':'/private'},{'limit':True},{'limit':101},{'before':'cursor','after':'cursor'},{'around_incoming_seq':1},{'agent_id':'../other'}):
            with self.assertRaises(Exception):validate_request(self.request(**extras))
    async def test_fixture_pages_over_500_and_wrong_cursor_scope_rejected(self):
        root=Path(self.temp.name);(root/'fixture-config.json').write_text(json.dumps({'history_count':800}))
        with patch.dict(os.environ,{'ZERUS_MOBILE_FIXTURE_DIR':self.temp.name}):
            self.c.hgs=str(Path(__file__).with_name('fixture_hgs.py').resolve());await self.c.snapshot()
            first=await self.c.execute(self.request());self.assertEqual(first['state'],'completed',first)
            result=first['result'];self.assertTrue(result['head']['complete']);self.assertEqual(400,result['head']['total_incoming'])
            old=await self.c.execute(self.request(before=result['next_before']));self.assertEqual(old['state'],'completed',old)
            self.assertTrue(set(row['history_id'] for row in old['result']['events']).isdisjoint(row['history_id'] for row in result['events']))
            anchor=await self.c.execute(self.request(around_incoming_seq=10,history_epoch=result['history_epoch']));self.assertEqual(11,next(row['incoming_seq'] for row in anchor['result']['events'] if row['incoming_seq']))
            invalid=await self.c.execute(self.request(before=result['next_before']+'bad'));self.assertEqual(invalid['state'],'failed')
    async def test_wrong_history_ack_and_archive_match_fail_closed(self):
        request=self.request();wrong={'request_id':request['request_id'],'name':request['session'],'run_id':'replacement','conversation_id':'fixture-conversation','events':[],'history_epoch':'epoch','head':{}}
        with patch.object(self.c,'native',AsyncMock(return_value=wrong)):
            self.assertEqual((await self.c.execute(request))['state'],'failed')
        request=self.request(archive_id=str(uuid.uuid4()))
        with patch.object(self.c,'native',AsyncMock()) as native:
            self.assertEqual((await self.c.execute(request))['state'],'failed');native.assert_not_awaited()
    async def test_recovery_five_field_identity_and_duplicate_no_repeat(self):
        request=self.request('recovery_action',job_id=str(uuid.uuid4()),action='now');payload=request['payload']
        job={'id':payload['job_id'],'state':'waiting','identity':['fixture-run','fixture-conversation','model',None,None]}
        fresh={'name':request['session'],'run_id':'fixture-run','conversation_id':'fixture-conversation','recovery':job}
        result={'request_id':request['request_id'],'name':request['session'],'run_id':'fixture-run','conversation_id':'fixture-conversation','job_id':job['id'],'action':'now','status':'scheduled','recovery':job}
        with patch.object(self.c,'native',AsyncMock(side_effect=[fresh,result])) as native:
            receipt=await self.c.execute(request);self.assertEqual(receipt['state'],'completed',receipt)
            self.assertEqual(await self.c.execute(request),receipt);self.assertEqual(native.await_count,2)
            self.assertEqual(native.call_args_list[1].args[0],['recovery','action','--scoped-json'])
        bad=self.request('recovery_action',job_id=str(uuid.uuid4()),action='cancel')
        fresh['recovery']['identity']=['fixture-run','fixture-conversation']
        with patch.object(self.c,'native',AsyncMock(return_value=fresh)) as native:
            self.assertEqual((await self.c.execute(bad))['state'],'failed');self.assertEqual(native.await_count,1)
    async def test_head_locally_advances_bounded_index_and_partial_stops(self):
        request=self.request();p=request['payload']
        base={'request_id':request['request_id'],'name':request['session'],'run_id':p['expected_run_id'],'conversation_id':p['expected_conversation_id'],'events':[],'history_epoch':'epoch','head':{'complete':False},'indexing':True}
        stages=[{**base,'source_status':{'progress':{'source_bytes':n}}} for n in range(3)]
        stages[-1].update(indexing=False,head={'complete':True,'total_incoming':0})
        with patch.object(self.c,'native',AsyncMock(side_effect=stages)) as native:
            result=await self.c.execute(request);self.assertTrue(result['result']['head']['complete']);self.assertEqual(native.await_count,3)
            self.assertTrue(all(call.args[1]==p for call in native.call_args_list))
        for progressing, expected_calls in ((True,2),(False,1)):
            request=self.request();partial={**base,'request_id':request['request_id'],'indexing':progressing,'source_status':{'provider':'partial'}}
            with patch.object(self.c,'native',AsyncMock(return_value=partial)) as native:
                self.assertEqual((await self.c.execute(request))['state'],'completed');self.assertEqual(native.await_count,expected_calls)
    async def test_history_cursor_reads_are_not_repeated_for_indexing(self):
        request=self.request(before='opaque');p=request['payload']
        result={'request_id':request['request_id'],'name':request['session'],'run_id':p['expected_run_id'],'conversation_id':p['expected_conversation_id'],'events':[],'history_epoch':'epoch','head':{'complete':False},'indexing':True}
        with patch.object(self.c,'native',AsyncMock(return_value=result)) as native:
            self.assertEqual((await self.c.execute(request))['state'],'completed');self.assertEqual(native.await_count,1)
    async def test_native_recovery_uncertain_preserved(self):
        request=self.request('recovery_action',job_id=str(uuid.uuid4()),action='cancel');p=request['payload']
        fresh={'name':request['session'],'run_id':p['expected_run_id'],'conversation_id':p['expected_conversation_id'],'recovery':{'id':p['job_id'],'state':'waiting','identity':[p['expected_run_id'],p['expected_conversation_id'],None,None,None]}}
        ack={'request_id':request['request_id'],'name':request['session'],'run_id':p['expected_run_id'],'conversation_id':p['expected_conversation_id'],'job_id':p['job_id'],'action':'cancel','status':'uncertain','error':'interrupted'}
        with patch.object(self.c,'native',AsyncMock(side_effect=[fresh,ack])):
            self.assertEqual((await self.c.execute(request))['state'],'uncertain')
