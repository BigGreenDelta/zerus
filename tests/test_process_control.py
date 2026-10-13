#!/usr/bin/env python3
"""OS process cancellation in private fake-agent trees; no live sessions/models."""
import json
import os
import signal
import subprocess
import unittest

import test_input

AGENT = r'''
import json,os,pathlib,signal,subprocess,time
root=pathlib.Path(os.environ['INPUT_FIXTURE'])
children=[]
for name in ['first','second']:
 child=subprocess.Popen(['sh','-c','trap "" TERM; sleep 300 & echo $! > "$1"; wait','sh',str(root/(name+'-child'))],start_new_session=True)
 children.append(child)
(root/'children').write_text(json.dumps([p.pid for p in children]))
(root/'agent-pid').write_text(str(os.getpid()))
try:
 while True:
  time.sleep(.1)
  for p in children:p.poll()
finally:
 for p in children:
  try:os.killpg(p.pid,signal.SIGKILL)
  except ProcessLookupError:pass
'''

class ProcessControl(unittest.TestCase):
    script=test_input.InputTransport.script
    write_record=test_input.InputTransport.write_record
    tmux=test_input.InputTransport.tmux
    wait_for=test_input.InputTransport.wait_for
    tearDown=test_input.InputTransport.tearDown
    def setUp(self):
        original=test_input.FAKE_AGENT;test_input.FAKE_AGENT=AGENT
        try:test_input.InputTransport.setUp(self)
        finally:test_input.FAKE_AGENT=original
        self.wait_for(lambda:(self.root/'children').exists())
        self.children=json.loads((self.root/'children').read_text())
        def cleanup():
            for pid in self.children:
                try:os.killpg(pid,signal.SIGKILL)
                except ProcessLookupError:pass
        self.addCleanup(cleanup)
    def cli(self,*args,ok=True):
        p=subprocess.run([str(test_input.HGS),*args],env=self.env,text=True,capture_output=True,timeout=10)
        if ok:self.assertEqual(p.returncode,0,p.stderr);return json.loads(p.stdout)
        self.assertNotEqual(p.returncode,0);return p.stderr
    def jobs(self):return [j for j in self.cli('processes',self.name)['items'] if j['source']=='process_tree']
    def test_scoped_stop_preserves_sibling_and_agent(self):
        for provider in ['claude','codex','kimi']:
            with self.subTest(provider=provider):
                self.record['agent']=provider;self.write_record()
                jobs=self.jobs();self.assertEqual({j['os_pid'] for j in jobs},set(self.children))
                self.assertTrue(all(j['capabilities']['stop'] for j in jobs))
        chosen=next(j for j in jobs if j['os_pid']==self.children[0])
        self.assertGreaterEqual(chosen['child_count'],1)
        # The shell records the sleep's PID after starting it; wait for the content.
        self.wait_for(lambda:(self.root/'first-child').exists() and (self.root/'first-child').read_text().strip())
        grandchild=int((self.root/'first-child').read_text())
        self.cli('processes',self.name,'--stop',chosen['id'],'--run','other','--conversation',self.conversation_id,ok=False)
        self.assertEqual(len(self.jobs()),2)
        self.cli('processes',self.name,'--stop','os-unrelated','--run',self.run_id,'--conversation',self.conversation_id,ok=False)
        self.assertEqual(len(self.jobs()),2)
        result=self.cli('processes',self.name,'--stop',chosen['id'],'--run',self.run_id,'--conversation',self.conversation_id)
        self.assertEqual(result['status'],'requested')
        self.wait_for(lambda:len(self.jobs())==1)
        self.assertEqual(self.jobs()[0]['os_pid'],self.children[1])
        state=subprocess.run(['ps','-p',str(grandchild),'-o','stat='],text=True,capture_output=True).stdout.strip()
        self.assertTrue(not state or state.startswith(('Z','X')),state)
        os.kill(self.record['pid'],0)
        self.cli('processes',self.name,'--stop',chosen['id'],'--run',self.run_id,'--conversation',self.conversation_id,ok=False)
        self.assertEqual(len(self.jobs()),1)
    def test_skip_processes_avoids_live_scan_and_identity_is_run_scoped(self):
        full=self.cli('inspect',self.name)
        self.assertEqual(full['processes']['live_count'],2)
        light=self.cli('inspect',self.name,'--skip-processes')
        self.assertNotIn('processes',light)
        old=self.jobs()[0]['id']
        self.record['conversation_id']='replacement-conversation';self.write_record()
        new=self.jobs()[0]['id'];self.assertNotEqual(old,new)
        self.cli('processes',self.name,'--stop',old,'--run',self.run_id,'--conversation','replacement-conversation',ok=False)
        self.assertEqual(len(self.jobs()),2)
    def test_idle_background_work_is_current_and_clears_after_os_exit(self):
        self.record.update(phase='idle', active_tools={})
        self.write_record()
        def listed():
            return next(s for s in self.cli('ls','--json','--local')['sessions'] if s['name']==self.name)
        row=listed()
        self.assertEqual(row['activity'],'idle')
        self.assertEqual(row['process_work']['count'],2)
        self.assertGreater(row['process_work']['started_at'],0)
        self.assertIn('sh',row['process_work']['command'])
        self.assertEqual(row['process_work']['started_at'],listed()['process_work']['started_at'])
        self.assertEqual(self.cli('inspect',self.name)['process_work']['count'],2)
        self.assertNotIn('process_work',self.cli('inspect',self.name,'--skip-processes'))
        for pid in self.children:os.killpg(pid,signal.SIGKILL)
        self.wait_for(lambda:listed()['process_work']['count']==0)
        os.kill(self.record['pid'],0)
        # Recorded running jobs are not live evidence after their processes end.
        self.record['shell_jobs']={'old':dict(id='old',status='running',background=True,observed=True,
            source='hooks',run_id=self.run_id,conversation_id=self.conversation_id,command='old command')}
        self.write_record()
        self.assertEqual(listed()['process_work']['count'],0)

if __name__=='__main__':unittest.main()
