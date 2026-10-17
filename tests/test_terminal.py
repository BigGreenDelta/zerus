"""Raw Terminal input on private tmux only; real providers are never invoked."""
import json
import os
import subprocess
import time
import unittest
import uuid
import test_input as fixtures


@unittest.skipUnless(fixtures.TMUX, 'tmux required')
class Terminal(fixtures.InputTransport):
    # Reuse fixture setup, not the inherited ordinary-send test suite.
    def identity(self):
        return {'request_id':str(uuid.uuid4()),'expected_run_id':self.run_id,'expected_conversation_id':self.conversation_id}

    def terminal(self, payload, rc=0):
        result=subprocess.run([str(fixtures.HGS),'terminal',self.name,'--json'],input=json.dumps(payload),text=True,capture_output=True,env=self.env,timeout=10)
        self.assertEqual(result.returncode,rc,result.stderr)
        return json.loads(result.stdout) if rc==0 else result.stderr

    def snapshot(self):
        return self.terminal({**self.identity(),'action':'snapshot'})

    def input_payload(self, **values):
        return {**self.identity(),'action':'input','terminal_binding_id':self.snapshot()['terminal_binding_id'],'expires_at':time.time()+5,**values}

    def test_raw_literal_deadline_and_durable_uuid(self):
        payload=self.input_payload(text='echo "synthetic"',enter=True)
        result=self.terminal(payload);self.assertEqual(result['status'],'submitted')
        # The agent records the paste and the Enter that follows it as it reads them.
        self.wait_for(lambda:self.received().endswith(b'\r'))
        before=self.received();self.assertIn(b'echo "synthetic"',before)
        self.assertEqual(self.terminal(payload),result);self.assertEqual(self.received(),before)
        expired=self.input_payload(text='must not arrive',enter=True);expired['expires_at']=time.time()-1
        self.assertEqual(self.terminal(expired)['status'],'failed');self.assertEqual(self.received(),before)
        changed=self.input_payload(text='wrong pane',enter=True);changed['terminal_binding_id']='0'*64
        self.assertEqual(self.terminal(changed)['status'],'failed');self.assertEqual(self.received(),before)
        self.assertNotIn('hgs-terminal-',self.tmux('list-buffers'))

    def test_canonical_interactive_shell_accepts_explicit_literal_command(self):
        # Killing the only session makes the server exit, and a new-session client
        # queued on its socket meanwhile fails with "server exited unexpectedly".
        self.tmux('set-option','-s','exit-empty','off')
        self.tmux('kill-session','-t',self.name)
        self.tmux('new-session','-d','-s',self.name,'-x','100','-y','30','/bin/bash','--noprofile','--norc','-i')
        self.pane=self.tmux('display-message','-p','-t','='+self.name+':','#{pane_id}').strip()
        pid=int(self.tmux('display-message','-p','-t',self.pane,'#{pane_pid}'))
        started=subprocess.check_output(['ps','-p',str(pid),'-o','lstart='],env=dict(self.env,LC_ALL='C',TZ='UTC'),text=True).strip()
        self.record.update(pid=pid,pane=self.pane,process_start=started);self.write_record()
        self.tmux('set-option','-t','='+self.name+':','@hgs_run',self.run_id)
        destination=self.root/'shell-result'
        request=self.input_payload(text="printf '%s' 'literal ; $(no-execution)' > '"+str(destination)+"'",enter=True)
        result=self.terminal(request);self.assertEqual(result['status'],'submitted')
        self.wait_for(destination.exists);self.assertEqual(destination.read_text(),'literal ; $(no-execution)')
        self.assertEqual(self.terminal(request),result)

    def test_escaped_near_cap_and_unknown_key_validate_before_state_read(self):
        request={**self.identity(),'action':'input','terminal_binding_id':'0'*64,'expires_at':time.time()+5,'text':'"\\\t'*21000,'enter':False}
        # This valid >100KiB JSON reaches binding validation and a JSON failed
        # receipt rather than being rejected by a too-small stdin byte cap.
        self.assertGreater(len(json.dumps(request)),100000)
        self.assertEqual(self.terminal(request)['status'],'failed')
        request=self.input_payload(key='C-z');self.terminal(request,rc=1)

# Avoid duplicating the inherited send transport methods in this test module.
for _name in list(vars(fixtures.InputTransport)):
    if _name.startswith('test_'):
        setattr(Terminal,_name,None)

if __name__=='__main__':unittest.main()
