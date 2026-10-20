#!/usr/bin/env python3
"""Synthetic native ABI fixture for mobile integration; never invokes hgs/tmux."""
import json
import hashlib
import base64
import os
from pathlib import Path
import sys
import tempfile
import time

ARCHIVE_ID = "55555555-5555-4555-8555-555555555555"
SECOND_ARCHIVE_ID = "77777777-7777-4777-8777-777777777777"


def initial():
    return {
        "name": "codex/example/mobile", "agent": "codex", "project": "Example",
        "run_id": "fixture-run", "conversation_id": "fixture-conversation",
        "tracked": True, "activity": "idle", "phase": "idle", "process_state": "running",
        "runtime_state": "live", "turn_started": 1.0, "interrupt_supported": False,
        "prompt": "Review the mobile connection", "pending_questions": [],
        "attachment_messages": [],
        "compact_context_supported": True, "clear_context_supported": True,
        "compact_context_request": None,
        "settings_change_supported": True, "settings_scope": "session", "settings_apply_when": "now",
        "model": "fixture-model", "effort": "medium", "model_options": [
            {"id": "fixture-model", "label": "Synthetic standard model", "effort_options": ["low", "medium", "high"]},
            {"id": "fixture-fast", "label": "Synthetic fast model", "effort_options": ["low", "medium"]}],
        "effort_options": ["low", "medium", "high"], "input_queue": None,
        "subagents": {}, "processes": {"items": []}, "terminal_supported": True,
        "events": [{"seq": 1, "type": "UserPromptSubmit", "agent_id": "", "at": 1,
                    "detail": "Review the mobile connection."}],
        "provider_messages": [{"message_id": "reply-1", "type": "AgentMessage", "at": 2,
                               "agent_id": "", "source": "codex_transcript",
                               "detail": "The secure connection is ready. Send a message to test the complete route."}],
    }


def extra_states(record):
    """Optional synthetic desktop filter rows, including a reused live name."""
    result = []
    for suffix, fields in (("attention", {"activity": "busy", "phase": "input", "attention_id": "fixture-attention"}),
                           ("working", {"activity": "busy", "phase": "working"}),
                           ("saved", {"activity": "idle", "phase": "idle", "state": "paused", "runtime_state": "paused", "process_state": "stopped"}),
                           ("stopped", {"activity": "idle", "phase": "idle", "state": "stopped", "runtime_state": "stopped", "process_state": "stopped"})):
        row = {**initial(), "name": "codex/example/" + suffix, "run_id": "fixture-" + suffix + "-run", "conversation_id": "fixture-" + suffix + "-conversation", **fields}
        row["provider_messages"] = [{"message_id": "fixture-" + suffix + "-reply", "type": "AgentMessage", "agent_id": "", "at": 2,
                                    "source": "codex_transcript", "detail": "Synthetic " + suffix + " conversation."}]
        result.append(row)
    archive = {**initial(), "name": record["name"], "state": "archived", "archive_id": ARCHIVE_ID,
               "archived_at": 3, "run_id": "fixture-archive-run", "conversation_id": "fixture-archive-conversation",
               "runtime_state": "archived", "process_state": "stopped", "resumable": True}
    archive["events"][0]["detail"] = "Synthetic archived request."
    archive["provider_messages"] = [{"message_id": "fixture-archive-reply", "type": "AgentMessage", "agent_id": "", "at": 2,
                                    "source": "codex_transcript", "detail": "Synthetic archived reply; the same name also has a live conversation."}]
    result.append(archive)
    second = {**archive, "archive_id": SECOND_ARCHIVE_ID, "run_id": "fixture-second-archive-run", "conversation_id": "fixture-second-archive-conversation", "archived_at": 4,
              "events": [{"seq": 1, "type": "UserPromptSubmit", "agent_id": "", "at": 1, "detail": "Second synthetic archived request."}],
              "provider_messages": [{"message_id": "fixture-second-archive-reply", "type": "AgentMessage", "agent_id": "", "at": 2, "source": "codex_transcript", "detail": "Second synthetic archived reply; immutable archive identity selects this history."}]}
    result.append(second)
    return result


