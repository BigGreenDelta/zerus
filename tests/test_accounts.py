#!/usr/bin/env python3
"""Native account isolation/transport integration checks. Uses only temporary credentials."""
import base64
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time
import unittest

HGS = str(Path(os.environ.get('HGS_TEST_BIN', 'target/debug/hgs')).resolve())

class Accounts(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='zerus-accounts-test-')
        self.root = Path(self.temp.name)
        self.home = self.root / 'home'; self.home.mkdir()
        self.config = self.home / '.config/hgs'; self.config.mkdir(parents=True)
        self.env = {k:v for k,v in os.environ.items() if not k.startswith('HGS_') and k not in ('CODEX_HOME','CLAUDE_CONFIG_DIR','KIMI_CODE_HOME','TMUX','TMUX_PANE')}
        self.env.update(HOME=str(self.home), HGS_CONFIG_DIR=str(self.config), HGS_STATE_DIR=str(self.root/'state'), HGS_SELF='test', HGS_PEERS='', HGS_TAB='0')
        # HGS prepends HOME/.local/bin, /opt/homebrew/bin and /usr/local/bin.
        # Put mocks in the first of those, so Homebrew can never win on macOS.
        self.bin = self.home/'.local/bin'; self.bin.mkdir(parents=True)
        for executable in ('codex','claude','kimi','dsh','ssh','security','tmux'):
            script = self.bin/executable
            script.write_text('#!/bin/sh\nexit '+('1' if executable == 'tmux' else '99')+'\n')
            script.chmod(0o755)
        self.env['PATH'] = str(self.bin) + os.pathsep + self.env['PATH']
        prepared_path = os.pathsep.join((str(self.home/'.local/bin'), '/opt/homebrew/bin', '/usr/local/bin', self.env['PATH']))
        for executable in ('codex','claude','kimi','dsh','ssh','security','tmux'):
            resolved = shutil.which(executable, path=prepared_path)
            self.assertIsNotNone(resolved)
            self.assertEqual(Path(resolved).resolve(), (self.bin/executable).resolve(),
                             'Test isolation failed: HGS could invoke a real '+executable)
    def tearDown(self): self.temp.cleanup()
    def run_hgs(self,*args,ok=True,input=None):
        p = subprocess.run([HGS,*args],env=self.env,cwd=self.root,input=input,capture_output=True,timeout=15)
        self.assertEqual(p.returncode == 0,ok,(args,p.stderr.decode()))
        return p
    def ls(self): return json.loads(self.run_hgs('account','ls').stdout)
    def add(self,id='work',provider='codex'):
        return json.loads(self.run_hgs('account','add',id,'--provider',provider,'--label','Work').stdout)
    def fake_auth(self):
        d=self.home/'.codex'; d.mkdir(); (d/'auth.json').write_text(json.dumps({'OPENAI_API_KEY':'TEMPORARY-TEST-KEY'})); return d
    def test_native_opencode_login_dry_run_never_invokes_auth(self):
        fake=self.bin
        binary=fake/"opencode";marker=self.root/"opencode-auth-marker"
        binary.write_text("#!/bin/sh\nprintf called > "+str(marker)+"\n");binary.chmod(0o700)
        previous=self.env.get("PATH","");self.env["PATH"]=str(fake)+":"+previous
        try:
            planned=self.run_hgs('account','login','native-opencode','--dry-run')
            self.assertIn(b'Connect OpenCode providers with `opencode auth login` in a terminal on this computer',planned.stdout)
            self.assertFalse(marker.exists())
            refused=self.run_hgs('account','login','native-opencode',ok=False)
            self.assertIn(b'in a terminal on this computer',refused.stderr)
            self.assertFalse(marker.exists())
        finally:self.env["PATH"]=previous

    def test_native_opencode_row_without_a_service_and_start_scope(self):
        fake=self.bin;binary=fake/"opencode"
        binary.write_text("#!/bin/sh\n[ \"$1\" = --version ] && echo v2.0.32 || exit 9\n");binary.chmod(0o700)
        previous=self.env.get("PATH","");self.env["PATH"]=str(fake)+":"+previous
        try:
            row=[p for p in self.ls()['profiles'] if p['id']=='native-opencode'][0]
            self.assertTrue(row['installed'])
            self.assertEqual(row['catalog_state'],'not_running')
            self.assertEqual(row['model_options'],[])
            binary.write_text("#!/bin/sh\n[ \"$1\" = --version ] && echo 1.4.0 || exit 9\n")
            row=[p for p in self.ls()['profiles'] if p['id']=='native-opencode'][0]
            self.assertFalse(row['installed'])
            self.assertIn('OpenCode 1.4.0 is not supported',row['install_reason'])
            refused=self.run_hgs('account','start','native-codex',ok=False)
            self.assertIn(b'Only the native OpenCode account has a service to start',refused.stderr)
        finally:self.env["PATH"]=previous

    def test_defaults_do_not_touch_agent_homes(self):
        data=self.ls(); self.assertEqual({p['provider'] for p in data['profiles'] if p['provider'] not in {'dsh','opencode'}}, {'codex','claude','kimi'})
        self.assertFalse((self.home/'.codex').exists()); self.assertFalse((self.config/'accounts.json').exists())
        self.assertNotIn('TEMPORARY',json.dumps(data))
    def test_create_private_catalog_and_profile(self):
        self.add(); self.assertEqual((self.config/'accounts.json').stat().st_mode & 0o777,0o600)
        self.assertEqual((self.config/'accounts/work').stat().st_mode & 0o777,0o700)
        self.assertNotIn('auth',json.loads((self.config/'accounts.json').read_text())['profiles']['work'])
    def test_stale_revision_and_duplicate_rejected(self):
        before=self.ls()['revision']; self.add()
        self.run_hgs('account','add','other','--provider','kimi','--revision',before,ok=False)
        self.run_hgs('account','add','work','--provider','codex',ok=False)
        self.assertEqual(len([p for p in self.ls()['profiles'] if p['provider'] not in {'dsh','opencode'}]),4)
    def test_permissions_are_revision_checked_and_machine_local(self):
        self.add();native=self.fake_auth();before=(native/'auth.json').read_bytes()
        for account in ['work','native-codex','native-claude','native-kimi','native-dsh']:
            revision=self.ls()['revision']
            data=json.loads(self.run_hgs('account','permissions',account,'--mode','bypass','--revision',revision).stdout)
            self.assertEqual(next(p for p in data['profiles'] if p['id']==account)['permission_mode'],'bypass')
            self.run_hgs('account','permissions',account,'--mode','provider','--revision',revision,ok=False)
            self.run_hgs('account','permissions',account,'--mode','invalid',ok=False)
            self.run_hgs('account','permissions',account,'--mode','provider')
            self.assertEqual(next(p for p in self.ls()['profiles'] if p['id']==account)['permission_mode'],'provider')
        self.run_hgs('account','permissions','missing','--mode','bypass',ok=False)
        self.assertEqual((native/'auth.json').read_bytes(),before)
        self.run_hgs('account','permissions','work','--mode','bypass','--dry-run')
        self.assertEqual(next(p for p in self.ls()['profiles'] if p['id']=='work')['permission_mode'],'provider')

    def test_remove_preserves_native_context(self):
        self.add(); f=self.config/'accounts/work/session.jsonl'; f.write_text('saved context')
        self.run_hgs('account','rm','work'); self.assertEqual(f.read_text(),'saved context')
        original=self.fake_auth();auth=(original/'auth.json').read_bytes()
        self.run_hgs('account','rename','native-codex','--label','Personal')
        before=self.ls()['revision']
        self.run_hgs('account','rm','native-codex','--revision',before)
        removed=self.ls();self.assertNotIn('native-codex',{p['id'] for p in removed['profiles']})
        self.assertEqual(next(p for p in removed['removed_profiles'] if p['id']=='native-codex')['label'],'Personal')
        self.assertEqual((original/'auth.json').read_bytes(),auth)
        self.run_hgs('account','restore','native-codex','--revision',before,ok=False)
        self.run_hgs('account','restore','native-codex','--revision',removed['revision'])
        self.assertEqual(next(p for p in self.ls()['profiles'] if p['id']=='native-codex')['label'],'Personal')
        self.run_hgs('account','rm','native-fake',ok=False)
        self.run_hgs('account','restore','unknown',ok=False)
        self.run_hgs('account','restore','work')
        self.assertEqual(f.read_text(),'saved context')
        self.run_hgs('account','rm','work')
        self.run_hgs('account','add','work','--provider','codex',ok=False)

    def test_defaults_are_provider_scoped_revision_checked_and_explicitly_overridden(self):
        self.add('work','claude');self.add('code','codex')
        revision=self.ls()['revision']
        selected=json.loads(self.run_hgs('account','default','work','--revision',revision).stdout)
        self.assertEqual(selected['defaults'],{'claude':'work'})
        self.assertEqual([p['id'] for p in selected['profiles'] if p['provider']=='claude' and p['is_default']],['work'])
        self.run_hgs('account','default','code','--revision',revision,ok=False)
        out=self.run_hgs('claude',str(self.root),'--new','--dry-run').stdout.decode()
        self.assertIn('CLAUDE_CONFIG_DIR='+str(self.config/'accounts/work'),out)
        self.assertIn('HGS_ACCOUNT_ID=work',out)
        native=self.run_hgs('claude',str(self.root),'--new','--account','native-claude','--dry-run').stdout.decode()
        self.assertIn('HGS_ACCOUNT_ID=native-claude',native);self.assertNotIn('accounts/work',native)
        code=self.run_hgs('codex',str(self.root),'--new','--dry-run').stdout.decode()
        self.assertNotIn('accounts/work',code)
        self.run_hgs('account','default','code','--dry-run');self.assertEqual(self.ls()['defaults'],{'claude':'work'})
        self.run_hgs('account','default','code');self.assertEqual(self.ls()['defaults'],{'claude':'work','codex':'code'})
        self.run_hgs('account','default','missing',ok=False)
        self.run_hgs('account','default','native-kimi')
        self.run_hgs('account','rm','work');self.assertNotIn('claude',self.ls()['defaults'])
        self.run_hgs('account','default','work',ok=False)
        self.run_hgs('account','restore','work');self.assertNotIn('claude',self.ls()['defaults'])
        self.run_hgs('account','default','native-claude');self.assertEqual(self.ls()['defaults']['claude'],'native-claude')
    def test_rename_preserves_ids_credentials_and_checks_revision(self):
        original=self.fake_auth(); auth=(original/'auth.json').read_bytes(); self.add()
        before=self.ls()['revision']
        result=json.loads(self.run_hgs('account','rename','native-codex','--label','Personal','--revision',before).stdout)
        native=next(p for p in result['profiles'] if p['id']=='native-codex')
        self.assertEqual(native['label'],'Personal');self.assertEqual((original/'auth.json').read_bytes(),auth)
        self.assertFalse((self.config/'accounts/native-codex').exists())
        self.run_hgs('account','rename','work','--label','Company','--revision',before,ok=False)
        self.run_hgs('account','rename','work','--label','Company')
        self.assertEqual(next(p for p in self.ls()['profiles'] if p['id']=='work')['label'],'Company')
        for bad in ['','  ','bad\nname']:
            self.run_hgs('account','rename','work','--label',bad,ok=False)
        self.run_hgs('account','rename','unknown','--label','Other',ok=False)

    def test_native_account_probe_is_scoped_sanitized_and_cached(self):
        self.add();self.env['OPENAI_API_KEY']='PARENT-SECRET'
        cmd=self.bin/'codex'
        cmd.write_text('''#!/usr/bin/env python3
import json,os,sys,pathlib
home=pathlib.Path(os.environ['CODEX_HOME'])
assert home.name=='work' and not os.environ.get('OPENAI_API_KEY')
count=home/'probes';count.write_text(str(int(count.read_text())+1) if count.exists() else '1')
for line in sys.stdin:
 r=json.loads(line);rid=r.get('id')
 if rid is None:continue
 if r['method']=='initialize':value={}
 elif r['method']=='account/read':value={'account':{'email':'work@example.test','planType':'pro','type':'chatgpt','token':'DO-NOT-EXPOSE'}}
 elif r['method']=='account/rateLimits/read':value={'rateLimits':{'primary':{'usedPercent':72,'windowDurationMins':300,'resetsAt':1900000000}}}
 else:raise RuntimeError('unexpected method')
 print(json.dumps({'id':rid,'result':value}),flush=True)
''')
        cmd.chmod(0o755)
        response=self.run_hgs('account','inspect','work');data=json.loads(response.stdout)
        self.assertNotIn(b'DO-NOT-EXPOSE',response.stdout);self.assertNotIn(b'PARENT-SECRET',response.stdout)
        self.assertEqual(data['identity']['email'],'work@example.test')
        self.assertEqual(data['windows'][0]['used_percent'],72)
        self.assertEqual(data['windows'][0]['window_minutes'],300)
        self.run_hgs('account','inspect','work');home=self.config/'accounts/work'
        self.assertEqual((home/'probes').read_text(),'1')
        catalog=self.ls(); row=next(p for p in catalog['profiles'] if p['id']=='work')
        self.assertEqual(row['account_status']['identity']['email'],'work@example.test')
        self.assertEqual(row['account_status']['windows'][0]['used_percent'],72)
        self.assertEqual(row['account_status']['windows'][0]['window_minutes'],300)
        self.assertEqual((home/'probes').read_text(),'1') # listing never contacts the provider
        self.assertNotIn('DO-NOT-EXPOSE',json.dumps(catalog))
        (home/'auth.json').write_text('{"tokens":{"access_token":"CHANGED"}}')
        self.assertIsNone(next(p for p in self.ls()['profiles'] if p['id']=='work')['account_status'])
        self.run_hgs('account','inspect','work');self.assertEqual((home/'probes').read_text(),'2')
        for f in (self.config/'account-status').glob('*.json'):
            self.assertEqual(f.stat().st_mode & 0o777,0o600)
            self.assertNotIn('DO-NOT-EXPOSE',f.read_text());self.assertNotIn('CHANGED',f.read_text())
    def test_explicit_agent_install_is_allowlisted_and_failed_download_never_runs(self):
        # Unix runs npm or the vendors' install.sh; Windows gets install.ps1 (src/accounts/install.rs).
        for provider,package,url in (('codex','@openai/codex',''),('claude','','https://claude.ai/install.sh'),
                                     ('kimi','','https://code.kimi.com/kimi-code/install.sh'),('dsh','@deepseek-ai/dsh@0.2.0-rc.2','')):
            result=json.loads(self.run_hgs('account','install',provider,'--dry-run').stdout)
            self.assertEqual(result,{'provider':provider,'package':package,'installer_url':url,'dry_run':True})
        self.run_hgs('account','install','arbitrary-command',ok=False)
        npm=self.bin/'npm'
        npm.write_text('#!/usr/bin/env python3\nimport json,pathlib,sys\npathlib.Path(__file__).with_name("npm-args").write_text(json.dumps(sys.argv[1:]))\n')
        npm.chmod(0o755)
        self.run_hgs('account','install','codex')
        self.assertEqual(json.loads((self.bin/'npm-args').read_text()),['install','-g','@openai/codex'])
        curl=self.bin/'curl'; marker=self.root/'installer-ran'; self.env['TMPDIR']=str(self.root)
        curl.write_text('#!/usr/bin/env python3\nimport pathlib,sys\np=pathlib.Path(sys.argv[sys.argv.index("--output")+1])\np.with_name("curl-url").write_text(sys.argv[-1])\np.write_text('+repr('printf installed > '+str(marker)+'\n')+')\nsys.exit(22)\n')
        curl.chmod(0o755)
        self.run_hgs('account','install','kimi',ok=False)
        self.assertFalse(marker.exists())
        curl.write_text(curl.read_text().replace('sys.exit(22)','sys.exit(0)'))
        self.run_hgs('account','install','kimi')
        self.assertEqual(marker.read_text(),'installed')
        self.run_hgs('account','install','claude')
        self.assertEqual((self.root/'curl-url').read_text(),'https://claude.ai/install.sh')
        self.assertEqual(list(self.root.glob('.tmp*')),[]) # The downloaded script is removed.

    def test_claude_signed_out_json_uses_exit_one(self):
        cmd=self.bin/'claude'
        cmd.write_text('#!/usr/bin/env python3\nimport json,sys\nprint(json.dumps({"loggedIn":False,"authMethod":"none","token":"DO-NOT-EXPOSE"}))\nsys.exit(1)\n')
        cmd.chmod(0o755)
        response=self.run_hgs('account','inspect','native-claude')
        self.assertEqual(json.loads(response.stdout)['status'],'signed_out')
        self.assertNotIn(b'DO-NOT-EXPOSE',response.stdout)
        self.assertFalse((self.home/'.claude').exists())

    def test_profile_launch_and_login_use_native_home(self):
        self.add(); self.env['OPENAI_API_KEY']='PARENT-SECRET'
        out=self.run_hgs('codex',str(self.root),'--new','-n','new','--account','work','--dry-run').stdout.decode()
        self.assertIn('CODEX_HOME='+str(self.config/'accounts/work'),out)
        self.assertIn('OPENAI_API_KEY=',out); self.assertNotIn('PARENT-SECRET',out)
        self.assertIn('exec env',out)
        login=json.loads(self.run_hgs('account','login','work','--dry-run').stdout)
        self.assertEqual(login['home'],str(self.config/'accounts/work')); self.assertEqual(login['arguments'],['login'])
        self.run_hgs('kimi',str(self.root),'--account','work','--dry-run',ok=False)
    def test_claude_continue_uses_selected_profile_history(self):
        self.add('work','claude')
        escaped=''.join(c if c.isascii() and c.isalnum() else '-' for c in str(self.root))
        directory=self.config/'accounts/work/projects'/escaped; directory.mkdir(parents=True)
        (directory/'history.jsonl').write_text('{}\n')
        out=self.run_hgs('claude',str(self.root),'-c','-n','new-name','--account','work','--dry-run').stdout.decode()
        self.assertIn('claude -c',out)
        self.assertFalse((self.home/'.claude/projects').exists())
    def test_native_login_preserves_unset_home_and_managed_claude_uses_subscription(self):
        self.fake_auth()
        provider=self.bin/'codex'
        provider.write_text('#!/usr/bin/env python3\nimport json,os\nprint(json.dumps({"home":os.environ.get("CODEX_HOME")}))\n')
        result=json.loads(self.run_hgs('account','login','native-codex').stdout)
        self.assertIsNone(result['home'])
        self.add('claude-work','claude')
        result=json.loads(self.run_hgs('account','login','claude-work','--dry-run').stdout)
        self.assertEqual(result['arguments'],['auth','login','--claudeai'])
    def claude_login_fixture(self):
        self.add('claude-work','claude')
        home=self.config/'accounts/claude-work'
        settings={'oauthAccount':{'emailAddress':'work@example.test'}, 'projects':{'/untrusted':{'hasTrustDialogAccepted':False}}, 'theme':'dark','unrelated':{'keep':True}}
        (home/'.claude.json').write_text(json.dumps(settings))
        (home/'.credentials.json').write_text('TEMPORARY-UNCHANGED-CREDENTIALS')
        (home/'settings.json').write_text('TEMPORARY-UNCHANGED-PERMISSIONS')
        (self.bin/'claude').write_text('''#!/usr/bin/env python3
import json,os,sys,pathlib
home=pathlib.Path(os.environ['CLAUDE_CONFIG_DIR'])
assert home.name=='claude-work'
assert os.environ['ANTHROPIC_CONFIG_DIR']==str(home/'anthropic')
assert not os.environ.get('ANTHROPIC_API_KEY')
for key in ['CLAUDE_CODE_OAUTH_TOKEN_FILE_DESCRIPTOR','CLAUDE_CODE_API_KEY_FILE_DESCRIPTOR','CLAUDE_CODE_OAUTH_REFRESH_TOKEN','CLAUDE_CODE_OAUTH_SCOPES','CLAUDE_SECURESTORAGE_CONFIG_DIR']:
 assert not os.environ.get(key),key
if sys.argv[1:3]==['auth','login']:
 assert sys.argv[3:]==['--claudeai']
 sys.exit(1 if (home/'fail-login').exists() else 0)
assert sys.argv[1:]==['auth','status','--json']
print(json.dumps({'loggedIn':not (home/'signed-out').exists(),'authMethod':'claude.ai'}))
''')
        return home,settings

    def test_native_claude_login_requires_verified_status(self):
        (self.bin/'claude').write_text('''#!/usr/bin/env python3
import json,os,pathlib,sys
assert not os.environ.get('CLAUDE_CONFIG_DIR')
if sys.argv[1:]==['auth','login']:sys.exit(0)
assert sys.argv[1:]==['auth','status','--json']
signed_in=(pathlib.Path.home()/'signed-in').exists()
print(json.dumps({'loggedIn':signed_in,'authMethod':'claude.ai' if signed_in else 'none'}))
sys.exit(0 if signed_in else 1)
''')
        failed=self.run_hgs('account','login','native-claude',ok=False)
        self.assertIn(b'setup is incomplete',failed.stderr)
        (self.home/'signed-in').touch()
        success=self.run_hgs('account','login','native-claude')
        self.assertIn(b'Sign-in verified',success.stderr)
        self.assertFalse((self.home/'.claude.json').exists())

    def test_claude_login_completes_setup_without_trusting_projects(self):
        home,settings=self.claude_login_fixture()
        native=self.home/'.claude.json';native.write_text('{"native":"unchanged"}')
        self.env['ANTHROPIC_API_KEY']='WRONG-PARENT-ACCOUNT'
        for key in ['CLAUDE_CODE_OAUTH_TOKEN_FILE_DESCRIPTOR','CLAUDE_CODE_API_KEY_FILE_DESCRIPTOR','CLAUDE_CODE_OAUTH_REFRESH_TOKEN','CLAUDE_CODE_OAUTH_SCOPES','CLAUDE_SECURESTORAGE_CONFIG_DIR']:
            self.env[key]='WRONG-PARENT-ACCOUNT'
        response=self.run_hgs('account','login','claude-work')
        settings['hasCompletedOnboarding']=True
        self.assertEqual(json.loads((home/'.claude.json').read_text()),settings)
        self.assertEqual((home/'.credentials.json').read_text(),'TEMPORARY-UNCHANGED-CREDENTIALS')
        self.assertEqual((home/'settings.json').read_text(),'TEMPORARY-UNCHANGED-PERMISSIONS')
        self.assertEqual(native.read_text(),'{"native":"unchanged"}')
        self.assertEqual((home/'.claude.json').stat().st_mode & 0o777,0o600)
        self.assertFalse((home/'.claude.json.lock').exists())
        self.assertNotIn(b'TEMPORARY',response.stdout+response.stderr)
        before=(home/'.claude.json').stat().st_mtime_ns
        self.run_hgs('account','login','claude-work')
        self.assertEqual((home/'.claude.json').stat().st_mtime_ns,before)

    def test_claude_failed_or_unverified_login_does_not_complete_setup(self):
        home,settings=self.claude_login_fixture()
        (home/'fail-login').touch()
        self.run_hgs('account','login','claude-work',ok=False)
        self.assertEqual(json.loads((home/'.claude.json').read_text()),settings)
        (home/'fail-login').unlink();(home/'signed-out').touch()
        result=self.run_hgs('account','login','claude-work',ok=False)
        self.assertIn(b'setup is incomplete',result.stderr)
        self.assertEqual(json.loads((home/'.claude.json').read_text()),settings)

    def test_claude_setup_preserves_corrupt_or_locked_or_linked_config(self):
        home,settings=self.claude_login_fixture();path=home/'.claude.json'
        path.write_text('broken json')
        self.run_hgs('account','login','claude-work',ok=False)
        self.assertEqual(path.read_text(),'broken json');self.assertFalse((home/'.claude.json.lock').exists())
        path.write_text(json.dumps(settings));(home/'.claude.json.lock').mkdir()
        self.run_hgs('account','login','claude-work',ok=False)
        self.assertEqual(json.loads(path.read_text()),settings);self.assertTrue((home/'.claude.json.lock').is_dir())
        (home/'.claude.json.lock').rmdir();path.unlink()
        native=self.home/'.claude.json';native.write_text(json.dumps(settings));path.symlink_to(native)
        self.run_hgs('account','login','claude-work',ok=False)
        self.assertTrue(path.is_symlink());self.assertEqual(json.loads(native.read_text()),settings)

    def test_claude_sign_in_status_does_not_depend_on_quota_availability(self):
        home,_=self.claude_login_fixture()
        result=json.loads(self.run_hgs('account','inspect','claude-work','--refresh').stdout)
        self.assertEqual(result['auth_status'],'signed_in')
        self.assertEqual(result['status'],'unavailable')
        row=next(p for p in self.ls()['profiles'] if p['id']=='claude-work')
        self.assertEqual(row['account_status']['auth_status'],'signed_in')

    @unittest.skipUnless(os.uname().sysname=='Darwin', 'macOS Keychain diagnostic')
    def test_claude_inaccessible_saved_sign_in_preserves_profile_identity(self):
        import hashlib
        home,_=self.claude_login_fixture();(home/'signed-out').touch()
        expected='Claude Code-credentials-'+hashlib.sha256(str(home).encode()).hexdigest()[:8]
        (self.bin/'security').write_text('#!/usr/bin/env python3\nimport sys\nassert sys.argv[1:]=='+repr(['find-generic-password','-s',expected])+'\nprint("metadata only")\n')
        result=self.run_hgs('account','inspect','claude-work','--refresh')
        data=json.loads(result.stdout)
        self.assertIn(data['status'],['credentials_locked','desktop_session_unavailable','credentials_unavailable'])
        self.assertEqual(data['identity']['email'],'work@example.test')
        self.assertTrue(data['identity_cached'])
        self.assertNotIn('TEMPORARY',result.stdout.decode())

    def test_account_copy_local_real_pipe(self):
        original=self.fake_auth(); (original/'config.toml').write_text('model = "private-model"\nnotify = ["do-not-copy"]\n')
        receipt=self.run_hgs('account','copy','native-codex','--to','@local','--as','copied','--label','Copy')
        self.assertNotIn(b'TEMPORARY-TEST-KEY',receipt.stdout+receipt.stderr)
        copied=self.config/'accounts/copied'; self.assertEqual((copied/'auth.json').read_bytes(),(original/'auth.json').read_bytes())
        self.assertNotIn('notify',(copied/'config.toml').read_text())
        self.assertEqual((copied/'auth.json').stat().st_mode & 0o777,0o600)
        self.run_hgs('account','copy','native-codex','--to','@local','--as','copied',ok=False)
    def test_kimi_only_auth_and_provider_config_copied(self):
        d=self.home/'.kimi-code'; (d/'credentials').mkdir(parents=True)
        (d/'config.toml').write_text('default_model="test"\nnotify=["evil"]\n[[hooks]]\ncommand="evil"\n[providers.test]\ntype="openai"\napi_key="TEMPORARY"\n[models.test]\nprovider="test"\nmodel="test"\n')
        (d/'credentials/default.json').write_text('{"token":"TEMPORARY"}')
        (d/'history.jsonl').write_text('private conversations')
        self.run_hgs('account','copy','native-kimi','--to','@local','--as','kimi-copy')
        dest=self.config/'accounts/kimi-copy'; config=(dest/'config.toml').read_text()
        self.assertIn('providers.test',config); self.assertNotIn('notify',config); self.assertNotIn('hooks',config)
        self.assertTrue((dest/'credentials/default.json').exists()); self.assertFalse((dest/'history.jsonl').exists())
    def test_import_rejects_traversal_or_unrelated_settings(self):
        for name,content in [('../escape','{}'),('config.toml','notify=["bad"]')]:
            payload=json.dumps({'version':1,'provider':'codex','files':{name:base64.b64encode(content.encode()).decode()}}).encode()
            self.run_hgs('account','_import','bad',input=payload,ok=False)
        self.assertFalse((self.config/'accounts/bad').exists())
    def test_export_rejects_external_symlink_and_claude(self):
        d=self.home/'.codex'; d.mkdir(); outside=self.root/'outside'; outside.write_text('{"token":"outside"}'); (d/'auth.json').symlink_to(outside)
        self.run_hgs('account','copy','native-codex','--to','@local','--as','bad',ok=False)
        self.run_hgs('account','copy','native-claude','--to','@local','--as','bad',ok=False)
    def test_ssh_helper_cannot_hold_transfer_pipe_open_after_parent_exits(self):
        self.env['HGS_PEERS']='remote'
        payload=json.dumps({'version':1,'provider':'codex','files':{
            'auth.json':base64.b64encode(b'{"OPENAI_API_KEY":"TEMPORARY-TEST-KEY"}').decode(),
            'config.toml':base64.b64encode(b'cli_auth_credentials_store="file"\n').decode()}})
        script=self.bin/'ssh'
        script.write_text('#!/usr/bin/env python3\nimport subprocess,sys\nsubprocess.Popen(["sleep","30"])\nprint('+repr(payload)+',flush=True)\n')
        script.chmod(0o755)
        start=time.monotonic()
        result=self.run_hgs('account','copy','native-codex','--from','remote','--to','@local','--as','copied')
        self.assertLess(time.monotonic()-start,5)
        self.assertNotIn(b'TEMPORARY-TEST-KEY',result.stdout+result.stderr)
        self.assertTrue((self.config/'accounts/copied/auth.json').exists())
    def test_escaped_ssh_helper_cannot_extend_transfer_deadline(self):
        self.env['HGS_PEERS']='remote'
        pid_file=self.root/'escaped-helper.pid'
        script=self.bin/'ssh'
        script.write_text('#!/usr/bin/env python3\nimport pathlib,subprocess\np=subprocess.Popen(["sleep","60"],start_new_session=True)\npathlib.Path('+repr(str(pid_file))+').write_text(str(p.pid))\nprint("{}",flush=True)\n')
        script.chmod(0o755)
        start=time.monotonic()
        try:
            result=subprocess.run([HGS,'account','copy','native-codex','--from','remote','--to','@local','--as','copied'],env=self.env,cwd=self.root,capture_output=True,timeout=26)
            self.assertNotEqual(result.returncode,0)
            self.assertLess(time.monotonic()-start,25)
            self.assertIn(b'timed out',result.stderr)
            self.assertFalse((self.config/'accounts/copied').exists())
        finally:
            if pid_file.exists():
                try: os.kill(int(pid_file.read_text()),signal.SIGTERM)
                except ProcessLookupError: pass
    def test_invalid_ids_are_not_paths(self):
        for id in ('../bad','native-fake','-oops','with space'):
            self.run_hgs('account','add',id,'--provider','codex',ok=False)
        self.assertFalse((self.config/'accounts').exists())


