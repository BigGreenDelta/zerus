#!/usr/bin/env python3
"""Mobile creation bounds and signal cleanup using only private Git hooks/processes."""
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

ROOT=Path(__file__).resolve().parents[1]
HGS=Path(os.environ.get('HGS_TEST_BIN',ROOT/'target/debug/hgs')).resolve()
GIT=shutil.which('git')
class MobileWorktreesNative(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix='zerus-mobile-worktrees-');self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name);self.home=self.root/'home';self.home.mkdir()
        self.env={**os.environ,'HOME':str(self.home),'XDG_CONFIG_HOME':str(self.home/'.config'),'HGS_STATE_DIR':str(self.root/'state'),'HGS_SELF':'fixture','HGS_PEERS':'','GIT_CONFIG_NOSYSTEM':'1','GIT_CONFIG_GLOBAL':'/dev/null'}
        for key in ('GIT_DIR','GIT_COMMON_DIR','GIT_WORK_TREE','GIT_INDEX_FILE','GIT_CONFIG_COUNT'):self.env.pop(key,None)
        self.repo=self.root/'repo';self.repo.mkdir()
        self.git('init','-q','-b','main',str(self.repo));self.git('-C',str(self.repo),'-c','user.name=Fixture','-c','user.email=fixture@example.test','commit','--allow-empty','-qm','initial')
    def git(self,*args):return subprocess.check_output([GIT,*args],env=self.env,stderr=subprocess.PIPE).decode().strip()
    def create(self,destination='linked'):
        return [str(HGS),'__state','worktrees','create','--path',str(self.repo),'--common-dir',str((self.repo/'.git').resolve()),'--destination',str(self.root/destination),'--branch','feature','--base','HEAD','--request-id','11111111-1111-4111-8111-111111111111','--json','--mobile']
    def hook(self):
        marker=self.root/'hook.pid';hook=self.repo/'.git/hooks/post-checkout'
        hook.write_text('#!'+sys.executable+'\nimport os,time\nopen('+repr(str(marker))+',"w").write(str(os.getpid()))\ntime.sleep(60)\n');hook.chmod(0o700)
        return marker
    def await_hook(self,marker,process):
        deadline=time.monotonic()+5
        while not marker.exists() and time.monotonic()<deadline and process.poll() is None:time.sleep(.02)
        self.assertTrue(marker.exists(),'isolated hook did not start');return int(marker.read_text())
    def assert_gone(self,pid):
        deadline=time.monotonic()+3
        while time.monotonic()<deadline:
            result=subprocess.run(['ps','-o','stat=','-p',str(pid)],capture_output=True,text=True)
            if not result.stdout.strip() or result.stdout.lstrip().startswith(('Z','X')):return
            time.sleep(.02)
        self.fail('owned Git hook still alive after mobile helper finished')
    def test_mobile_deadline_cleans_slow_hook_and_preserves_partial_work(self):
        marker=self.hook();started=time.monotonic();process=subprocess.Popen(self.create(),env=self.env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        pid=self.await_hook(marker,process);out,err=process.communicate(timeout=25)
        self.assertNotEqual(process.returncode,0);self.assertLess(time.monotonic()-started,24)
        self.assert_gone(pid);self.assertTrue((self.root/'linked').is_dir());self.assertIn('feature',self.git('-C',str(self.repo),'branch','--list','feature'))
    def test_transport_hup_and_repeated_term_clean_only_owned_hook(self):
        sentinel=subprocess.Popen([sys.executable,'-c','import time;time.sleep(60)'])
        try:
            marker=self.hook();process=subprocess.Popen(self.create(),env=self.env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            pid=self.await_hook(marker,process)
            for sig in (signal.SIGHUP,signal.SIGTERM,signal.SIGTERM):
                try:process.send_signal(sig)
                except ProcessLookupError:pass
            process.communicate(timeout=4);self.assertNotEqual(process.returncode,0);self.assert_gone(pid)
            self.assertIsNone(sentinel.poll());self.assertTrue((self.root/'linked').is_dir())
        finally:sentinel.terminate();sentinel.wait(timeout=3)
    def test_parent_symlink_is_confirmed_as_canonical_destination(self):
        alias=self.root/'alias';alias.symlink_to(self.root,target_is_directory=True)
        result=subprocess.run(self.create('alias/linked'),env=self.env,capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(json.loads(result.stdout)['path'],str((self.root/'linked').resolve()))
if __name__=='__main__':unittest.main()
