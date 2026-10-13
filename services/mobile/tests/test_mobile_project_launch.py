"""Project selection binds exact launch receipt; failed placement never relaunches."""
import copy
import os
from pathlib import Path
import tempfile
import unittest
import uuid
from unittest.mock import AsyncMock, patch

from zerus_mobile.connector import Connector, ConnectorError
from zerus_mobile.launch import catalog, projects, validate, project_choice
from test_mobile_projects import native_catalog


class ProjectLaunchTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.connector = Connector({'server_url':'https://relay.example.invalid','node_token':'fixture','state_dir':self.temp.name})
        self.connector.launch_supported = self.connector.project_launch_supported = True
        self.native_projects = native_catalog()
        self.payload = {'request_id':str(uuid.uuid4()),'agent':'codex','account_id':'native-codex','tag':'project launch',
            'directory':'/example/mobile','swarm_id':'native-shared','project_id':'logical','add_folder':True}
        self.request = {'request_id':self.payload['request_id'],'operation':'launch','session':'','payload':self.payload}
        self.accounts = {'profiles':[{'id':'native-codex','provider':'codex','label':'Synthetic','installed':True}]}
        self.target = {'name':'codex/example/project launch','run_id':'new-run','conversation_id':'new-conversation','launch_id':self.payload['request_id']}

    async def asyncTearDown(self):
        await self.connector.accounts.close()
        self.connector.journal.close()
        self.temp.cleanup()

    def assignment(self, status='assigned'):
        return {'request_id':self.payload['request_id'],'name':self.target['name'],'run_id':'new-run','conversation_id':'new-conversation',
                'swarm_id':self.payload['swarm_id'],'project_id':'logical','status':status}

    async def test_successful_placement_and_exact_uuid_retry_do_not_launch_twice(self):
        with patch.object(self.connector,'native',AsyncMock(side_effect=[self.accounts,{'path':'/example/mobile'},self.native_projects,'Created',{'sessions':[self.target]},self.assignment()])) as native:
            result = await self.connector.execute(self.request)
            self.assertEqual(result['state'],'completed',result)
            self.assertEqual(result['result']['project_assignment']['status'],'assigned')
            self.assertEqual(native.call_args_list[-1].args[0],['swarm','assign-launch','--json'])
            self.assertEqual(native.call_args_list[-1].args[1]['request_id'],self.payload['request_id'])
            self.assertEqual(await self.connector.execute(self.request),result)
            self.assertEqual(native.await_count,6)

    async def test_created_target_retained_after_assignment_failure_uncertainty_or_wrong_ack(self):
        for answer in (self.assignment('failed'),ConnectorError('private error'),{**self.assignment(),'run_id':'replacement'}):
            self.payload['request_id']=str(uuid.uuid4());self.request['request_id']=self.payload['request_id']
            self.target['launch_id']=self.payload['request_id']
            if isinstance(answer,dict): answer['request_id']=self.payload['request_id']
            with patch.object(self.connector,'native',AsyncMock(side_effect=[self.accounts,{'path':'/example/mobile'},self.native_projects,'Created',{'sessions':[self.target]},answer])) as native:
                result=await self.connector.execute(self.request)
                self.assertEqual(result['state'],'completed',result)
                self.assertEqual(result['result']['status'],'created')
                self.assertEqual(result['result']['result_target']['name'],self.target['name'])
                self.assertNotEqual(result['result']['project_assignment']['status'],'assigned')
                self.assertIn('not be repeated',result['result']['warning'])
                self.assertEqual(await self.connector.execute(self.request),result)
                self.assertEqual(native.await_count,6)

    async def test_missing_old_cli_or_changed_project_rejects_before_creation(self):
        self.connector.project_launch_supported=False
        with patch.object(self.connector,'native',AsyncMock(side_effect=[self.accounts,{'path':'/example/mobile'}])) as native:
            self.assertEqual((await self.connector.execute(self.request))['state'],'failed')
            self.assertEqual(native.await_count,2)
        self.connector.project_launch_supported=True
        self.payload['request_id']=str(uuid.uuid4());self.request['request_id']=self.payload['request_id'];self.payload['swarm_id']='other'
        with patch.object(self.connector,'native',AsyncMock(side_effect=[self.accounts,{'path':'/example/mobile'},self.native_projects])) as native:
            self.assertEqual((await self.connector.execute(self.request))['state'],'failed')
            self.assertEqual(native.await_count,3)

    async def test_symlink_saved_folder_uses_fresh_directory_equivalence(self):
        self.payload.update(directory='/example/link',project_folder_id='f-local',add_folder=False)
        self.native_projects['organization']['projects'][0]['folders'][0]['path']='/example/link'
        with patch.object(self.connector,'native',AsyncMock(side_effect=[self.accounts,{'path':'/example/mobile'},self.native_projects,{'path':'/example/mobile'},'Created',{'sessions':[self.target]},self.assignment()])) as native:
            result=await self.connector.execute(self.request)
            self.assertEqual(result['result']['project_assignment']['status'],'assigned',result)
            self.assertEqual(native.call_args_list[3].args[0],['dirs','/example/link'])


class ProjectCatalogTests(unittest.TestCase):
    def test_exact_profiles_default_and_cached_email_survive_without_paths_or_secrets(self):
        rows=[{'id':'work-one','provider':'claude','label':'Same','installed':True,'is_default':True,'account_status':{'auth_status':'signed_in','identity':{'email':'account@example.invalid','token':'private'}},'home':'/private'},
              {'id':'work-two','provider':'claude','label':'Same','installed':True,'account_status':{'status':'expired'}}]
        result=catalog({'profiles':rows})
        self.assertEqual([row['id'] for row in result['accounts']],['work-one','work-two'])
        self.assertTrue(result['accounts'][0]['is_default'])
        self.assertEqual([row['auth_status'] for row in result['accounts']],['signed_in','expired'])
        self.assertEqual(result['accounts'][0]['usage']['identity'],{'email':'account@example.invalid'})
        self.assertNotIn('private',str(result))

    def test_project_catalog_has_only_local_folders_and_accessible_groups(self):
        result=projects(native_catalog())
        self.assertEqual([row['id'] for row in result['projects']],['logical','ungrouped'])
        self.assertEqual([row['id'] for row in result['projects'][0]['folders']],['f-local'])
        self.assertNotIn('peers',result)
        self.assertNotIn('/private/remote',str(result))

    def test_project_fields_and_existing_folder_intent_are_validated_before_mutation(self):
        base={'request_id':'request','agent':'codex','directory':'/example/mobile','tag':'mobile launch'}
        validate('launch',base,'request')
        for fields in ({'project_id':'logical'},{'swarm_id':'swarm','project_id':'x'*129,'add_folder':True},
                       {'swarm_id':'swarm','project_id':'logical','add_folder':'true'},
                       {'swarm_id':'swarm','project_id':'界'*43,'add_folder':True},
                       {'swarm_id':'swarm','project_id':'logical','project_folder_id':'f','add_folder':True}):
            with self.assertRaises(ValueError):validate('launch',{**base,**fields},'request')
