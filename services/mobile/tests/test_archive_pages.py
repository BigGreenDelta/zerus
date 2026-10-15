"""Additive paged archive read contract; exact identity admission stays bounded."""
import json
import tempfile
import time
import unittest
import uuid
from unittest.mock import AsyncMock, patch
from zerus_mobile import archives
from zerus_mobile.connector import Connector
from zerus_mobile.context import READ_OPERATIONS, READ_SQL
from zerus_mobile.server import validate_request

class ArchiveValidation(unittest.TestCase):
    def test_read_class_strict_bounds_and_sessionless_envelope(self):
        self.assertIn('archive_list',READ_OPERATIONS);self.assertNotIn('archive_list',READ_SQL)
        for payload in ({},{'from':10,'until':11,'limit':1},{'query':'Русский %_'}):
            archives.validate(payload)
            validate_request(dict(request_id=str(uuid.uuid4()),computer_id=str(uuid.uuid4()),operation='archive_list',session='',payload=payload))
        for payload in ({'limit':51},{'limit':True},{'from':5,'until':5},{'until':253402300801},{'cursor':'#'},{'query':'x\nx'},{'offset':1000}):
            with self.assertRaises(ValueError):archives.validate(payload)
        with self.assertRaises(ValueError):validate_request(dict(request_id=str(uuid.uuid4()),computer_id=str(uuid.uuid4()),operation='archive_list',session='codex/example/x',payload={}))