def inspection(record, config):
    """Presentation-only delay/history; never changes the fixture session."""
    delay = min(30, max(0, float(config.get("inspect_delay", "0"))))
    if delay:
        time.sleep(delay)
    count = min(5000 if config.get("_history_page") else 500, max(0, int(config.get("history_count", "0"))))
    result = {**record, "events": list(record.get("events", [])),
              "provider_messages": list(record.get("provider_messages", []))}
    existing = len(result["events"]) + len(result["provider_messages"])
    for index in range(max(0, count - existing)):
        at = 0.001 * (index + 1)
        if index % 2 == 0:
            result["events"].append({"seq": 100_000 + index, "type": "UserPromptSubmit",
                "agent_id": "", "at": at, "detail": f"Synthetic history request {index + 1}."})
        else:
            result["provider_messages"].append({"message_id": f"fixture-history-{index}",
                "type": "AgentMessage", "agent_id": "", "source": "codex_transcript", "at": at,
                "detail": f"Synthetic history reply {index + 1}. This row has a stable native message identity."})
    main = [row for row in result["events"] if row.get("type") in {"UserPromptSubmit", "UserPromptQueued", "UserMessage", "TurnStarted", "QuestionAnswered", "AgentMessage", "Stop"}
            and row.get("agent_id", "") in {"", "main"}]
    archived = bool(result.get("archive_id"))
    live = result.get("runtime_state") == "live"
    result["allowed_actions"] = (["restore", "rename", "fork", "forget"] if archived else
        ["rename", "fork", "terminate", "forget", *(["pause"] if live and result.get("activity") == "idle" else []), *(["resume", "archive"] if not live else [])])
    result["action_reasons"] = {}
    result["message_events"] = main[-100:]
    result["message_events_truncated"] = len(main) > 100
    result["message_events_limit"] = 100
    result["message_events_max_bytes"] = 256 * 1024
    tools = min(5000, max(0, int(config.get("tool_count", 0))))
    for index in range(tools):
        result["events"].append({"seq": 200_000 + index, "type": "PostToolUse", "agent_id": "",
                                 "at": 1000 + index, "tool": "Synthetic tool", "detail": "Synthetic completed tool."})
    if tools:
        result["events"] = result["events"][-100:]
    return result


def history_page(record, config, payload):
    """Synthetic canonical pages, scoped opaque cursors and honest totals."""
    source = inspection(record, {**config, 'inspect_delay': 0, 'tool_count': 0, '_history_page': True})
    streams = [('journal', source['events']), ('provider', source['provider_messages']), ('attachment', source.get('attachment_messages', []))]
    if payload.get('agent_id'):
        assert payload['agent_id'] in record['subagents']
        streams=[('journal',[{'seq':300001,'type':'AgentMessage','agent_id':'','at':2,'detail':'Synthetic subagent history.'}])]
    rows = []
    epoch = str(config.get('history_epoch') or hashlib.sha256(json.dumps([record['conversation_id'],config.get('history_count',0)]).encode()).hexdigest()[:32])
    scope = [record['conversation_id'], record.get('archive_id', ''), payload.get('agent_id', '')]
    def cursor(index):
        body = json.dumps([epoch, scope, index], separators=(',', ':')).encode()
        return base64.urlsafe_b64encode(body).decode().rstrip('=') + '.' + hashlib.sha256(body).hexdigest()
    def position(value):
        body, signature = value.split('.')
        data = base64.urlsafe_b64decode(body + '=' * (-len(body) % 4))
        assert hashlib.sha256(data).hexdigest() == signature
        saved_epoch, saved_scope, index = json.loads(data)
        assert saved_epoch == epoch and saved_scope == scope
        return index
    for stream, values in streams:
        for value in values:
            if value.get('type') not in {'UserPromptSubmit', 'UserPromptQueued', 'UserMessage', 'TurnStarted', 'QuestionAnswered', 'Stop', 'AgentMessage'}:
                continue
            row = dict(value)
            native = row.get('message_id', row.get('seq'))
            role = 'You (answer)' if row['type'] == 'QuestionAnswered' else 'You' if row['type'] in {'UserPromptSubmit', 'UserPromptQueued', 'UserMessage', 'TurnStarted'} else 'AgentMessage'
            origin = f"{stream}:{row.get('source', stream)}:{role}:native:{native}"
            row.update(history_id=hashlib.sha256(json.dumps([scope, origin]).encode()).hexdigest(), original_ids=[origin], history_stream=stream)
            rows.append(row)
    rows.sort(key=lambda row: (row.get('at', 0), row['history_id']))
    complete = not config.get('history_indexing', False) and not config.get('history_gap', False)
    total = 0
    for index, row in enumerate(rows):
        if row['type'] in {'Stop', 'AgentMessage'}:
            total += 1
            row['incoming_seq'] = total if complete else None
        else:
            row['incoming_seq'] = None
        row['history_cursor'] = cursor(index)
    limit = payload.get('limit', 100)
    if 'before' in payload:
        end = position(payload['before']); start = max(0, end-limit)
    elif 'after' in payload:
        start = position(payload['after'])+1; end = min(len(rows), start+limit)
    elif 'around' in payload:
        start = position(payload['around']); end = min(len(rows), start+limit)
    elif 'around_incoming_seq' in payload:
        assert complete and payload['history_epoch'] == epoch
        start = next((i for i,row in enumerate(rows) if (row['incoming_seq'] or 0)>payload['around_incoming_seq']), max(0,len(rows)-limit)); end = min(len(rows),start+limit)
    else:
        end = len(rows); start = max(0,end-limit)
    return {'request_id':payload['request_id'],'name':record['name'],'run_id':record['run_id'],'conversation_id':record['conversation_id']+'/'+payload['agent_id'] if payload.get('agent_id') else record['conversation_id'],
        **({'agent_id':payload['agent_id'],'parent_conversation_id':record['conversation_id']} if payload.get('agent_id') else {}),
        **({'archive_id':record['archive_id']} if record.get('archive_id') else {}), 'events':rows[start:end], 'history_epoch':epoch,
        'next_before':cursor(start) if start>0 else None,'next_after':cursor(end-1) if end<len(rows) and end>start else None,
        'has_more_before':start>0,'has_more_after':end<len(rows), 'head':{'complete':complete,'incoming_seq':total if complete else None,'total_incoming':total if complete else None},
        'indexing':bool(config.get('history_indexing',False)), 'truncated':not complete,'source_status':{'provider':'complete' if complete else 'indexing' if config.get('history_indexing') else 'partial'}}


