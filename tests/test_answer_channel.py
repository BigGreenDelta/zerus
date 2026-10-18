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
                        process_start='not this process',phase='approval',pending_approval_at=time.time(),
                        pending_approval=dict(hook_event_name='PermissionRequest',tool_name='Bash',tool_input=self.INPUT),
                        claude_tool_uses=uses if uses is not None else dict(toolu_1=dict(tool_name='Bash',input=digest,at=time.time()-1)))
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
        now=time.time()
        twice=dict(toolu_1=dict(tool_name='Bash',input=digest,at=now-2),toolu_2=dict(tool_name='Bash',input=digest,at=now-1))
        self.assertEqual(self.bound(self.event(),twice),[],'two identical calls are ambiguous')
        self.assertEqual(self.bound(self.event(),{}),[],'no call began')
        late=dict(toolu_1=dict(tool_name='Bash',input=digest,at=time.time()+60))
        self.assertEqual(self.bound(self.event(),late),[],'a call after the request is not its call')


class AnswerChannelApprovalBinding(unittest.TestCase):
    """A hook binds to the tool call its first record shows, never to a later
    identical one; the request may be journaled after the hook starts."""
    NAME='claude/project/task';RUN='run-1';CONVERSATION='6f1f6a3e-6c55-4c55-9a55-1c2b3d4e5f60'
    INPUT=dict(command='printf same')

    def digest(self):
        return hashlib.sha256(json.dumps(self.INPUT,separators=(',',':'),sort_keys=True).encode()).hexdigest()

    def run_worker(self,first_uses,later):
        """Start a hook for one PermissionRequest on a record holding only
        `first_uses`, then 0.35 s later write `later` (uses, pending call)."""
        event=dict(hook_event_name='PermissionRequest',tool_name='Bash',session_id=self.CONVERSATION,tool_input=self.INPUT)
        with tempfile.TemporaryDirectory(prefix='hgs-answer-channel-') as state:
            path=Path(state)/(hashlib.sha256(self.NAME.encode()).hexdigest()+'.json')
            record=dict(version=1,name=self.NAME,agent='claude',run_id=self.RUN,conversation_id=self.CONVERSATION,pid=1,
                        process_start='not this process',phase='tool',claude_tool_uses=first_uses)
            def write(value):
                temporary=path.with_suffix('.new');temporary.write_text(json.dumps(value));temporary.replace(path)
            write(record)
            values=dict(os.environ,HGS_STATE_DIR=state,HGS_CONFIG_DIR=os.path.join(state,'config'),HGS_SESSION=self.NAME,HGS_RUN_ID=self.RUN)
            for name in ('TMUX','TMUX_PANE'):values.pop(name,None)
            worker=subprocess.Popen([str(HGS),'__state','answer-channel'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE,text=True,env=values)
            worker.stdin.write(json.dumps(event));worker.stdin.close();worker.stdin=None
            time.sleep(.35)
            started=worker.poll() is None
            if later is not None:
                # A call journaled now began after the hook started; a call the
                # first record already showed keeps its own start.
                uses,tool=later;at=int(time.time())
                write(dict(record,phase='approval',pending_approval=event,pending_approval_at=at+1,
                           claude_tool_uses={name:use if name in first_uses else dict(use,at=time.time())
                                             for name,use in uses.items()}))
            stdout,stderr=worker.communicate(timeout=30)
            self.assertEqual((worker.returncode,stdout,stderr),(0,'',''))
            channels=[json.loads(Path(p).read_text()) for p in glob.glob(os.path.join(state,'native_answers','*','*','waiting.json'))]
            return started,[c['question_id'] for c in channels],(at+1 if later is not None else None)

    def identity(self,tool,at):
        key=[self.RUN,self.CONVERSATION,None,1,'not this process',tool,'Bash',self.INPUT,at]
        return 'claude-approval:'+hashlib.sha256(json.dumps(key,separators=(',',':'),sort_keys=True).encode()).hexdigest()

    def use(self):
        return dict(tool_name='Bash',input=self.digest(),at=int(time.time())-1)

    def test_a_later_identical_call_is_never_adopted(self):
        started,channels,_=self.run_worker(dict(toolu_A=self.use()),(dict(toolu_B=self.use()),'toolu_B'))
        self.assertTrue(started,'the hook waited for the journal')
        self.assertEqual(channels,[],'the call that replaced A is not the one this hook asks about')

    def test_the_same_call_journaled_late_binds(self):
        started,channels,at=self.run_worker(dict(toolu_A=self.use()),(dict(toolu_A=self.use()),'toolu_A'))
        self.assertTrue(started)
        self.assertEqual(channels,[self.identity('toolu_A',at)])

    def test_no_or_two_first_calls_never_bind(self):
        for first in ({},dict(toolu_A=self.use(),toolu_A2=self.use())):
            started,channels,_=self.run_worker(first,(dict(toolu_A=self.use()),'toolu_A'))
            self.assertEqual(channels,[],first)


if __name__=='__main__':
    unittest.main()