class OpenCodeUserInstall(unittest.TestCase):
    """OpenCode's official user install, ~/.opencode/bin, is found after every other
    PATH folder. The caller's PATH holds only a private folder: /usr/bin can hold a
    real OpenCode (an Arch package does), and a managed GUI runs hgs with a minimal
    PATH. hgs puts ~/.local/bin, /opt/homebrew/bin and /usr/local/bin first, so an
    OpenCode in either system folder would mask these cases."""
    def setUp(self):
        for folder in ('/opt/homebrew/bin', '/usr/local/bin'):
            if os.access(os.path.join(folder, 'opencode'), os.X_OK):
                self.skipTest('An OpenCode in ' + folder + ' precedes every folder these cases control')
        temp = tempfile.TemporaryDirectory(prefix='zerus-opencode-install-')
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.home = self.root / 'home'
        self.config = self.home / '.config/hgs'
        self.config.mkdir(parents=True)
        # The other agents and tools hgs may probe, never the real ones.
        local = self.home / '.local/bin'
        local.mkdir(parents=True)
        for executable in ('codex', 'claude', 'kimi', 'dsh', 'ssh', 'security', 'tmux'):
            (local / executable).write_text('#!/bin/sh\nexit ' + ('1' if executable == 'tmux' else '99') + '\n')
            (local / executable).chmod(0o755)
        self.caller = self.root / 'caller-bin'
        self.caller.mkdir()
        self.env = {key: value for key, value in os.environ.items()
                    if not key.startswith(('HGS_', 'OPENCODE_', 'XDG_'))
                    and key not in ('CODEX_HOME', 'CLAUDE_CONFIG_DIR', 'KIMI_CODE_HOME', 'TMUX', 'TMUX_PANE')}
        self.env.update(HOME=str(self.home), PATH=str(self.caller), HGS_CONFIG_DIR=str(self.config),
                        HGS_STATE_DIR=str(self.root / 'state'), HGS_SELF='test', HGS_PEERS='', HGS_TAB='0')
        self.user_install = self.home / '.opencode/bin'

    def fake_opencode(self, folder, version):
        """An `opencode` that logs every argument list it receives and answers only
        `--version`; any other command fails. Returns its log."""
        folder.mkdir(parents=True, exist_ok=True)
        log = folder / 'calls.log'
        (folder / 'opencode').write_text(
            '#!/bin/sh\nprintf "%s\\n" "$*" >> "' + str(log) + '"\n'
            '[ "$#" -eq 1 ] && [ "$1" = --version ] && echo "opencode v' + version + '" && exit 0\nexit 9\n')
        (folder / 'opencode').chmod(0o755)
        return log

    def accounts(self):
        run = subprocess.run([HGS, 'account', 'ls'], env=self.env, cwd=self.root, capture_output=True, text=True,
                             timeout=30)
        self.assertEqual(run.returncode, 0, run.stderr)
        return json.loads(run.stdout)

    def opencode_row(self):
        rows = [row for row in self.accounts()['profiles'] if row['id'] == 'native-opencode']
        self.assertEqual(len(rows), 1, 'no native OpenCode row')
        return rows[0]

    def assert_nothing_started(self):
        self.assertFalse((self.home / '.local/state/opencode').exists(), 'a service registration appeared')
        from test_opencode_native import owned_processes
        self.assertEqual(owned_processes(str(self.root)), [])

    def test_the_official_user_install_is_found_and_only_asked_its_version(self):
        calls = self.fake_opencode(self.user_install, '2.0.32')
        row = self.opencode_row()
        self.assertTrue(row['installed'], row.get('install_reason'))
        self.assertNotIn('install_reason', row)
        self.assertEqual((row['catalog_state'], row['model_options']), ('not_running', []))
        self.assertIn('OpenCode is not running', row['catalog_reason'])
        # Listing asked only for the version: no service, API, provider or model command.
        self.assertEqual(calls.read_text().splitlines(), ['--version'])
        self.assert_nothing_started()

    def test_without_any_opencode_there_is_no_native_row(self):
        listed = self.accounts()
        self.assertNotIn('native-opencode', [row['id'] for row in listed['profiles'] + listed['removed_profiles']])
        self.assert_nothing_started()

    def test_an_opencode_earlier_on_path_wins_over_the_user_install(self):
        user = self.fake_opencode(self.user_install, '2.0.32')
        earlier = self.fake_opencode(self.caller, '1.18.35')
        row = self.opencode_row()
        self.assertFalse(row['installed'])
        self.assertEqual(row['install_reason'], 'OpenCode 1.18.35 is not supported; install OpenCode 2.0.32 or newer 2.x')
        self.assertEqual(earlier.read_text().splitlines(), ['--version'])
        self.assertFalse(user.exists(), 'the user install was run')
        self.assert_nothing_started()

if __name__=='__main__': unittest.main()