def context_metadata(record, config):
    """Synthetic reported telemetry; defaults leave existing fixture UI quiet."""
    usage = {"status": "ok", "source": record.get("agent", "codex"), "scope": "conversation",
             "includes_subagents": False, "partial": False,
             "context": {"used": 12000, "limit": 200000},
             "totals": {"input": 12000, "output": 1000, "cache_read": 8000, "cache_write": 0},
             "last_request": {"input": 12000, "output": 1000},
             "prompt_cache": {"status": "unknown", "source": "native_usage", "observed_at": 1,
                              "input": 12000, "cache_read": 8000, "cache_write": 0}}
    first = "session_usage" not in record
    record.setdefault("session_usage", config.get("session_usage") if isinstance(config.get("session_usage"), dict) else usage)
    if first and isinstance(config.get("cache_hint"), dict):
        record["cache_hint"] = config["cache_hint"]
    if first:
        for field in ("input_queue", "subagents", "processes", "settings_apply_when", "recovery"):
            if field in config:
                record[field] = config[field]
    if config.get("context_supported") is False:
        record["compact_context_supported"] = record["clear_context_supported"] = False


def advance_context(record, config):
    request = record.get("compact_context_request")
    if not isinstance(request, dict) or request.get("status") not in {"submitted", "compacting"}:
        return False
    elapsed = time.time() - request["at"]
    delay = min(60, max(0, float(config.get("compact_delay", 2))))
    status = "compacting" if elapsed < delay else config.get("compact_status", "completed")
    if status not in {"compacting", "completed", "failed", "cancelled", "unchanged", "uncertain"}:
        raise SystemExit("invalid synthetic compaction status")
    request["status"] = status
    pending = status in {"submitted", "compacting", "uncertain"}
    record["activity"] = "busy" if pending else "idle"
    record["phase"] = "compacting" if pending else "idle"
    record["compact_context_supported"] = record["clear_context_supported"] = not pending and config.get("context_supported") is not False
    if status == "completed":
        request["completed_at"] = time.time()
        record["session_usage"]["context"]["used"] = min(4000, record["session_usage"]["context"].get("used", 4000))
        record.pop("cache_hint", None)
        record["session_usage"]["prompt_cache"] = {"status": "warm", "source": "native_usage",
            "observed_at": time.time(), "expires_at": time.time() + 300, "ttl_seconds": 300, "estimated": True}
    return True


def persist(path, record):
    with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, delete=False) as output:
        json.dump(record, output)
        temporary = Path(output.name)
    temporary.replace(path)


def account_profiles(config):
    return config.get("account_profiles", [{"id": "native-codex", "provider": "codex", "label": "Synthetic Codex",
        "installed": True, "native": True, "is_default": True, "auth_revision": "synthetic-account-revision",
        "home": "/private/never-forward-this", "credentials": "never-forward-this",
        "account_status": {"status": "ok", "checked_at": time.time() - 180,
                           "identity": {"email": "mobile@example.test", "plan": "Synthetic Pro"}}}])


def account_usage(identity, config):
    profile = next(row for row in account_profiles(config) if row["id"] == identity)
    return {"id": identity, "provider": profile["provider"], "home": "/private/never-forward-this",
            **config.get("account_usage", {}).get(identity, {"status": "ok", "checked_at": time.time(),
                "source": "Codex App Server", "auth_status": "signed_in",
                "identity": {"email": "mobile@example.test", "plan": "Synthetic Pro"},
                "windows": [{"id": "primary", "label": "5 hours", "used_percent": 42,
                             "window_minutes": 300, "resets_at": time.time() + 1800},
                            {"id": "secondary", "label": "7 days", "used_percent": 73,
                             "window_minutes": 10080, "resets_at": "2026-12-01T00:00:00Z"}]})}


