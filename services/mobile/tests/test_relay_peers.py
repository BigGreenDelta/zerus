"""One-hop registry, immutable dispatch, and bounded catalog regressions."""
import inspect
import json
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch
import uuid

from aiohttp.test_utils import TestClient, TestServer
from zerus_mobile.routes import RouteError
from zerus_mobile.server import Config, create_app
from zerus_mobile.store import Store, canonical, digest


def identity():return str(uuid.uuid4())
def snapshot(name='session',**extra):
    return {'sessions':[{'name':name,'run_id':'run','conversation_id':'conversation',**extra}],
            'mobile_capabilities':{'protocol_version':1,'operations':['inspect','send'], 'features':['gateway_one_hop']}}


class SQLitePeerTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.store=Store(Path(self.temp.name)/'db')
        await self.setup_identity()

    async def setup_identity(self):
        self.workspace=await self.call('workspace','Example')
        self.gateway=await self.call('node',self.workspace,'Gateway')
        self.node=await self.call('authenticate',self.gateway['node_token'],'nodes')
        self.phone=await self.call('pair',(await self.call('invite',self.workspace))['pair_code'],'Phone')
        self.device=await self.call('authenticate',self.phone['device_token'],'devices')
        self.local,self.remote,self.route=identity(),identity(),identity()
        self.peers=[{'route_id':self.route,'machine_id':self.remote,'name':'Remote','online':True}]
        await self.manifest()
        await self.call('peer_heartbeat',self.node,self.route,self.remote,snapshot())
        self.target=next(row['id'] for row in await self.call('computers',self.workspace) if row['machine_id']==self.remote)

    async def asyncTearDown(self):
        await self.call('close');self.temp.cleanup()

    async def call(self,method,*args,**kwargs):
        function=getattr(self.store,method)
        return await function(*args,**kwargs) if inspect.iscoroutinefunction(function) else function(*args,**kwargs)

    async def sql(self,sql,*args):
        return [dict(row) for row in self.store.db.execute(sql,args)]

    async def manifest(self,peers=None,snap=None):
        return await self.call('heartbeat',self.node,snap or snapshot(),machine_id=self.local,peers=self.peers if peers is None else peers)

    def command(self,target=None):
        return {'request_id':identity(),'computer_id':target or self.target,'operation':'inspect','session':'session','payload':{}}

    async def submit(self,body):
        return await self.call('submit',self.device,body,200,100*1024*1024)

    async def test_peer_creates_no_credential_and_workspace_isolation(self):
        self.assertEqual(len(await self.sql('SELECT id FROM nodes')),1)
        other=await self.call('workspace','Other')
        foreign=await self.call('node',other,'Foreign')
        result=await self.submit(self.command(foreign['node_id']))
        self.assertEqual(result[0],404)
        self.assertEqual(len(await self.call('computers',self.workspace)),2)

    async def test_claim_requires_explicit_optin_and_pins_native_identity(self):
        body=self.command();self.assertEqual((await self.submit(body))[0],202)
        self.assertFalse(await self.call('has_pending',self.node))
        self.assertEqual(await self.call('claim',self.node),[])
        commands=await self.call('claim',self.node,allow_gateway=True)
        self.assertEqual(commands[0]['gateway_route'],{'schema':1,'route_id':self.route,'computer_id':self.target,'machine_id':self.remote})
        self.assertEqual(await self.call('claim',self.node,allow_gateway=True),[])
        local=self.command(self.gateway['node_id']);await self.submit(local)
        self.assertEqual(await self.call('claim',self.node),[])
        self.assertIsNone((await self.call('claim',self.node,allow_gateway=True))[0]['gateway_route']['route_id'])

    async def test_withdraw_then_readd_never_revives_old_request_or_receipt(self):
        old=self.command();await self.submit(old)
        await self.manifest([]);await self.manifest();await self.call('peer_heartbeat',self.node,self.route,self.remote,snapshot())
        self.assertEqual((await self.submit(old))[1]['state'],'failed')
        new=self.command();await self.submit(new)
        self.assertEqual((await self.call('claim',self.node,allow_gateway=True))[0]['request_id'],new['request_id'])
        rows=await self.sql('SELECT body_hash FROM requests WHERE id=?',old['request_id'])
        self.assertEqual(rows[0]['body_hash'],digest(canonical(old)))

    async def test_withdraw_after_claim_is_uncertain_and_exact_result_cannot_resolve(self):
        body=self.command();await self.submit(body);await self.call('claim',self.node,allow_gateway=True)
        await self.manifest([])
        self.assertEqual((await self.call('get_request',self.device,body['request_id']))['state'],'uncertain')
        self.assertEqual(await self.call('result',self.node,body['request_id'],{'state':'completed','result':{},'error':None}),409)
        usage=(await self.sql('SELECT active,payload_bytes FROM workspace_usage WHERE workspace_id=?',self.workspace))[0]
        total=(await self.sql('SELECT sum(body_bytes+result_bytes+reserved_bytes) AS n FROM requests WHERE workspace_id=?',self.workspace))[0]['n']
        self.assertEqual(usage,{'active':0,'payload_bytes':total})

    async def test_gateway_freshness_never_refreshes_peer_snapshot(self):
        await self.sql('UPDATE computer_routes SET last_seen=? WHERE gateway_id=? AND route_id=?',time.time()-60,self.node['id'],self.route)
        if not inspect.iscoroutinefunction(self.store.heartbeat):self.store.db.commit()
        await self.manifest()
        catalog=await self.call('computers',self.workspace)
        remote=next(row for row in catalog if row['id']==self.target)
        self.assertFalse(remote['available'])
        self.assertEqual((await self.submit(self.command()))[0],409)

    async def test_offline_peer_keeps_last_seen_without_becoming_fresh(self):
        async def listed(machine):
            return next((row for row in await self.call('computers',self.workspace) if row['machine_id']==machine),None)
        seen=(await listed(self.remote))['last_seen_at']
        self.assertIsNotNone(seen)
        offline=[{**self.peers[0],'online':False}]
        await self.manifest(offline);await self.manifest(offline,snapshot('changed'))
        row=await listed(self.remote)
        self.assertFalse(row['available']);self.assertIsNone(row['snapshot'])
        self.assertEqual(row['last_seen_at'],seen)
        # Marked online again, the retained time is not freshness evidence.
        await self.manifest()
        row=await listed(self.remote)
        self.assertFalse(row['available']);self.assertEqual(row['last_seen_at'],seen)
        self.assertEqual((await self.submit(self.command()))[0],409)
        await self.call('peer_heartbeat',self.node,self.route,self.remote,snapshot())
        row=await listed(self.remote)
        self.assertTrue(row['available']);self.assertGreaterEqual(row['last_seen_at'],seen)
        # A changed verified identity on the same route starts without history.
        replacement=identity()
        await self.manifest([{**offline[0],'machine_id':replacement}])
        self.assertIsNone(await listed(self.remote))
        self.assertIsNone((await listed(replacement))['last_seen_at'])

    async def test_stale_frozen_route_cannot_reroute_to_fresh_direct_node(self):
        body=self.command();await self.submit(body)
        direct=await self.call('node',self.workspace,'Direct remote')
        directrow=await self.call('authenticate',direct['node_token'],'nodes')
        await self.call('heartbeat',directrow,snapshot(),machine_id=self.remote,peers=[])
        await self.sql('UPDATE computer_routes SET last_seen=? WHERE gateway_id=? AND route_id=?',time.time()-60,self.node['id'],self.route)
        if hasattr(self.store,'db'):self.store.db.commit()
        self.assertEqual(await self.call('claim',directrow,allow_gateway=True),[])
        self.assertEqual(await self.call('claim',self.node,allow_gateway=True),[])
        self.assertEqual((await self.call('get_request',self.device,body['request_id']))['state'],'failed')

    async def test_unexposed_direct_enrollment_alias_attaches_existing_peer(self):
        direct=await self.call('node',self.workspace,'Direct remote')
        row=await self.call('authenticate',direct['node_token'],'nodes')
        await self.call('heartbeat',row,snapshot(),machine_id=self.remote,peers=[])
        catalog=await self.call('computers',self.workspace)
        remote=[r for r in catalog if r['machine_id']==self.remote]
        self.assertEqual(len(remote),1);self.assertEqual(remote[0]['id'],self.target)
        self.assertIn(direct['node_id'],remote[0]['aliases']);self.assertIsNone(remote[0]['via'])
        body=self.command(direct['node_id']);await self.submit(body)
        self.assertEqual((await self.call('claim',row,allow_gateway=True))[0]['gateway_route']['computer_id'],self.target)

    async def test_exposed_legacy_collision_ids_remain_visible_and_revoke_physical_machine(self):
        direct=await self.call('node',self.workspace,'Legacy remote')
        row=await self.call('authenticate',direct['node_token'],'nodes')
        await self.call('computers',self.workspace) # old phone already saved this node id
        await self.call('heartbeat',row,snapshot(),machine_id=self.remote,peers=[])
        catalog=await self.call('computers',self.workspace)
        self.assertEqual({r['id'] for r in catalog if r['machine_id']==self.remote},{self.target,direct['node_id']})
        self.assertTrue(await self.call('revoke_computer',direct['node_id']))
        self.assertFalse(any(r['machine_id']==self.remote for r in await self.call('computers',self.workspace)))
        await self.manifest()
        await self.call('heartbeat',row,snapshot(),machine_id=self.remote,peers=[])

    async def test_revoked_peer_manifest_does_not_disable_gateway_or_other_peers(self):
        other={'route_id':identity(),'machine_id':identity(),'name':'Other','online':True}
        peers=self.peers+[other]
        await self.manifest(peers)
        await self.call('peer_heartbeat',self.node,other['route_id'],other['machine_id'],snapshot())
        queued=self.command();await self.submit(queued)
        await self.call('revoke_computer',self.target)
        # The connector keeps publishing its unchanged enabled Config.peers.
        client=TestClient(TestServer(create_app(self.store,Config(background=False))))
        await client.start_server()
        headers={'Authorization':'Bearer '+self.gateway['node_token']}
        try:
            for _ in range(2):
                response=await client.post('/v1/node/heartbeat',json={'snapshot':snapshot(),'machine_id':self.local,'peers':peers},headers=headers)
                self.assertEqual(response.status,200,await response.text())
                await self.call('peer_heartbeat',self.node,other['route_id'],other['machine_id'],snapshot())
            catalog=await self.call('computers',self.workspace)
            self.assertEqual({r['machine_id'] for r in catalog if r['available']},{self.local,other['machine_id']})
            self.assertFalse(any(r['machine_id']==self.remote for r in catalog))
            self.assertEqual((await self.submit(queued))[1]['state'],'failed')
            self.assertEqual((await self.submit(self.command()))[0],409)
            with self.assertRaises(RouteError):await self.call('peer_heartbeat',self.node,self.route,self.remote,snapshot())
            self.assertEqual(await self.call('claim',self.node,allow_gateway=True),[])
        finally:await client.close()

    async def test_root_computer_revoke_preserves_gateway_credential_and_peer_routes(self):
        local=self.command(self.gateway['node_id']);await self.submit(local)
        await self.call('revoke_computer',self.gateway['node_id'])
        for _ in range(2):
            await self.manifest()
            await self.call('peer_heartbeat',self.node,self.route,self.remote,snapshot())
        catalog=await self.call('computers',self.workspace)
        self.assertEqual({r['machine_id'] for r in catalog if r['available']},{self.remote})
        self.assertTrue(await self.call('authorized',self.node['id'],'nodes'))
        self.assertEqual((await self.submit(local))[1]['state'],'failed')
        self.assertEqual((await self.submit(self.command(self.gateway['node_id'])))[0],409)
        body=self.command();self.assertEqual((await self.submit(body))[0],202)
        self.assertEqual((await self.call('claim',self.node,allow_gateway=True))[0]['request_id'],body['request_id'])
        with self.assertRaises(RouteError):await self.call('heartbeat',self.node,snapshot(),machine_id=identity(),peers=self.peers)

    async def test_revoked_root_does_not_publish_new_local_snapshot_events_or_pushes(self):
        await self.call('register_push',self.device['id'],'fcm','synthetic-target')
        before=(await self.sql('SELECT snapshot FROM nodes WHERE id=?',self.node['id']))[0]['snapshot']
        await self.call('revoke_computer',self.gateway['node_id'])
        await self.manifest(snap=snapshot(name='private-revoked-session',phase='input'))
        self.assertEqual((await self.sql('SELECT snapshot FROM nodes WHERE id=?',self.node['id']))[0]['snapshot'],before)
        self.assertEqual(await self.call('events',self.workspace,0),[])
        self.assertEqual(await self.sql('SELECT id FROM push_jobs'),[])
        await self.call('peer_heartbeat',self.node,self.route,self.remote,snapshot(phase='input'))
        events=await self.call('events',self.workspace,0)
        self.assertEqual([(e['node_id'],e['kind']) for e in events],[(self.target,'attention')])
        self.assertEqual(len(await self.sql('SELECT id FROM push_jobs')),1)

    async def test_gateway_revoke_preserves_independent_direct_route(self):
        direct=await self.call('node',self.workspace,'Direct remote');row=await self.call('authenticate',direct['node_token'],'nodes')
        await self.call('heartbeat',row,snapshot(),machine_id=self.remote,peers=[])
        await self.call('revoke','nodes',self.node['id'])
        body=self.command();self.assertEqual((await self.submit(body))[0],202)
        self.assertEqual(len(await self.call('claim',row,allow_gateway=True)),1)

    async def test_immutable_root_binding_and_authorized_peer_metadata(self):
        with self.assertRaises(RouteError):await self.call('heartbeat',self.node,snapshot(),machine_id=identity(),peers=[])
        with self.assertRaises(RouteError):await self.call('peer_heartbeat',self.node,self.route,identity(),snapshot())
        with self.assertRaises(RouteError):await self.call('peer_heartbeat',self.node,identity(),self.remote,snapshot())
        await self.manifest([])
        with self.assertRaises(RouteError):await self.call('peer_heartbeat',self.node,self.route,self.remote,snapshot())

    async def test_peer_events_use_target_computer_and_keep_event_accounting(self):
        await self.call('peer_heartbeat',self.node,self.route,self.remote,snapshot(phase='input'))
        events=await self.call('events',self.workspace,0)
        self.assertEqual([(e['node_id'],e['kind']) for e in events],[(self.target,'attention')])
        await self.call('peer_heartbeat',self.node,self.route,self.remote,snapshot(phase='input'))
        self.assertEqual(len(await self.call('events',self.workspace,0)),1)
        if hasattr(self.store,'pool'):
            self.assertEqual((await self.sql('SELECT events FROM workspace_usage WHERE workspace_id=?',self.workspace))[0]['events'],1)
            self.assertEqual((await self.sql('SELECT events FROM global_usage WHERE id=1'))[0]['events'],1)

    async def test_revoke_using_attached_node_alias_blocks_existing_peer_machine(self):
        direct=await self.call('node',self.workspace,'Direct');row=await self.call('authenticate',direct['node_token'],'nodes')
        await self.call('heartbeat',row,snapshot(),machine_id=self.remote,peers=[])
        self.assertTrue(await self.call('revoke_computer',direct['node_id']))
        self.assertFalse(any(r['machine_id']==self.remote for r in await self.call('computers',self.workspace)))

    async def test_attached_direct_events_use_visible_computer_identity(self):
        direct=await self.call('node',self.workspace,'Direct');row=await self.call('authenticate',direct['node_token'],'nodes')
        await self.call('heartbeat',row,snapshot(),machine_id=self.remote,peers=[])
        await self.call('heartbeat',row,snapshot(phase='input'),machine_id=self.remote,peers=[])
        events=await self.call('events',self.workspace,0)
        self.assertEqual([(e['node_id'],e['kind']) for e in events],[(self.target,'attention')])

    async def test_catalog_rejects_selected_payload_aggregate_before_fetch(self):
        # Each bounded snapshot is legal; their selected aggregate is too large.
        large=snapshot();large['padding']='x'*550000
        await self.manifest(snap=large);await self.call('peer_heartbeat',self.node,self.route,self.remote,large)
        self.assertIsNone(await self.call('computers',self.workspace))

    async def test_catalog_unicode_wire_cap_is_enforced_for_selected_peers(self):
        peers=[{'route_id':identity(),'machine_id':identity(),'name':'😀'*128,'online':True} for _ in range(20)]
        await self.manifest(peers)
        value=snapshot();value['padding']='x'*50500
        for peer in peers:await self.call('peer_heartbeat',self.node,peer['route_id'],peer['machine_id'],value)
        # Projected catalog and HTTP both count escaped display names before
        # returning an oversized phone envelope.
        with self.assertRaises(RouteError) as refusal:
            await self.call('computers',self.workspace)
        self.assertEqual(refusal.exception.status,413)
        client=TestClient(TestServer(create_app(self.store,Config(background=False))))
        await client.start_server()
        try:
            response=await client.get('/v1/computers',headers={'Authorization':'Bearer '+self.phone['device_token']})
            self.assertEqual(response.status,413,await response.text())
        finally:await client.close()

    async def test_registry_retention_ceiling_rolls_back_manifest(self):
        with patch('zerus_mobile.routes.MAX_COMPUTERS',2):
            with self.assertRaises(RouteError):
                await self.manifest(self.peers+[{'route_id':identity(),'machine_id':identity(),'name':'Extra','online':True}])
        self.assertEqual(len(await self.sql('SELECT id FROM computers')),2)
        self.assertEqual(len(await self.sql('SELECT route_id FROM computer_routes WHERE local=0')),1)

    async def test_strict_http_metadata_and_optin(self):
        client=TestClient(TestServer(create_app(self.store,Config(background=False))))
        await client.start_server()
        headers={'Authorization':'Bearer '+self.gateway['node_token']}
        try:
            for value in ({'snapshot':snapshot(),'machine_id':self.local,'peers':[dict(self.peers[0],via='ssh-alias')]},
                          {'snapshot':snapshot(),'machine_id':self.local,'peers':self.peers*33},
                          {'snapshot':snapshot(),'machine_id':'bad','peers':[]}):
                response=await client.post('/v1/node/heartbeat',json=value,headers=headers);self.assertEqual(response.status,400);await response.read()
            response=await client.get('/v1/node/requests?gateway_one_hop=ssh',headers=headers);self.assertEqual(response.status,400);await response.read()
            response=await client.post('/v1/node/peers/'+self.route+'/heartbeat',json={'machine_id':self.remote,'snapshot':snapshot(),'peers':[]},headers=headers)
            self.assertEqual(response.status,400);await response.read()
        finally:await client.close()
