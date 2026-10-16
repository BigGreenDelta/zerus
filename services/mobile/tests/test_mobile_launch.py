"""Safe fixed-argument creation, exact launch UUID and minimal account projection."""
import os
import json
from pathlib import Path
import tempfile
import unittest
import uuid
from unittest.mock import AsyncMock, patch
from zerus_mobile.connector import Connector, native_environment
from relay_oracle import validate_request
from zerus_mobile.launch import catalog


class LaunchTransport(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.connector=Connector({'server_url':'https://relay.example.invalid','node_token':'fixture','state_dir':self.temp.name})
        self.connector.launch_supported=True

    async def asyncTearDown(self):
        self.connector.journal.close();self.temp.cleanup()

    def request(self, operation='launch', **fields):
        identity=str(uuid.uuid4())
        payload={'request_id':identity}
        if operation=='launch':payload.update(agent='codex',directory='/example/mobile',tag='new-session')
        payload.update(fields)
        return {'request_id':identity,'computer_id':str(uuid.uuid4()),'operation':operation,'session':'','payload':payload}

    async def test_exact_marker_required_and_duplicate_cannot_launch_again(self):
        request=self.request(account_id='native-codex')
        catalog={'profiles':[{'id':'native-codex','provider':'codex','label':'Synthetic','installed':True,'secret':'must not escape'}]}
        target={'name':'codex/example/new-session','run_id':'new-run','conversation_id':'new-conversation','launch_id':request['request_id']}
        with patch.object(self.connector,'native',AsyncMock(side_effect=[catalog,{'path':'/example/mobile'},'Created',{'sessions':[target]}])) as native:
            result=await self.connector.execute(request)
            self.assertEqual(result['state'],'completed');self.assertEqual(result['result']['status'],'created')
            self.assertEqual(await self.connector.execute(request),result);self.assertEqual(native.await_count,4)
            self.assertEqual(native.call_args_list[2].args[0],['codex','/example/mobile','--new','-n','new-session','-d','--launch-id',request['request_id'],'--account','native-codex'])
            self.assertNotIn('launch_id',result['result']['result_target'])

    async def test_preflight_old_cli_invalid_fields_and_wrong_provider_do_not_launch(self):
        self.connector.launch_supported=False
        with patch.object(self.connector,'native',AsyncMock()) as native:
            self.assertEqual((await self.connector.execute(self.request()))['state'],'failed');native.assert_not_awaited()
        self.connector.launch_supported=True
        for fields in ({'directory':'../relative'},{'tag':'x/evil'},{'tag':'x.y'},{'agent':'shell'},{'command':'rm'},{'account_id':'../secret'}):
            request=self.request(**fields)
            with self.assertRaises(Exception):validate_request(request)
            with patch.object(self.connector,'native',AsyncMock()) as native:
                self.assertEqual((await self.connector.execute(request))['state'],'failed');native.assert_not_awaited()
        with patch.object(self.connector,'native',AsyncMock(return_value={'profiles':[{'id':'native-claude','provider':'claude','label':'Synthetic','installed':True}]})) as native:
            self.assertEqual((await self.connector.execute(self.request()))['state'],'failed');self.assertEqual(native.await_count,1)

    async def test_native_handoff_failure_is_uncertain_and_never_replayed(self):
        from zerus_mobile.connector import ConnectorError
        request=self.request()
        accounts={'profiles':[{'id':'native-codex','provider':'codex','label':'Synthetic','installed':True}]}
        with patch.object(self.connector,'native',AsyncMock(side_effect=[accounts,{'path':'/example/mobile'},ConnectorError('native command timed out')])) as native:
            result=await self.connector.execute(request);self.assertEqual(result['state'],'uncertain')
            self.assertEqual(await self.connector.execute(request),result);self.assertEqual(native.await_count,3)

    async def test_opencode_exact_offered_model_reaches_fixed_native_launch_once(self):
        request=self.request(agent='opencode',account_id='native-opencode',model='ollama/qwen3-coder:30b')
        accounts={'profiles':[{'id':'native-opencode','provider':'opencode','label':'Native OpenCode','installed':True,
                              'model_options':[{'value':'ollama/qwen3-coder:30b','label':'Local coding model','api_key':'private'}]}]}
        target={'name':'opencode/example/new-session','run_id':'new-run','conversation_id':'ses_native','launch_id':request['request_id']}
        with patch.object(self.connector,'native',AsyncMock(side_effect=[accounts,{'path':'/example/mobile'},'Created',{'sessions':[target]}])) as native:
            result=await self.connector.execute(request)
            self.assertEqual(result['state'],'completed',result)
            self.assertEqual(await self.connector.execute(request),result)
            self.assertEqual(native.await_count,4)
            self.assertEqual(native.call_args_list[2].args[0],['opencode','/example/mobile','--new','-n','new-session','-d','--launch-id',request['request_id'],'--account','native-opencode','--model','ollama/qwen3-coder:30b'])
            self.assertEqual(result['result']['result_target']['conversation_id'],'ses_native')

    async def test_changed_or_other_account_model_blocks_before_directory_or_launch(self):
        request=self.request(agent='opencode',account_id='native-opencode',model='ollama/qwen3-coder:30b')
        for rows in ([],[{'value':'other/local','label':'Another model'}]):
            accounts={'profiles':[{'id':'native-opencode','provider':'opencode','label':'Native OpenCode','installed':True,'model_options':rows}]}
            with patch.object(self.connector,'native',AsyncMock(return_value=accounts)) as native:
                result=await self.connector.execute(request)
                self.assertEqual(result['state'],'failed',result)
                self.assertEqual(native.await_count,1)
            request['request_id']=request['payload']['request_id']=str(uuid.uuid4())

    async def test_opencode_default_model_does_not_require_or_inject_a_model(self):
        request=self.request(agent='opencode',account_id='native-opencode')
        accounts={'profiles':[{'id':'native-opencode','provider':'opencode','label':'Native OpenCode','installed':True}]}
        target={'name':'opencode/example/new-session','run_id':'new-run','conversation_id':'ses_default','launch_id':request['request_id']}
        with patch.object(self.connector,'native',AsyncMock(side_effect=[accounts,{'path':'/example/mobile'},'Created',{'sessions':[target]}])) as native:
            self.assertEqual((await self.connector.execute(request))['state'],'completed')
            self.assertNotIn('--model',native.call_args_list[2].args[0])

    async def test_model_route_is_bounded_and_specific_to_native_opencode(self):
        valid={'agent':'opencode','account_id':'native-opencode','model':'ollama/qwen3-coder:30b'}
        validate_request(self.request(**valid))
        validate_request(self.request(**{**valid,'model':'ollama/' + ('界' * 83)}))  # 256 UTF-8 bytes
        for delta in ({'agent':'codex'},{'account_id':'native-codex'},{'model':'local'},
                      {'model':'/local'},{'model':'ollama/'},{'model':'ollama/with space'},
                      {'model':'ollama/'+('a'*257)},{'model':'ollama/'+('界'*84)},{'model':'ollama/local\n'}):
            with self.assertRaises(Exception):validate_request(self.request(**{**valid,**delta}))

    async def test_fixture_catalog_launch_then_inspect_and_send_new_exact_target(self):
        with patch.dict(os.environ,{'ZERUS_MOBILE_FIXTURE_DIR':self.temp.name}):
            self.connector.hgs=str(Path(__file__).with_name('fixture_hgs.py').resolve())
            catalog=await self.connector.execute(self.request('catalog'))
            self.assertEqual(catalog['state'],'completed')
            self.assertEqual(set(catalog['result']['accounts'][0]),{'id','provider','label','native','installed','is_default','auth_status','usage'})
            result=await self.connector.execute(self.request())
            self.assertEqual(result['state'],'completed',result)
            target=result['result']['result_target']
            snapshot=await self.connector.snapshot()
            self.assertIn(target['name'],self.connector.sessions)
            request=self.request('inspect');request.update(session=target['name'],payload={})
            detail=await self.connector.execute(request)
            self.assertEqual(detail['result']['conversation_id'],target['conversation_id'])
            identity=str(uuid.uuid4())
            sent=await self.connector.execute({'request_id':identity,'operation':'send','session':target['name'],'payload':{'request_id':identity,'expected_run_id':target['run_id'],'expected_conversation_id':target['conversation_id'],'text':'Synthetic new-session message'}})
            self.assertEqual(sent['state'],'completed',sent)


class CatalogPrivacy(unittest.TestCase):
    def test_inherited_opencode_configuration_cannot_retarget_connector_calls(self):
        with patch.dict(os.environ,{'OPENCODE_CONFIG_DIR':'/private/foreign-agent','OPENCODE_CONFIG':'/private/foreign.json','OPENCODE_CONFIG_CONTENT':'{"provider":{"foreign":{}}}','HGS_STATE_DIR':'/private/fixture-state'}):
            value=native_environment()
        self.assertNotIn('OPENCODE_CONFIG_DIR',value)
        self.assertNotIn('OPENCODE_CONFIG',value)
        self.assertNotIn('OPENCODE_CONFIG_CONTENT',value)
        self.assertEqual(value['HGS_STATE_DIR'],'/private/fixture-state')
    def test_cached_limits_are_projected_without_native_errors_or_credentials(self):
        value=catalog({'profiles':[{'id':'native-codex','provider':'codex','label':'Alias','installed':True,
            'account_status':{'status':'ok','identity':{'email':'account@example.invalid','token':'private'},
                'windows':[{'window_minutes':300,'used_percent':17,'resets_at':1900000000,'secret':'private'}],
                'error':'private','auth_revision':'private','home':'/private'}}]})
        usage=value['accounts'][0]['usage']
        self.assertEqual(usage['identity'],{'email':'account@example.invalid'})
        self.assertEqual(usage['windows'],[{'window_minutes':300,'used_percent':17,'resets_at':1900000000}])
        self.assertNotIn('private',json.dumps(value))

    def test_only_selected_public_fields_are_forwarded(self):
        value=catalog({'profiles':[{'id':'native-codex','provider':'codex','label':'Synthetic','installed':True,'home':'/private','credentials':{'secret':'private'}}],'defaults':{'private':'secret'}})
        self.assertEqual(value,{'accounts':[{'id':'native-codex','provider':'codex','label':'Synthetic','native':False,'installed':True,'is_default':False,'auth_status':'unknown'}],'agents':['codex']})

    def test_opencode_models_project_only_bounded_ids_labels_and_provider(self):
        raw={'profiles':[{'id':'native-opencode','provider':'opencode','label':'Native OpenCode','installed':True,
                         'home':'/private/config','credentials':{'secret':'private'},'model_options':[
                             {'value':'ollama/qwen3-coder:30b','label':'Local coding','provider':'private','base_url':'https://private.example.test','api_key':'private'},
                             {'value':'ollama/qwen3-coder:30b','label':'Duplicate'},None,{'value':'not-a-route'},
                             {'value':'ollama/with space'},{'value':'ollama/local\n'},{'value':'ollama/'+('a'*257)}]}]}
        value=catalog(raw)
        self.assertEqual(value['agents'],['opencode'])
        self.assertEqual(value['accounts'][0]['model_options'],[{'value':'ollama/qwen3-coder:30b','label':'Local coding','provider':'ollama'}])
        self.assertNotIn('private',str(value))

    def test_model_catalog_cannot_exceed_public_byte_bound(self):
        raw={'profiles':[{'id':'native-opencode-'+str(i),'provider':'opencode','label':'Native OpenCode','installed':True,
                          'model_options':[{'value':'local/model'+str(j),'label':'界'*160} for j in range(256)]} for i in range(2)]}
        with self.assertRaises(ValueError):catalog(raw)
