#!/usr/bin/env python3
"""Selected account survives native tmux startup, pause/resume and provider fork."""
import json
import os
from pathlib import Path
import unittest
import subprocess
import test_fork
import test_pause

test_pause.HGS = Path(os.environ.get('HGS_TEST_BIN', Path(__file__).resolve().parents[1]/'target/debug/hgs')).resolve()

class AccountLifecycle(test_fork.Base):
    fork = test_fork.Forks.fork
    def setUp(self):
        original = test_pause.AGENT
        test_pause.AGENT = test_fork.AGENT.replace('"codex_home": os.environ.get("CODEX_HOME")', '"codex_home": os.environ.get("CODEX_HOME"), "api_key": os.environ.get("OPENAI_API_KEY"), "anthropic_home": os.environ.get("ANTHROPIC_CONFIG_DIR"), "anthropic_key": os.environ.get("ANTHROPIC_API_KEY")')
        if os.uname().sysname == 'Darwin':
            test_pause.AGENT = test_pause.AGENT.replace('with open(home / "argv.jsonl", "a") as f:', '''import ctypes
security = ctypes.CDLL('/System/Library/Frameworks/Security.framework/Security')
session = ctypes.c_uint32(); session_flags = ctypes.c_uint32()
assert security.SessionGetInfo(-1, ctypes.byref(session), ctypes.byref(session_flags)) == 0
with open(home / "argv.jsonl", "a") as f:''').replace('"codex_home": os.environ.get("CODEX_HOME")', '"security_session": session.value, "security_flags": session_flags.value, "codex_home": os.environ.get("CODEX_HOME")')
        try: super().setUp()
        finally: test_pause.AGENT = original
        # Simulate login-shell startup exporting the wrong account and API key.
        self.script('shell', '#!/bin/sh\nexport CODEX_HOME=/wrong-account OPENAI_API_KEY=WRONG-SHELL-KEY ANTHROPIC_CONFIG_DIR=/wrong-console ANTHROPIC_API_KEY=WRONG-SHELL-KEY\nshift\nexec /bin/bash -c "$@"\n')
    def test_account_survives_resume_and_fork_with_shell_overrides(self):
        self.hgs('account','add','work','--provider','codex','--label','Work')
        source = self.start('codex','one','--account','work','--new')
        home = str(self.root/'cfg/accounts/work')
        self.assertEqual(self.binding(source)['agent_home'],home)
        self.assertEqual(self.binding(source)['account_id'],'work')
        rows=json.loads(self.hgs('ls','--json','--local'))['sessions']
        listed=next(row for row in rows if row['name']==source)
        self.assertEqual(listed['account_id'],'work');self.assertEqual(listed['account_home'],home)
        self.hgs('account','rm','work')
        self.hgs('pause',source); self.resume(source)
        self.assertEqual(self.binding(source)['agent_home'],home)
        self.assertEqual(self.binding(source)['account_id'],'work')
        branch = self.fork(source)
        self.assertEqual(self.binding(branch)['agent_home'],home)
        self.assertEqual(self.binding(branch)['account_id'],'work')
        self.hgs('pause',branch); self.resume(branch)
        self.assertEqual(self.binding(branch)['account_id'],'work')
        for invocation in map(json.loads,(self.root/'argv.jsonl').read_text().splitlines()):
            self.assertEqual(invocation['codex_home'],home)
            self.assertIn(invocation['api_key'],('',None))
        self.hgs('codex','p','-n','one','--account','work','-d',rc=1)

    def test_claude_profile_also_isolates_global_anthropic_config(self):
        self.hgs('account','add','work','--provider','claude','--label','Work')
        source=self.start('claude','one','--account','work','--new')
        profile=str(self.root/'cfg/accounts/work'); anthropic=str(self.root/'cfg/accounts/work/anthropic')
        self.assertEqual(self.binding(source)['agent_home'],profile)
        self.hgs('pause',source); self.resume(source)
        branch=self.fork(source)
        self.hgs('pause',branch); self.resume(branch)
        for invocation in map(json.loads,(self.root/'argv.jsonl').read_text().splitlines()):
            self.assertEqual(invocation['anthropic_home'],anthropic)
            self.assertIn(invocation['anthropic_key'],('',None))
        self.assertEqual((self.root/'cfg/accounts/work/anthropic').stat().st_mode & 0o777,0o700)
        self.assertFalse((self.root/'.config/anthropic').exists())

    def test_zz_pause_timing_samples(self):
        # Temporary diagnostics for zerus-d84a: repeat the failing pauses.
        self.hgs('account','add','work','--provider','claude','--label','Work')
        source=self.start('claude','one','--account','work','--new')
        for _ in range(12):
            self.hgs('pause',source); self.resume(source)

    def test_default_changes_only_new_launches_not_existing_resume_or_fork(self):
        for id in ('work','next'):self.hgs('account','add',id,'--provider','claude')
        self.hgs('account','default','work')
        original=self.start('claude','original','--new')
        self.assertEqual(self.binding(original)['account_id'],'work')
        self.hgs('account','default','next')
        self.hgs('claude','p','-n','original','-d')
        self.assertEqual(self.binding(original)['account_id'],'work')
        self.hgs('pause',original)
        previous_run=self.binding(original)['run_id']
        self.hgs('claude','p','-n','original','-d')
        self.wait(lambda:self.binding(original).get('run_id')!=previous_run and self.binding(original).get('activity')=='idle' and not self.binding(original).get('expected_id'))
        self.assertEqual(self.binding(original)['account_id'],'work')
        branch=self.fork(original)
        self.assertEqual(self.binding(branch)['account_id'],'work')
        fresh=self.start('claude','fresh','--new')
        self.assertEqual(self.binding(fresh)['account_id'],'next')

    def test_permission_modes_start_resume_and_fork_without_baking_in_flags(self):
        self.script('shell', '#!/bin/sh\nshift\nexec /bin/bash -c "$@"\n')
        for provider,flag in [('codex','--dangerously-bypass-approvals-and-sandbox'),('claude','--permission-mode'),('kimi','--auto')]:
            account='native-'+provider
            self.hgs('account','permissions',account,'--mode','bypass')
            source=self.start(provider,'permissions')
            self.hgs('pause',source);self.resume(source)
            branch=self.fork(source,'permissions-branch')
            invocations=[v['argv'] for v in map(json.loads,(self.root/'argv.jsonl').read_text().splitlines()) if v['agent']==provider]
            self.assertTrue(invocations)
            for argv in invocations:self.assertIn(flag,argv)
            self.hgs('account','permissions',account,'--mode','provider')
            self.hgs('pause',source);self.resume(source)
            self.assertNotIn(flag,json.loads((self.root/'argv.jsonl').read_text().splitlines()[-1])['argv'])

    def test_managed_claude_inherits_user_settings_on_start_and_resume(self):
        native=self.root/'.claude/settings.json';native.parent.mkdir(exist_ok=True)
        native.write_text(json.dumps({'permissions':{'defaultMode':'bypassPermissions'},'effortLevel':'high','env':{'ANTHROPIC_API_KEY':'DO-NOT-COPY'}}))
        self.hgs('account','add','prefs','--provider','claude')
        source=self.start('claude','prefs','--account','prefs','--new')
        settings=self.root/'cfg/accounts/prefs/settings.json'
        first=json.loads(settings.read_text());self.assertEqual(first['permissions']['defaultMode'],'bypassPermissions')
        self.assertEqual(first['effortLevel'],'high');self.assertNotIn('ANTHROPIC_API_KEY',first.get('env',{}));self.assertIn('hooks',first)
        user=json.loads(native.read_text());user['permissions']['defaultMode']='default';native.write_text(json.dumps(user))
        self.hgs('pause',source);self.resume(source)
        updated=json.loads(settings.read_text());self.assertEqual(updated['permissions']['defaultMode'],'default');self.assertEqual(updated['hooks'],first['hooks'])

    @unittest.skipUnless(os.uname().sysname=='Darwin', 'macOS security sessions')
    def test_claude_joins_desktop_from_existing_remote_tmux_server(self):
        import ctypes
        security=ctypes.CDLL('/System/Library/Frameworks/Security.framework/Security')
        session=ctypes.c_uint32(); flags=ctypes.c_uint32()
        security.SessionGetInfo(-1,ctypes.byref(session),ctypes.byref(flags))
        if flags.value & 0x10:self.skipTest('Run from SSH to verify a distinct security context')
        if subprocess.run(['launchctl','print',f'gui/{os.getuid()}/com.hgdev.hgs.user-session'],capture_output=True).returncode:
            self.skipTest('Install the HGS desktop session service first')
        self.hgs('account','add','remote','--provider','codex')
        self.start('codex','server-owner','--account','remote','--new')
        rows=lambda:list(map(json.loads,(self.root/'argv.jsonl').read_text().splitlines()))
        original=rows()[-1]
        self.assertFalse(original['security_flags'] & 0x10)
        self.hgs('account','add','work','--provider','claude','--label','Work')
        source=self.start('claude','desktop','--account','work','--new')
        self.assertTrue(rows()[-1]['security_flags'] & 0x10)
        self.assertNotEqual(rows()[-1]['security_session'],original['security_session'])
        self.hgs('pause',source);self.resume(source)
        self.assertTrue(rows()[-1]['security_flags'] & 0x10)
        self.assertEqual(rows()[-1]['security_session'],rows()[-2]['security_session'])

if __name__ == '__main__': unittest.main()