class ArchiveConnector(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.node=Connector({'server_url':'https://gateway.example','node_token':'synthetic','hgs_path':'/synthetic/hgs','state_dir':self.temp.name})
        self.addCleanup(self.node.journal.close);self.node.archive_supported=True
    def request(self,**payload):return dict(request_id=str(uuid.uuid4()),operation='archive_list',session='',payload=payload)
    def page(self):return dict(version=1,sessions=[dict(name='codex/example/a',archive_id='10000000-0000-4000-8000-000000000001',state='archived',archived_at=100)],next_cursor='',total=1,**{'from':0,'until':253402300800,'query':''})
    async def test_page_provenance_survives_active_snapshot_and_native_recheck_remains(self):
        req=self.request();self.node.fleet.resolve=AsyncMock(return_value=(self.node,None))
        with patch.object(self.node,'native',AsyncMock(return_value=self.page())) as native:
            result=await self.node.execute(req)
        self.assertEqual(result['state'],'completed',result);self.assertEqual(native.call_args.args,(['archives','--json'],{}));self.assertEqual(native.call_args.kwargs['timeout'],30)
        self.node.archives=set();self.node.snapshot_seen_at=time.monotonic()
        self.assertTrue(self.node.archive_allowed('codex/example/a','10000000-0000-4000-8000-000000000001'))
        self.assertFalse(self.node.archive_allowed('codex/example/other','10000000-0000-4000-8000-000000000001'))
        target=('codex/example/a','10000000-0000-4000-8000-000000000001');self.node.paged_archives[target]-=3601
        self.assertFalse(self.node.archive_allowed(*target))
    async def test_bad_native_pages_never_grant_archive_admission(self):
        self.node.fleet.resolve=AsyncMock(return_value=(self.node,None))
        for edit in ({'version':2},{'total':0},{'from':5},{'next_cursor':'#'},{'sessions':[]}):
            page={**self.page(),**edit}
            if edit=={'sessions':[]}:page['next_cursor']='loop'
            with patch.object(self.node,'native',AsyncMock(return_value=page)):
                self.assertEqual((await self.node.execute(self.request()))['state'],'failed')
            self.assertFalse(self.node.paged_archives)
    async def test_uncached_archive_requires_exact_native_lookup_before_any_action(self):
        self.node.fleet.resolve=AsyncMock(return_value=(self.node,None))
        archive='10000000-0000-4000-8000-000000000001';name='codex/example/a'
        detail=dict(version=1,name=name,archive_id=archive,run_id='old-run',conversation_id='old-conversation')
        def inspect():return dict(request_id=str(uuid.uuid4()),operation='inspect',session=name,payload={'archive_id':archive})
        for wrong in ({**detail,'archive_id':str(uuid.uuid4())},{**detail,'name':name+'other'},{**detail,'version':True}):
            with patch.object(self.node,'native',AsyncMock(return_value=wrong)) as native:
                self.assertEqual((await self.node.execute(inspect()))['state'],'failed')
                self.assertEqual(native.call_count,1)
            self.assertFalse(self.node.paged_archives)
        req=dict(request_id=str(uuid.uuid4()),operation='forget',session=name)
        req['payload']=dict(request_id=req['request_id'],archive_id=archive,expected_run_id='replacement-run',expected_conversation_id='old-conversation')
        with patch.object(self.node,'native',AsyncMock(return_value=detail)) as native:
            self.assertEqual((await self.node.execute(req))['state'],'failed');self.assertEqual(native.call_count,1)
        self.assertFalse(self.node.paged_archives,'Wrong-run metadata must not admit a mutation')
        with patch.object(self.node,'native',AsyncMock(side_effect=[detail,{'name':name,'archive_id':archive}])) as native:
            self.assertEqual((await self.node.execute(inspect()))['state'],'completed')
            self.assertEqual(native.call_args_list[0].args,(['archives','lookup','--json'],{'name':name,'archive_id':archive}))
        self.assertTrue(self.node.archive_allowed(name,archive))
    async def test_only_successful_deliverable_archive_reads_extend_admission(self):
        self.node.fleet.resolve=AsyncMock(return_value=(self.node,None))
        name='codex/example/a';archive='10000000-0000-4000-8000-000000000001'
        key=(name,archive);prior=time.monotonic()-30;self.node.paged_archives[key]=prior
        def request():return dict(request_id=str(uuid.uuid4()),operation='inspect',session=name,payload={'archive_id':archive})
        with patch.object(self.node,'native',AsyncMock(return_value={'text':'x'*(1024*1024)})):
            self.assertEqual((await self.node.execute(request()))['state'],'failed')
        self.assertEqual(self.node.paged_archives[key],prior)
        with patch.object(self.node,'native',AsyncMock(return_value={'name':name,'archive_id':archive})):
            self.assertEqual((await self.node.execute(request()))['state'],'completed')
        self.assertGreater(self.node.paged_archives[key],prior)

from bb_operations import real_hgs, OperationCase
from connector_harness import BlackBoxCase, listing

class NativeArchivePaging(OperationCase,BlackBoxCase):
    async def test_real_native_pages_stay_lazy_and_admit_exact_archived_inspection(self):
        native=real_hgs()
        if native is None:self.fail('Build the native hgs before running archive transport contracts')
        state=self.scratch/'hgs-state';records=state/'archives';records.mkdir(parents=True)
        for i in range(121):
            ident=str(uuid.UUID(int=i+1))
            value=dict(version=1,name=f'codex/archive-fixture/{i}',agent='codex',archive_id=ident,archived_at=100+i,
                       run_id=f'run-{i}',conversation_id=f'conversation-{i}',state='archived',created=1,cwd=str(self.scratch))
            (records/(ident+'.json')).write_text(json.dumps(value),encoding='utf-8')
        self.hgs.prepend({'argv':['ls','--json','--local','--no-archives'],'stdout':{**listing(),'archive_revision':'fixture-revision'}},{'argv':['--help'],'exec':native},{'prefix':['archives'],'exec':native},{'prefix':['inspect'],'exec':native})
        process=await self.launch();mark=self.mark()
        snapshot=self.relay.heartbeats()[-1].body['snapshot']
        self.assertIn('archive_list',snapshot['mobile_capabilities']['operations'])
        legacy=[row for row in snapshot['sessions'] if row.get('state')=='archived']
        self.assertEqual(len(legacy),50);self.assertEqual(snapshot['mobile_archive']['total'],121);self.assertTrue(snapshot['mobile_archive']['partial'])
        self.assertEqual(self.since(mark,'archives'),[])
        def request(cursor=''):return dict(request_id=str(uuid.uuid4()),computer_id=self.relay.computer_id,operation='archive_list',session='',payload={'from':0,'until':253402300800,'limit':50,'cursor':cursor,'query':''})
        first=await self.submit(request(),process);self.assertEqual(first['state'],'completed',first)
        page=first['result'];self.assertEqual(page['total'],121);self.assertEqual(len(page['sessions']),50)
        self.assertEqual(len(self.since(mark,'archives')),1)
        second=await self.submit(request(page['next_cursor']),process);self.assertEqual(second['state'],'completed',second)
        self.assertEqual(len(second['result']['sessions']),50)
        self.assertFalse({r['archive_id']for r in page['sessions']} & {r['archive_id']for r in second['result']['sessions']})
        row=second['result']['sessions'][0]
        self.assertNotIn(row['archive_id'],{entry['archive_id'] for entry in legacy},'The inspection must exercise a target outside the legacy window')
        inspect=dict(request_id=str(uuid.uuid4()),computer_id=self.relay.computer_id,operation='inspect',session=row['name'],payload={'archive_id':row['archive_id']})
        result=await self.submit(inspect,process);self.assertEqual(result['state'],'completed',result)
        self.assertEqual(result['result']['archive_id'],row['archive_id']);self.assertEqual(result['result']['run_id'],row['run_id'])
        self.assertEqual(len(self.since(mark,'archives')),2,'Inspect must not load the entire archive again')
        process.stop()
        process=await self.launch()
        mark=self.mark()
        # The phone can retain this exact target while the connector restarts.
        # It is deliberately absent from the bounded recent snapshot window.
        inspect['request_id']=str(uuid.uuid4())
        result=await self.submit(inspect,process);self.assertEqual(result['state'],'completed',result)
        self.assertEqual(result['result']['archive_id'],row['archive_id'])
        self.assertEqual(result['result']['run_id'],row['run_id'])
        self.assertEqual([call['argv'] for call in self.since(mark,'archives')],[['archives','lookup','--json']])
