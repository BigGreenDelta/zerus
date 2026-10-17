#!/usr/bin/env python3
"""The Claude answer channel process (`hgs __state answer-channel`) as Claude
runs it: anything it cannot serve ends silently, so Claude's own panel stays."""
import glob,hashlib,json,os,subprocess,tempfile,time,unittest
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


class AnswerChannelConversation(unittest.TestCase):
    """Only Claude's PermissionRequest from the saved conversation binds."""
    NAME='claude/project/task';RUN='run-1';CONVERSATION='6f1f6a3e-6c55-4c55-9a55-1c2b3d4e5f60'

    def card(self):
        items=[dict(id='q_0',question=QUESTIONS[0]['question'],header='Release',body='',other_label='',other_description='',
                    multi_select=False,allow_other=True,options=[dict(id='opt_0_'+str(i),label=o['label'],description=o['description'])
                                                                for i,o in enumerate(QUESTIONS[0]['options'])])]
        return dict(question_id='tool:toolu_1',tool_call_id='toolu_1',agent_id='main',source='hook',question_hash='hash-1',
                    run_id=self.RUN,conversation_id=self.CONVERSATION,questions=items)

    def bound(self,event):
        with tempfile.TemporaryDirectory(prefix='hgs-answer-channel-') as state:
            # The saved run's process has gone, so a bound channel closes at once.
            record=dict(version=1,name=self.NAME,agent='claude',run_id=self.RUN,conversation_id=self.CONVERSATION,
                        pid=1,process_start='not this process',pending_questions=dict(toolu_1=self.card()))
            path=os.path.join(state,hashlib.sha256(self.NAME.encode()).hexdigest()+'.json')
            with open(path,'w') as file:json.dump(record,file)
            values=dict(os.environ,HGS_STATE_DIR=state,HGS_CONFIG_DIR=os.path.join(state,'config'),HGS_SESSION=self.NAME,HGS_RUN_ID=self.RUN)
            for name in ('TMUX','TMUX_PANE'):values.pop(name,None)
            result=subprocess.run([str(HGS),'__state','answer-channel'],input=json.dumps(event),text=True,
                                  capture_output=True,env=values,timeout=30)
            self.assertEqual((result.returncode,result.stdout,result.stderr),(0,'',''))
            return glob.glob(os.path.join(state,'native_answers','*','*','waiting.json'))

    def test_only_the_saved_conversation_binds(self):
        event=dict(hook_event_name='PermissionRequest',tool_name='AskUserQuestion',session_id=self.CONVERSATION,
                   tool_input=dict(questions=QUESTIONS))
        self.assertEqual(len(self.bound(event)),1)
        for session in ('d2b0c6a1-1111-4222-8333-944455556666','',None):
            foreign=dict(event)
            if session is None:foreign.pop('session_id')
            else:foreign['session_id']=session
            self.assertEqual(self.bound(foreign),[],session)


if __name__=='__main__':
    unittest.main()
