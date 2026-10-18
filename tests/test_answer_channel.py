#!/usr/bin/env python3
"""The Claude answer channel process (`hgs __state answer-channel`) as Claude
runs it: anything it cannot serve ends silently, so Claude's own panel stays."""
import glob,hashlib,json,os,subprocess,tempfile,time,unittest
from pathlib import Path
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


class AnswerChannelApproval(AnswerChannelConversation):
    """A main-agent tool approval binds to its one active tool call only."""
    INPUT=dict(command='printf approved > approved.txt',description='Create a fixture file')

    def bound(self,event,uses=None):
        digest=hashlib.sha256(json.dumps(self.INPUT,separators=(',',':'),sort_keys=True).encode()).hexdigest()
        with tempfile.TemporaryDirectory(prefix='hgs-answer-channel-') as state:
            record=dict(version=1,name=self.NAME,agent='claude',run_id=self.RUN,conversation_id=self.CONVERSATION,pid=1,
                        process_start='not this process',phase='approval',pending_approval_at=20.0,
                        pending_approval=dict(hook_event_name='PermissionRequest',tool_name='Bash',tool_input=self.INPUT),
                        claude_tool_uses=uses if uses is not None else dict(toolu_1=dict(tool_name='Bash',input=digest,at=10.0)))
            path=os.path.join(state,hashlib.sha256(self.NAME.encode()).hexdigest()+'.json')
            with open(path,'w') as file:json.dump(record,file)
            values=dict(os.environ,HGS_STATE_DIR=state,HGS_CONFIG_DIR=os.path.join(state,'config'),HGS_SESSION=self.NAME,HGS_RUN_ID=self.RUN)
            for name in ('TMUX','TMUX_PANE'):values.pop(name,None)
            result=subprocess.run([str(HGS),'__state','answer-channel'],input=json.dumps(event),text=True,
                                  capture_output=True,env=values,timeout=30)
            self.assertEqual((result.returncode,result.stdout,result.stderr),(0,'',''))
            channels=glob.glob(os.path.join(state,'native_answers','*','*','waiting.json'))
            closed=glob.glob(os.path.join(state,'native_answers','*','*','closed.json'))
            read=lambda path:json.loads(Path(path).read_text())
            return [(read(path)['question_id'],read(closed[0])['reason']) for path in channels]

    def event(self,**changes):
        return dict(dict(hook_event_name='PermissionRequest',tool_name='Bash',session_id=self.CONVERSATION,tool_input=self.INPUT),**changes)

    def test_only_the_saved_conversation_binds(self):
        [(question,reason)]=self.bound(self.event())
        self.assertTrue(question.startswith('claude-approval:'))
        self.assertEqual(reason,'agent exited')
        for session in ('d2b0c6a1-1111-4222-8333-944455556666',''):
            self.assertEqual(self.bound(self.event(session_id=session)),[],session)

    def test_children_other_requests_and_ambiguous_calls_never_bind(self):
        for event in (self.event(agent_id='child'),self.event(tool_input=dict(command='rm -rf fixture')),
                      self.event(tool_name='ExitPlanMode'),self.event(tool_name='AskUserQuestion')):
            self.assertEqual(self.bound(event),[],event)
        digest=hashlib.sha256(json.dumps(self.INPUT,separators=(',',':'),sort_keys=True).encode()).hexdigest()
        twice=dict(toolu_1=dict(tool_name='Bash',input=digest,at=10.0),toolu_2=dict(tool_name='Bash',input=digest,at=11.0))
        self.assertEqual(self.bound(self.event(),twice),[],'two identical calls are ambiguous')
        self.assertEqual(self.bound(self.event(),{}),[],'no call began')
        late=dict(toolu_1=dict(tool_name='Bash',input=digest,at=30.0))
        self.assertEqual(self.bound(self.event(),late),[],'a call after the request is not its call')


if __name__=='__main__':
    unittest.main()
