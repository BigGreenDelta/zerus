"""Exact logical placement contract: no cwd/launch side effects or retry of writes."""
import copy
import hashlib
import tempfile
import unittest
import uuid
from pathlib import Path
from unittest.mock import AsyncMock, patch
from zerus_mobile import session_projects as projects
from zerus_mobile.connector import Connector, ConnectorError
from zerus_mobile.context import READ_OPERATIONS
from zerus_mobile.server import validate_request
from zerus_mobile.store import Store
from zerus_mobile.fleet import route_id

NAME="claude/example/task"
MACHINE="11111111-1111-4111-8111-111111111111"
SWARM="22222222-2222-4222-8222-222222222222"
HELP="hgs swarm session-project --json\nhgs swarm assign-session --json\n"+projects.HELP_MARKER


def request(operation="project_assignment"):
    ident=str(uuid.uuid4())
    payload=dict(request_id=ident,expected_run_id="run-one",expected_conversation_id="one",expected_machine_id=MACHINE)
    if operation=="project_assign":payload.update(swarm_id=SWARM,membership_token="a"*64,catalog_epoch="b"*64,project_id="two")
    return dict(request_id=ident,operation=operation,session=NAME,computer_id=str(uuid.uuid4()),payload=payload)


def result(req,status=None):
    mutation=req["operation"]=="project_assign"
    payload=req["payload"]
    raw=dict(schema=1,request_id=req["request_id"],status=status or ("assigned" if mutation else "ok"),name=NAME,run_id="run-one",conversation_id="one",machine_id=MACHINE,
             swarm_id=SWARM,membership_token="a"*64,catalog_epoch="b"*64,current_project_id="two" if mutation else "ungrouped")
    if mutation:raw["project_id"]=payload["project_id"]
    else:raw.update(projects=[dict(id="two",name="Два ' $(literal)",color="#123456")],next_cursor=None)
    if raw["status"] in {"failed","uncertain"}:
        raw.pop("projects",None);raw.pop("next_cursor",None);raw["error"]="Synthetic unavailable outcome"
    return raw


