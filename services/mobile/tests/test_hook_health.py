"""Hook diagnostics are scoped, read-only, opt-in and never grant trust."""
import copy,json,tempfile,unittest,uuid,hashlib
from pathlib import Path
from unittest.mock import AsyncMock,patch
from zerus_mobile.connector import Connector
from zerus_mobile import hook_health
from zerus_mobile.context import READ_OPERATIONS
from zerus_mobile.server import validate_request
from zerus_mobile.store import Store

NAME='claude/example/task'
def request():
    id=str(uuid.uuid4())
    return dict(request_id=id,operation='hooks_status',session=NAME,computer_id=str(uuid.uuid4()),payload=dict(request_id=id,expected_run_id='run-1',expected_conversation_id='one'))
def result(req):
    return dict(schema=1,provider='claude',account_id='native-claude',configuration=dict(state='missing',compatibility=dict(state='unknown')),effective=dict(state='unknown',reason='Not verified'),trust=dict(state='unknown',reason='Not verified'),observed=dict(state='unknown',events=[]),running_agents_retrofitted=False,request_id=req['request_id'],name=NAME,run_id='run-1',conversation_id='one')

class HookHealthContracts(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='hook-health-connector-')
        self.connector=Connector(dict(server_url='https://relay.example.invalid',node_token='synthetic',state_dir=self.temp.name))
        self.connector.sessions={NAME};self.connector.hook_health_supported=True
    async def asyncTearDown(self):
        self.connector.journal.close();self.temp.cleanup()
    async def test_read_only_argv_exact_scope_and_receipt_replay(self):
        req=request()
        with patch.object(self.connector,'native',AsyncMock(return_value=result(req))) as native:
            first=await self.connector.execute(req);self.assertEqual(first['state'],'completed')
            self.assertEqual(await self.connector.execute(req),first)
            native.assert_awaited_once_with(['hooks',NAME,'--json'],req['payload'],timeout=5,max_output_bytes=16*1024)
        self.assertIn('hooks_status',READ_OPERATIONS)
        self.connector.journal.db.execute('UPDATE requests SET delivered=1,delivered_at=1')
        self.connector.journal.prune(now=1000)
        self.assertEqual(self.connector.journal.db.execute('SELECT count(*) FROM requests').fetchone()[0],0)
    async def test_missing_abi_wrong_scope_and_write_fields_never_execute(self):
        self.connector.hook_health_supported=False
        with patch.object(self.connector,'native',AsyncMock()) as native:
            self.assertEqual((await self.connector.execute(request()))['error'],hook_health.UNAVAILABLE);native.assert_not_awaited()
        self.connector.hook_health_supported=True
        for update in [dict(action='repair'),dict(account_id='foreign'),dict(archive_id=str(uuid.uuid4())),dict(agent_id='child'),dict(expected_run_id=''),dict(expected_conversation_id='')]:
            req=request();req['payload'].update(update)
            with patch.object(self.connector,'native',AsyncMock()) as native:
                self.assertEqual((await self.connector.execute(req))['state'],'failed');native.assert_not_awaited()
            with self.assertRaises(Exception):validate_request(req)
    async def test_mismatched_and_private_envelopes_fail_as_reads(self):
        for field in ['request_id','name','run_id','conversation_id','schema','private_settings']:
            req=request();raw=result(req);raw[field]='foreign'
            with patch.object(self.connector,'native',AsyncMock(return_value=raw)) as native:
                receipt=await self.connector.execute(req);self.assertEqual(receipt['state'],'failed')
                self.assertEqual(await self.connector.execute(req),receipt);self.assertEqual(native.await_count,1)
        req=request();raw=result(req);raw['configuration']['private_settings']={'secret':'never transport'}
        self.assertFalse(hook_health.result_valid(raw,req['payload'],NAME))
        for key in ['effective','trust']:
            req=request();raw=result(req);raw[key]['state']='verified'
            self.assertFalse(hook_health.result_valid(raw,req['payload'],NAME))
    async def test_probe_is_opt_in_and_never_invokes_a_provider(self):
        for help,expected in [('inspect answer',False),('hooks-health-v1',True)]:
            self.connector.capabilities_next_poll=0
            with patch.object(self.connector,'native',AsyncMock(return_value=help)) as native:
                await self.connector.probe_capabilities();self.assertEqual(self.connector.hook_health_supported,expected)
                native.assert_awaited_once_with(['--help'],timeout=3,json_output=False)
    def test_relay_and_native_payload_validator_accept_only_the_three_read_fields(self):
        req=request();validate_request(req);hook_health.validate(req['payload'],req['request_id'])
        for extra in ['repair','expected_path_id','expected_account_id']:
            bad=copy.deepcopy(req);bad['payload'][extra]='anything'
            with self.assertRaises(Exception):validate_request(bad)

    def test_relay_read_retention_is_indexed_and_never_expires_mutation_receipts(self):
        store=Store(Path(self.temp.name)/'relay.sqlite')
        try:
            workspace=store.workspace('Synthetic');node=store.node(workspace,'Synthetic')
            phone=store.pair(store.invite(workspace)['pair_code'],'Synthetic')
            ids=[]
            for operation in ['hooks_status','send']:
                id=str(uuid.uuid4());ids.append(id)
                store.db.execute("INSERT INTO requests(id,workspace_id,device_id,node_id,operation,body_hash,body_bytes,body,state,result,created,updated) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
                    (id,workspace,phone['device_id'],node['node_id'],operation,hashlib.sha256(b'{}').hexdigest(),2,'{}','completed','{}',1,1))
            query="SELECT id FROM requests INDEXED BY hook_health_history WHERE operation='hooks_status' AND state IN ('completed','failed','uncertain') AND updated<? ORDER BY updated,id LIMIT ?"
            plan=[tuple(row) for row in store.db.execute('EXPLAIN QUERY PLAN '+query,(880,256))]
            self.assertTrue(any('hook_health_history' in str(row) for row in plan))
            with patch('zerus_mobile.store.time.time',return_value=1000):store.maintain()
            self.assertIsNone(store.db.execute('SELECT id FROM requests WHERE id=?',(ids[0],)).fetchone())
            self.assertEqual(tuple(store.db.execute('SELECT body,result FROM requests WHERE id=?',(ids[1],)).fetchone()),('{}','{}'))
        finally:store.close()
