#!/usr/bin/env python3
"""The Claude answer channel process (`hgs __state answer-channel`) as Claude
runs it: anything it cannot serve ends silently, so Claude's own panel stays."""
import json,os,subprocess,tempfile,time,unittest
from test_input import HGS

QUESTIONS=[dict(question='Which release should we prepare?',header='Release',options=[
    dict(label='Stable',description='Use stable build'),dict(label='Preview',description='Use preview build')])]


class AnswerChannelProcess(unittest.TestCase):
    def channel(self,stdin,**env):
        with tempfile.TemporaryDirectory(prefix='hgs-answer-channel-') as state:
            values=dict(os.environ,HGS_STATE_DIR=state,HGS_CONFIG_DIR=os.path.join(state,'config'))
            for name in ('HGS_SESSION','HGS_RUN_ID','TMUX','TMUX_PANE'):values.pop(name,None)
            values.update(env)
            return subprocess.run([str(HGS),'__state','answer-channel'],input=stdin,text=True,
                                  capture_output=True,env=values,timeout=30)

    def test_unrelated_or_broken_input_ends_silently(self):
        ask=json.dumps(dict(hook_event_name='PermissionRequest',tool_name='AskUserQuestion',tool_input=dict(questions=QUESTIONS)))
        for env in ({},dict(HGS_SESSION='claude/project/task',HGS_RUN_ID='run-1')):
            for stdin in ('','not json','[]',json.dumps(dict(hook_event_name='PermissionRequest',tool_name='Bash')),ask):
                started=time.monotonic();result=self.channel(stdin,**env)
                self.assertEqual((result.returncode,result.stdout,result.stderr),(0,'',''),(env,stdin))
                # Without a recorded question it waits at most its binding window.
                self.assertLess(time.monotonic()-started,10)


if __name__=='__main__':
    unittest.main()