def main():
    root = Path(os.environ["ZERUS_MOBILE_FIXTURE_DIR"])
    args = sys.argv[1:]
    path = root / "session.json"
    launched = [(entry, json.loads(entry.read_text())) for entry in (root / "launches").glob("*.json")]
    if len(args) >= 2:
        path = next((entry for entry, row in launched if row["name"] == args[1]), path)
    record = json.loads(path.read_text()) if path.exists() else initial()
    fixture_config = json.loads((root / "fixture-config.json").read_text()) if (root / "fixture-config.json").exists() else {}
    if fixture_config.get("pending_questions") and not record.get("fixture_question_seeded"):
        record["pending_questions"]=fixture_config["pending_questions"];record["fixture_question_seeded"]=True;record["phase"]="input";record["activity"]="busy";persist(path,record)
    context_metadata(record, fixture_config)
    if advance_context(record, fixture_config):
        persist(path, record)
    states = [row for entry,row in launched if entry != path]
    states += extra_states(record) if fixture_config.get("all_states") is True or os.environ.get("ZERUS_MOBILE_FIXTURE_ALL_STATES") == "1" or os.environ.get("ZERUS_MOBILE_FIXTURE_CATALOG_MODE") == "filters" else []
    args = sys.argv[1:]
    if args[:2] == ["__state", "worktrees"]: args=args[1:]
    if args == ["--help"]:
        print("hgs session-action <session> --json scoped native lifecycle action\nhgs --launch-id UUID\nhgs swarm assign-launch --json\nhgs terminal <session> --json\nhgs history <session> --json\nhgs recovery action --scoped-json\nMobile worktree ABI: worktrees-v1\nhgs [@host] read <session> --json mark the current reply/attention read\nhgs swarm preference set mobile_delivery VALUE --json\nhgs [@host] attachment <session> --request ID --index N --conversation ID   read a sent attachment")
        return
    if len(args) >= 8 and args[0] == "attachment" and "--stage" not in args:
        # Receipt-addressed read of the exact conversation's own staged file.
        options = dict(zip(args[2::2], args[3::2]))
        row = next((row for row in [record, *states] if row["name"] == args[1]
                    and (row.get("archive_id") == options.get("--archive") if "--archive" in options else not row.get("archive_id"))), None)
        if row is None or options.get("--conversation") != row["conversation_id"] or options.get("--agent", "") != "":
            raise SystemExit("The session conversation changed; nothing was read")
        message = next((item for item in row.get("attachment_messages", []) if item["message_id"] == options["--request"]), None)
        index = int(options["--index"])
        stored = root / "attachment-store" / f"{options['--request']}-{index}.bin"
        if message is None or index >= len(message["attachments"]) or not stored.is_file() or stored.is_symlink():
            raise SystemExit("The attachment is no longer available")
        print(json.dumps({**message["attachments"][index], "data_base64": base64.b64encode(stored.read_bytes()).decode()}))
        return
    if args == ["ls", "--json", "--local"]:
        summaries = [{key: value for key, value in row.items()
                      if key not in ("events", "provider_messages", "pending_questions", "attachment_messages")} for row in [record, *states] if not row.get("fixture_forgotten")]
        result = {"host": "Integration fixture", "ok": True, "projects": {}, "sessions": summaries,
                  "preferences": json.loads((root / "preferences.json").read_text()) if (root / "preferences.json").exists() else {"mobile_delivery": "away"}}
        for key in ("desktop_presence", "preferences"):  # Phase 2 listing fields, synthetic here.
            if isinstance(fixture_config.get(key), dict):
                result[key] = fixture_config[key]
    elif args == ["swarm", "get"]:
        node_id = "11111111-1111-4111-8111-111111111111"
        result = {"schema": 1, "initialized": True, "node_id": node_id,
                  "swarm_id": "22222222-2222-4222-8222-222222222222",
                  "machines": [{"id": node_id, "connection": "Integration fixture", "local": True}],
                  "organization": {"version": 2, "default_project": "ungrouped", "projects": [
                      {"id": "ungrouped", "name": "General", "color": "#8d9baa", "accessible": True,
                       "folders": [], "sessions": []},
                      {"id": "33333333-3333-4333-8333-333333333333", "name": "Mobile integration", "color": "#64b5f6", "accessible": True,
                       "folders": [{"id": "44444444-4444-4444-8444-444444444444", "machine_id": node_id,
                                    "machine_name": "Integration fixture", "name": "Example folder", "path": "/example/mobile"}],
                       "sessions": ["Integration fixture\n" + row["name"] for row in [record, *states] if row.get("state") != "archived"]}]}}
        if states:
            result["organization"]["projects"].append({"id": "66666666-6666-4666-8666-666666666666", "name": "Saved integration history", "color": "#9c78cf", "accessible": True,
                "folders": [], "sessions": ["Integration fixture\narchive\n" + row["archive_id"] for row in states if row.get("state") == "archived"]})
        assignments_path = root / "project-assignments.json"
        assignments = json.loads(assignments_path.read_text()) if assignments_path.exists() else {}
        for placement in assignments.values():
            if placement.get("status") != "assigned":
                continue
            membership = "Integration fixture\n" + placement["name"]
            for group in result["organization"]["projects"]:
                group["sessions"] = [name for name in group["sessions"] if name != membership]
                if group["id"] == placement["project_id"]:
                    group["sessions"].append(membership)
                    if not any(folder["id"] == placement["folder_id"] for folder in group["folders"]):
                        group["folders"].append({"id": placement["folder_id"], "machine_id": node_id,
                            "machine_name": "Integration fixture", "name": "Added folder", "path": placement["directory"]})
    elif args[:4] == ["swarm", "preference", "set", "mobile_delivery"] and len(args) == 6 and args[5] == "--json":
        assert args[4] in ("immediate", "away", "away_5", "away_10", "away_15", "away_30")
        (root / "preferences.json").write_text(json.dumps({"mobile_delivery": args[4]}))
        result = {"ok": True, "mobile_delivery": args[4]}
    elif args == ["swarm", "assign-launch", "--json"]:
        payload = json.load(sys.stdin)
        created = next(row for row in [record, *states] if row["name"] == payload["name"])
        assert created["launch_id"] == payload["request_id"] and created["run_id"] == payload["expected_run_id"]
        assert not created.get("archive_id") and (not payload["expected_conversation_id"] or created["conversation_id"] == payload["expected_conversation_id"])
        assert payload["swarm_id"] == "22222222-2222-4222-8222-222222222222"
        assert payload["project_id"] in {"ungrouped", "33333333-3333-4333-8333-333333333333"}
        result = {"request_id": payload["request_id"], "status": fixture_config.get("project_assignment_status", "assigned"),
            "name": created["name"], "run_id": created["run_id"], "conversation_id": created["conversation_id"],
            "swarm_id": payload["swarm_id"], "project_id": payload["project_id"],
            "folder_id": payload.get("project_folder_id", "fixture-folder-" + payload["request_id"]), "directory": payload["directory"]}
        assignments_path = root / "project-assignments.json"
        assignments = json.loads(assignments_path.read_text()) if assignments_path.exists() else {}
        result = assignments.setdefault(payload["request_id"], result)
        persist(assignments_path, assignments)
    elif args == ["account", "ls"]:
        result = {"profiles": account_profiles(fixture_config)}
    elif len(args) == 3 and args[:2] == ["account", "inspect"]:
        result = account_usage(args[2], fixture_config)
    elif args and args[0] == "worktrees":
        options = dict(zip(args[2::2],args[3::2])) if args[1] == "create" else {"--path": args[args.index("--path")+1]}
        trees_path=root/"worktrees.json"
        catalog=json.loads(trees_path.read_text()) if trees_path.exists() else fixture_config.get("worktrees", {"state":"not_repo","path":options["--path"],"stale":False,"worktrees":[]})
        if args[1] == "create":
            options={key:args[args.index(key)+1] for key in ("--path","--common-dir","--destination","--branch","--request-id")}
            assert catalog["state"]=="ok" and catalog["common_dir"]==options["--common-dir"]
            time.sleep(min(25,max(0,float(fixture_config.get("worktree_create_delay",0)))))
            result={"status":"created","request_id":options["--request-id"],"path":options["--destination"],"branch":options["--branch"],"common_dir":catalog["common_dir"]}
            catalog["worktrees"].append({"path":result["path"],"kind":"linked","available":True,"branch":result["branch"]});persist(trees_path,catalog)
            with (root/"mutations.jsonl").open("a") as output: output.write(json.dumps({"operation":"worktree_create","request_id":result["request_id"]})+"\n")
        else:
            requested=options["--path"]
            matching=next((tree for tree in catalog.get("worktrees",[]) if requested==tree["path"] or requested.startswith(tree["path"].rstrip('/')+'/')),None)
            result={**catalog,"path":requested} if matching else {"state":"not_repo","stale":False,"path":requested,"worktrees":[]}
    elif len(args) == 2 and args[0] == "dirs":
        directory = "/example" if args[1] == "~" else args[1]
        assert directory.startswith("/")
        result = {"path": directory, "parent": str(Path(directory).parent), "home": "/example",
                  "directories": [{"name": "mobile", "path": "/example/mobile", "symlink": False}], "truncated": False}
    elif args and args[0] in {"codex", "claude", "kimi", "dsh"} and "--launch-id" in args:
        assert len(args) in {8,10} and args[2:4] == ["--new", "-n"] and args[5:7] == ["-d", "--launch-id"]
        identity = args[7]
        name = args[0] + "/example/" + args[4]
        assert all(row["name"] != name for row in [record,*states])
        created = {**initial(), "name": name, "agent": args[0], "launch_id": identity,
                   "run_id": "fixture-launch-run-" + identity, "conversation_id": "fixture-launch-conversation-" + identity}
        directory = root / "launches"; directory.mkdir(mode=0o700, exist_ok=True)
        persist(directory / (identity + ".json"), created)
        with (root / "mutations.jsonl").open("a") as output:
            output.write(json.dumps({"operation": "launch", "request_id": identity}) + "\n")
        print("Synthetic session created")
        return
    elif len(args)==3 and args[0]=='history' and args[2]=='--json':
        payload=json.load(sys.stdin)
        selected=[row for row in [record,*states] if row['name']==args[1] and row.get('archive_id','')==payload.get('archive_id','')]
        assert len(selected)==1
        selected=selected[0]
        assert selected['run_id']==payload['expected_run_id'] and selected['conversation_id']==payload['expected_conversation_id']
        result=history_page(selected,fixture_config,payload)
    elif args==['recovery','action','--scoped-json']:
        payload=json.load(sys.stdin)
        assert payload['name']==record['name'] and payload['expected_run_id']==record['run_id'] and payload['expected_conversation_id']==record['conversation_id']
        job=record['recovery']
        assert job['id']==payload['job_id'] and job['state']=='waiting' and job['identity']==[record['run_id'],record['conversation_id'],record['model'],record.get('account_id'),record.get('host_generation')]
        if payload['action']=='cancel':job.update(state='cancelled',reason='Cancelled by you')
        else:job['due_at']=max(time.time(),job.get('not_before',0))
        persist(path,record)
        with (root/'mutations.jsonl').open('a') as output:output.write(json.dumps({'operation':'recovery-action','request_id':payload['request_id']})+'\n')
        result={'request_id':payload['request_id'],'name':record['name'],'run_id':record['run_id'],'conversation_id':record['conversation_id'],'job_id':job['id'],'action':payload['action'],'status':'cancelled' if payload['action']=='cancel' else 'scheduled','recovery':job}
    elif len(args) == 3 and args == ["terminal", record["name"], "--json"]:
        payload = json.load(sys.stdin)
        assert payload["expected_run_id"] == record["run_id"] and payload["expected_conversation_id"] == record["conversation_id"]
        assert not record.get("archive_id") and record.get("runtime_state") == "live"
        binding = hashlib.sha256((record["name"] + record["run_id"] + record["conversation_id"]).encode()).hexdigest()
        result = {"request_id": payload["request_id"], "name": record["name"], "run_id": record["run_id"],
                  "conversation_id": record["conversation_id"], "terminal_binding_id": binding}
        if payload["action"] == "snapshot":
            result.update(status="snapshot", screen="Synthetic Terminal\n" + record.get("fixture_terminal_text", ""), cols=80, rows=24,
                          cursor={"x": 0, "y": 2}, captured_at=time.time(), truncated=False,
                          input_supported=True, text_supported=True, multiline_supported=True, input_reason="")
        else:
            assert payload["action"] == "input" and payload["terminal_binding_id"] == binding and time.time() < payload["expires_at"] <= time.time()+6
            with (root / "mutations.jsonl").open("a") as output:
                output.write(json.dumps({"operation": "terminal-input", "request_id": payload["request_id"]}) + "\n")
            record["fixture_terminal_text"] = payload.get("text", payload.get("key", ""))
            persist(path, record)
            result.update(status="submitted")
    elif len(args) >= 2 and args[0] == "inspect":
        options = dict(zip(args[2::2], args[3::2]))
        archive = options.get("--archive")
        candidates = [row for row in [record, *states] if row["name"] == args[1]
                      and (row.get("archive_id") == archive if archive else not row.get("archive_id"))]
        assert len(candidates) == 1
        result = candidates[0]
        if "--agent" in options:
            agent = options["--agent"]
            assert agent in result["subagents"]
            result = {"name": result["name"], "run_id": result["run_id"], "agent_id": agent,
                      "parent_conversation_id": result["conversation_id"],
                      "conversation_id": result["conversation_id"] + "/" + agent, "read_only": True,
                      "events": [{"seq": 300001, "type": "AgentMessage", "agent_id": "", "at": 2,
                                  "detail": "Synthetic subagent history."}]}
            if archive:
                result["archive_id"] = archive
                result["state"] = "archived"
    elif len(args) >= 6 and args[:2] == ["processes", record["name"]] and args[2] in {"--output", "--stop"}:
        options = dict(zip(args[4::2], args[5::2]))
        assert options["--run"] == record["run_id"] and options["--conversation"] == record["conversation_id"]
        assert any(row["id"] == args[3] for row in record["processes"]["items"])
        result = {"id": args[3], "run_id": record["run_id"], "conversation_id": record["conversation_id"],
                  "status": "requested" if args[2] == "--stop" else "running"}
        if args[2] == "--output":
            result.update(output="Synthetic process output.", truncated=False)
        else:
            assert "--archive" not in options
            with (root / "mutations.jsonl").open("a") as output:
                output.write(json.dumps({"operation": "process-stop", "process_id": args[3]}) + "\n")
    elif len(args) == 3 and args[1:] == [record["name"], "--json"] and args[0] in {"send-now", "settings"}:
        payload = json.load(sys.stdin)
        assert payload["expected_run_id"] == record["run_id"] and payload["expected_conversation_id"] == record["conversation_id"]
        result = {"request_id": payload["request_id"], "name": record["name"], "run_id": record["run_id"],
                  "conversation_id": record["conversation_id"]}
        with (root / "mutations.jsonl").open("a") as output:
            output.write(json.dumps({"operation": args[0], "payload": payload}) + "\n")
        if args[0] == "send-now":
            assert record["input_queue"]["id"] == payload["queue_id"] and record["input_queue"]["can_send_now"] is True
            record["input_queue"] = None
            result.update(status="submitted", queue_id=payload["queue_id"])
        else:
            assert record["settings_change_supported"] is True
            if "expected_pending_id" in payload:
                assert record.get("pending_settings_id") == payload["expected_pending_id"]
            result.update(model=payload.get("model", record["model"]), effort=payload.get("effort", record["effort"]),
                          status="scheduled" if record.get("settings_apply_when") in {"ready", "resume"} else "applied")
            if result["status"] == "scheduled":
                pending_id = payload.get("expected_pending_id", payload["request_id"])
                record.update(pending_settings_id=pending_id, pending_model=result["model"], pending_effort=result["effort"])
                result.update(pending_settings_id=pending_id, apply_when=record.get("settings_apply_when"))
            else:
                record.update(model=result["model"], effort=result["effort"])
                for field in ("pending_model", "pending_effort", "pending_settings_id"):
                    record.pop(field, None)
        persist(path, record)
    elif len(args) == 3 and args[0] == "session-action" and args[2] == "--json":
        payload = json.load(sys.stdin)
        candidates = [row for row in [record, *states] if row["name"] == args[1]
                      and row.get("archive_id", "") == payload.get("archive_id", "")]
        assert len(candidates) == 1
        selected = candidates[0]
        assert payload["expected_run_id"] == selected["run_id"] and payload["expected_conversation_id"] == selected["conversation_id"]
        assert selected is record, "Synthetic lifecycle mutations currently require the main fixture row"
        action = payload["action"]
        result = {"request_id": payload["request_id"], "name": args[1], "run_id": selected["run_id"],
                  "conversation_id": selected["conversation_id"], "status": "completed"}
        with (root / "mutations.jsonl").open("a") as output:
            output.write(json.dumps({"operation": action, "payload": payload}) + "\n")
        if action == "pause":
            record.update(state="paused", runtime_state="paused", process_state="stopped", activity="idle", phase="idle")
        elif action in {"resume", "restore"}:
            record.update(run_id="fixture-resume-" + payload["request_id"], runtime_state="live", process_state="running", activity="idle", phase="idle")
            for field in ("state", "archive_id", "archived_at"):
                record.pop(field, None)
        elif action in {"archive", "terminate"}:
            record.update(state="archived", archive_id=payload["request_id"], archived_at=time.time(), runtime_state="archived", process_state="stopped")
        elif action == "rename":
            record["name"] = payload["new_name"]
        elif action == "forget":
            record["fixture_forgotten"] = True
        elif action == "fork":
            pass  # Native Codex cannot authoritatively know its new conversation at handoff.
        else:
            raise AssertionError("Unknown fixture lifecycle action")
        if action not in {"forget", "fork"}:
            result["result_target"] = {field: record[field] for field in ("name", "run_id", "conversation_id", "archive_id") if field in record}
        persist(path, record)
    elif len(args) == 3 and args[1:] == [record["name"], "--json"] and args[0] in {"compact-context", "clear-context"}:
        payload = json.load(sys.stdin)
        assert set(payload) == {"request_id", "expected_run_id", "expected_conversation_id"}
        assert payload["expected_run_id"] == record["run_id"]
        assert payload["expected_conversation_id"] == record["conversation_id"]
        assert record[args[0].replace("-", "_") + "_supported"] is True
        result = {"request_id": payload["request_id"], "name": record["name"], "run_id": record["run_id"],
                  "conversation_id": record["conversation_id"], "status": "submitted"}
        with (root / "mutations.jsonl").open("a") as output:
            output.write(json.dumps({"operation": args[0], "payload": payload}) + "\n")
        if args[0] == "compact-context":
            record["compact_context_request"] = {**result, "at": time.time()}
            record["phase"] = "compacting"
            record["activity"] = "busy"
            record["compact_context_supported"] = record["clear_context_supported"] = False
        else:
            result["status"] = fixture_config.get("clear_status", "confirmed")
            assert result["status"] in {"submitted", "confirmed"}
            if result["status"] == "confirmed":
                record["conversation_id"] = "fixture-cleared-" + payload["request_id"]
                record["events"] = record["provider_messages"] = record["attachment_messages"] = []
                record["compact_context_request"] = None
                record["session_usage"]["context"]["used"] = 0
                record["session_usage"]["prompt_cache"] = {"status": "unknown", "source": "native_usage"}
                record.pop("cache_hint", None)
        persist(path, record)
        if fixture_config.get("context_failure_after_submit") is True:
            raise SystemExit("synthetic context command stopped after submission")
    elif len(args) == 3 and args[1:] == [record["name"], "--json"] and args[0] in {"send", "answer", "interrupt"}:
        payload = json.load(sys.stdin)
        assert payload["expected_run_id"] == record["run_id"]
        assert payload["expected_conversation_id"] == record["conversation_id"]
        if payload.get("expected_compaction_id"):
            compact = record["compact_context_request"]
            assert compact["request_id"] == payload["expected_compaction_id"]
            assert compact["run_id"] == record["run_id"] and compact["conversation_id"] == record["conversation_id"]
            assert compact["status"] == "completed" and record["activity"] == record["phase"] == "idle"
        if args[0] == "answer":
            question = record["pending_questions"][0]
            assert payload["question_id"] == question["question_id"]
            assert payload["expected_question_hash"] == question["question_hash"]
            time.sleep(min(25,max(0,float(fixture_config.get("answer_delay",0)))))
            if not fixture_config.get("answer_stale_question"): record["pending_questions"] = []
            record["phase"] = record["activity"] = "idle"
        if args[0] == "interrupt":
            assert payload["expected_turn_started"] == record["turn_started"]
            record["phase"] = "interrupted"
            record["activity"] = "idle"
            record["interrupt_supported"] = False
        with (root / "mutations.jsonl").open("a") as output:
            output.write(json.dumps({"operation": args[0], "payload": payload}) + "\n")
        if args[0] == "send":
            delay = float(fixture_config.get("send_delay", os.environ.get("ZERUS_MOBILE_FIXTURE_SEND_DELAY", "0")))
            if delay:
                time.sleep(min(30, max(0, delay)))
            at = time.time()
            files = []
            for index, attachment in enumerate(payload.get("attachments", [])):
                raw = base64.b64decode(attachment["data_base64"], validate=True)
                (root / "attachment-store").mkdir(mode=0o700, exist_ok=True)
                (root / "attachment-store" / f"{payload['request_id']}-{index}.bin").write_bytes(raw)
                files.append({"request_id": payload["request_id"], "index": index,
                              "name": attachment["name"], "mime": attachment["mime"],
                              "bytes": len(raw), "reference": attachment.get("reference", "")})
            if files:
                record.setdefault("attachment_messages", []).append({"type": "UserPromptSubmit", "source": "hgs_delivery",
                    "message_id": payload["request_id"], "agent_id": "", "at": at,
                    "detail": payload["text"], "submitted_text": payload["text"], "attachments": files})
            record["events"].append({"seq": len(record["events"]) + 1, "type": "UserPromptSubmit",
                                     "agent_id": "", "at": at, "detail": payload["text"]})
            record["provider_messages"].append({"message_id": "reply-" + payload["request_id"], "type": "AgentMessage",
                "agent_id": "", "source": "codex_transcript", "at": time.time() + 0.01,
                "detail": "Received through the Android relay: " + payload["text"]})
        persist(path, record)
        result = {"request_id": payload["request_id"], "status": "submitted", "run_id": record["run_id"],
                  "conversation_id": record["conversation_id"], "submitted_at": at if args[0] == "send" else time.time(), "text": payload.get("text", "")}
    elif len(args) == 3 and args[0] == "read" and args[2] == "--json":
        payload = json.load(sys.stdin)
        row = next((row for row in [record, *states] if row["name"] == args[1] and row.get("state") != "archived"), None)
        if row is None or fixture_config.get("read_not_found"):
            print(json.dumps({"ok": False, "error": "not_found"}))
            raise SystemExit(1)
        current = {**row, **fixture_config.get("read_current", {})}
        same = payload["run_id"] == row["run_id"] and payload["conversation_id"] == row["conversation_id"]
        applied = {field: same and payload[field] is not None and payload[field] == current.get(field)
                   for field in ("reply_id", "attention_signature")}
        with (root / "mutations.jsonl").open("a") as output:
            output.write(json.dumps({"operation": "read", "name": args[1], "payload": payload}) + "\n")
        result = {"ok": True, "applied": applied, "read": {
            "conversation_id": row["conversation_id"],
            "reply_id": payload["reply_id"] if applied["reply_id"] else None,
            "attention_signature": payload["attention_signature"] if applied["attention_signature"] else None,
            "at": time.time()} if any(applied.values()) else None}
        if fixture_config.get("read_success_exit"):
            # A broken native that prints a success receipt but fails.
            print(json.dumps(result))
            raise SystemExit(fixture_config["read_success_exit"])
    else:
        raise SystemExit("unsupported fixture operation")
    if args and args[0] == "inspect":
        result = inspection(result, fixture_config)
        if "--after" in options:
            after = int(options["--after"])
            result["events"] = [row for row in result["events"] if row.get("seq", 0) > after][:200]
        result["cursor"] = max((row.get("seq", 0) for row in result["events"]), default=0)
    print(json.dumps(result))


if __name__ == "__main__":
    main()