class SessionProjectContracts(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix="session-project-contract-")
        self.connector=Connector(dict(server_url="https://relay.example.invalid",node_token="synthetic",state_dir=self.temp.name))
        self.connector.sessions={NAME};self.connector.session_projects_supported=True
    async def asyncTearDown(self):
        self.connector.journal.close();self.temp.cleanup()
    async def test_exact_argv_no_folder_or_launch_and_duplicate_uuid_reuses_receipt(self):
        for operation in projects.OPERATIONS:
            req=request(operation)
            with patch.object(self.connector,"native",AsyncMock(return_value=result(req))) as native:
                first=await self.connector.execute(req)
                self.assertEqual(first["state"],"completed")
                self.assertEqual(await self.connector.execute(req),first)
                native.assert_awaited_once_with(["swarm","assign-session" if operation=="project_assign" else "session-project","--json"],
                    {**req["payload"],"name":NAME},timeout=10,max_output_bytes=128*1024)
                changed=copy.deepcopy(req);changed["payload"]["expected_run_id"]="foreign"
                with self.assertRaisesRegex(ConnectorError,"request_id conflicts"):
                    await self.connector.execute(changed)
                self.assertEqual(native.await_count,1)
        self.assertIn("project_assignment",READ_OPERATIONS);self.assertNotIn("project_assign",READ_OPERATIONS)
    async def test_unknown_mutation_is_never_reposted_and_read_only_check_is_separate(self):
        for failure in [TimeoutError("synthetic"),{"status":"assigned","project_id":"two"}]:
            req=request("project_assign")
            scripted=AsyncMock(side_effect=failure) if isinstance(failure,Exception) else AsyncMock(return_value=failure)
            with patch.object(self.connector,"native",scripted) as native:
                receipt=await self.connector.execute(req)
                self.assertEqual(receipt["state"],"uncertain")
                self.assertEqual(await self.connector.execute(req),receipt);self.assertEqual(native.await_count,1)
        raw=result(req,"uncertain")
        self.assertTrue(projects.result_valid(req["operation"],raw,req["payload"],NAME))
    async def test_missing_or_partial_marker_and_extra_authority_fields_never_execute(self):
        for help_text in ["",projects.HELP_MARKER,HELP.replace("swarm assign-session --json","")]:
            self.assertFalse(projects.from_help(help_text))
        self.assertTrue(projects.from_help(HELP))
        self.connector.session_projects_supported=False
        with patch.object(self.connector,"native",AsyncMock()) as native:
            self.assertEqual((await self.connector.execute(request("project_assign")))["error"],projects.UNAVAILABLE);native.assert_not_awaited()
        self.connector.session_projects_supported=True
        for field in ["directory","add_folder","project_folder_id","worktree","archive_id","agent_id","organization"]:
            req=request("project_assign");req["payload"][field]="foreign"
            with self.assertRaises(Exception):validate_request(req)
            with patch.object(self.connector,"native",AsyncMock()) as native:
                self.assertEqual((await self.connector.execute(req))["state"],"failed");native.assert_not_awaited()
    async def test_one_hop_uses_native_owned_edge_and_refuses_changed_route_before_call(self):
        fleet=self.connector.fleet
        fleet.supported=True;fleet.local_id="33333333-3333-4333-8333-333333333333"
        identity=route_id("synthetic-peer",MACHINE)
        fleet.routes={identity:dict(route_id=identity,machine_id=MACHINE,via="synthetic-peer",name="Synthetic peer",online=True)}
        context=self.connector.peer_context(MACHINE,fleet.callback(identity,MACHINE))
        context.sessions={NAME};context.snapshot_ready.set();context.session_projects_supported=True
        fleet.contexts={identity:context}
        try:
            req=request("project_assign");req["gateway_route"]=dict(schema=1,route_id=identity,computer_id=req["computer_id"],machine_id=MACHINE)
            with patch.object(self.connector,"_native",AsyncMock(return_value=result(req))) as native:
                receipt=await self.connector.execute(req);self.assertEqual(receipt["state"],"completed")
                self.assertEqual(await self.connector.execute(req),receipt);self.assertEqual(native.await_count,1)
                argv,envelope=native.call_args.args
                self.assertEqual(argv,["swarm","mobile-peer","--json"])
                self.assertEqual(envelope,dict(schema=1,via="synthetic-peer",target_machine_id=MACHINE,argv=["swarm","assign-session","--json"],payload={**req["payload"],"name":NAME}))
            for change in ["machine","computer","withdrawn"]:
                bad=request("project_assign");bad["gateway_route"]=dict(req["gateway_route"],computer_id=bad["computer_id"])
                if change=="machine":bad["gateway_route"]["machine_id"]="44444444-4444-4444-8444-444444444444"
                if change=="computer":bad["gateway_route"]["computer_id"]="44444444-4444-4444-8444-444444444444"
                if change=="withdrawn":fleet.routes={}
                with patch.object(self.connector,"_native",AsyncMock()) as native:
                    self.assertEqual((await self.connector.execute(bad))["state"],"failed");native.assert_not_awaited()
        finally:await context.accounts.close()

    def test_payload_and_result_require_full_identity_and_bounded_literal_choices(self):
        for operation in projects.OPERATIONS:
            req=request(operation);validate_request(req);projects.validate(operation,req["payload"],req["request_id"])
            raw=result(req);self.assertTrue(projects.result_valid(operation,raw,req["payload"],NAME))
            for key in ["request_id","name","run_id","conversation_id","machine_id","schema"]:
                wrong=copy.deepcopy(raw);wrong[key]="foreign"
                self.assertFalse(projects.result_valid(operation,wrong,req["payload"],NAME),key)
            for key in ["expected_machine_id","expected_run_id","expected_conversation_id"]:
                wrong=copy.deepcopy(req);del wrong["payload"][key]
                with self.assertRaises(Exception):validate_request(wrong)
            wrong=copy.deepcopy(raw);wrong["private_credentials"]="never exposed"
            self.assertFalse(projects.result_valid(operation,wrong,req["payload"],NAME))
        req=request();raw=result(req)
        for transform in [lambda r:r["projects"].append(copy.deepcopy(r["projects"][0])),lambda r:r["projects"][0].update(name="a"*321),lambda r:r["projects"][0].update(name="line\ncommand"),lambda r:r["projects"][0].update(color="red"),lambda r:r.update(next_cursor=True)]:
            wrong=copy.deepcopy(raw);transform(wrong);self.assertFalse(projects.result_valid(req["operation"],wrong,req["payload"],NAME))
    def test_new_read_class_is_additively_indexed_and_mutation_tombstones_survive(self):
        journal=self.connector.journal
        for operation in projects.OPERATIONS:
            req=request(operation);self.assertTrue(journal.claim(req["request_id"],operation,"x"))
            journal.finish(req["request_id"],dict(state="completed",result={"synthetic":True},error=None));journal.delivered(req["request_id"])
        journal.db.execute("UPDATE requests SET delivered_at=1");journal.prune(now=1000)
        self.assertEqual([row[0] for row in journal.db.execute("SELECT operation FROM requests")],["project_assign"])
        store=Store(Path(self.temp.name)/"relay.sqlite")
        try:
            workspace=store.workspace("Synthetic");node=store.node(workspace,"Synthetic")
            phone=store.pair(store.invite(workspace)["pair_code"],"Synthetic")
            ids={op:str(uuid.uuid4()) for op in projects.OPERATIONS}
            for operation,ident in ids.items():
                store.db.execute("INSERT INTO requests(id,workspace_id,device_id,node_id,operation,body_hash,body_bytes,body,state,result,created,updated) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
                    (ident,workspace,phone["device_id"],node["node_id"],operation,hashlib.sha256(b"{}").hexdigest(),2,"{}","completed","{}",1,1))
            query="SELECT id FROM requests INDEXED BY project_assignment_history_v1 WHERE operation='project_assignment' AND state IN ('completed','failed','uncertain') AND updated<? ORDER BY updated,id LIMIT ?"
            plan=[tuple(row) for row in store.db.execute("EXPLAIN QUERY PLAN "+query,(880,256))]
            self.assertTrue(any("project_assignment_history_v1" in str(row) for row in plan))
            with patch("zerus_mobile.store.time.time",return_value=1000):store.maintain()
            self.assertIsNone(store.db.execute("SELECT id FROM requests WHERE id=?",(ids["project_assignment"],)).fetchone())
            self.assertEqual(tuple(store.db.execute("SELECT body,result FROM requests WHERE id=?",(ids["project_assign"],)).fetchone()),("{}","{}"))
        finally:store.close()
