#!/usr/bin/env python3
"""Question transport integration. Private tmux and a local fake Kimi dialog only."""
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import time
import unittest
import uuid
import test_input as fixtures

HGS = fixtures.HGS

FAKE_QUESTION = fixtures.SIGNAL_SAFE_READ + r'''
import json, os, pathlib, signal, sys, termios, time, tty
root=pathlib.Path(os.environ['INPUT_FIXTURE'])
fd=sys.stdin.fileno(); original=termios.tcgetattr(fd); tty.setraw(fd)
os.write(1,b'\x1b[?2004h')
config=None; tab=0; selected={}; others={}; editing=False; draft=''; buffer=b''
def render():
    if config is None: return
    items=config['request']['questions']; lines=[' question','']
    if tab==len(items):
        lines+=[' Review your answer before submit','']
        for index,item in enumerate(items):
            labels=[o['label'] for i,o in enumerate(item['options']) if i in selected.get(index,set())]
            if others.get(index): labels.append(others[index])
            lines+=['  Q  '+item['question'],'  →  '+', '.join(labels),'']
        lines+=[' Ready to submit your answers?',' → [1] Submit',' [2] Cancel',' 1/2 choose · ↵ confirm']
    else:
        item=items[tab]; lines+=[' ? '+item['question'],'']
        if editing: lines+=[' Type your answer, then press Enter to save.','']
        for i,option in enumerate(item['options']):
            if item.get('multiSelect'): lines+=[' [✓] '+option['label'] if i in selected.get(tab,set()) else ' [ ] '+option['label']]
            else: lines+=[' '+('→ ' if i==0 else '')+'['+str(i+1)+'] '+option['label']]
        value=draft if editing else others.get(tab,'')
        label='Other'+(': '+value if value else '')
        if item.get('multiSelect'): lines+=[' [✓] '+label if others.get(tab) else ' [ ] '+label]
        else: lines+=[' ['+str(len(item['options'])+1)+'] '+label]
        lines+=[' type answer · ↵ save · esc cancel' if editing else ' ↑↓ select · ←/→/tab switch · esc cancel']
    lines+=['─'*75]
    os.write(1,b'\x1b[2J\x1b[H'+'\r\n'.join(lines).encode())
def reload(*args):
    global config
    config=json.loads((root/'question-config').read_text()); render()
signal.signal(signal.SIGUSR1,reload)
(root/'agent-pid').write_text(str(os.getpid()))
def advance():
    global tab
    count=len(config['request']['questions'])
    tab=next((i for i in range(count) if not selected.get(i) and not others.get(i)),count)
def handle(value):
    global tab,editing,draft
    items=config['request']['questions']
    if value==b'\t': tab=(tab+1)%(len(items)+1)
    elif value==b'\r' and editing:
        others[tab]=draft.strip(); editing=False; advance()
    elif value.isdigit() and len(value)==1 and not editing:
        choice=int(value)-1
        if tab==len(items):
            if choice==0:
                answer={item['question']:', '.join([o['label'] for i,o in enumerate(item['options']) if i in selected.get(index,set())]+([others[index]] if others.get(index) else [])) for index,item in enumerate(items)}
                if (root/'wrong-ack').exists(): answer[items[0]['question']]='Wrong answer'
                event={'type':'interaction.resolved','id':config['id'],'agentId':'main','response':{'answers':answer,'method':'number_key'},'time':int(time.time()*1000)}
                with open(config['wire'],'a') as file: file.write(json.dumps(event)+'\n')
                (root/'submitted').write_text(json.dumps(answer))
                os.write(1,b'\x1b[2J\x1b[HAgent working'); return
        elif choice==len(items[tab]['options']): editing=True; draft=others.get(tab,'')
        elif 0<=choice<len(items[tab]['options']):
            if items[tab].get('multiSelect'):
                chosen=selected.setdefault(tab,set())
                if choice in chosen: chosen.remove(choice)
                else: chosen.add(choice)
            else: selected[tab]={choice}; others.pop(tab,None); advance()
    render()
try:
    while True:
        value=read_input(fd)
        if not value: break
        with (root/'received').open('ab') as file: file.write(value)
        buffer+=value
        while buffer:
            if buffer.startswith(b'\x1b[200~'):
                end=buffer.find(b'\x1b[201~')
                if end<0: break
                if editing: draft+=buffer[6:end].decode()
                buffer=buffer[end+6:]; render()
            elif buffer[0]==27 and len(buffer)<6: break
            else:
                value,buffer=buffer[:1],buffer[1:]
                if config: handle(value)
finally: termios.tcsetattr(fd,termios.TCSANOW,original)
'''


FAKE_PLAN = fixtures.SIGNAL_SAFE_READ + r'''
import json, os, pathlib, signal, sys, termios, time, tty
root=pathlib.Path(os.environ['INPUT_FIXTURE'])
fd=sys.stdin.fileno(); original=termios.tcgetattr(fd); tty.setraw(fd)
os.write(1,b'\x1b[?2004h')
config=None; selected=0; feedback=None; buffer=b''
def choices():
    options=config['request']['display'].get('options') or [{'label':'Approve'}]
    return [option['label'] for option in options]+['Reject','Revise']
def render():
    if config is None: return
    rule='─'*75; labels=choices()
    lines=['  │ '+config['request']['display']['plan'].splitlines()[0]+' │',' '+rule,'   ▶ Ready to build with this plan?','']
    for index,label in enumerate(labels):
        row=str(index+1)+'. '+label
        if index==selected and feedback is not None: lines.append('   \x1b[1m▶ '+row+'\x1b[0m  '+feedback+'\x1b[7m \x1b[27m')
        elif index==selected: lines.append('   \x1b[1m▶ '+row+'\x1b[0m')
        else: lines.append('     '+row)
    hint='/'.join(str(index+1) for index in range(len(labels)))
    lines+=['','   Type feedback · ↵ submit.' if feedback is not None else '   ↑/↓ select · '+hint+' choose · ↵ confirm',' '+rule,' Ask When Needed plan  K3']
    os.write(1,b'\x1b[2J\x1b[H'+'\r\n'.join(lines).encode())
def reload(*args):
    global config
    config=json.loads((root/'question-config').read_text()); render()
signal.signal(signal.SIGUSR1,reload)
(root/'agent-pid').write_text(str(os.getpid()))
def resolve(label,decision,text=''):
    global config
    response={'decision':decision,'selectedLabel':label}
    if text: response['feedback']=text
    if (root/'wrong-ack').exists(): response['selectedLabel']='Reject'
    event={'type':'interaction.resolved','id':config['id'],'agentId':'main','response':response,'time':int(time.time()*1000)}
    with open(config['wire'],'a') as file: file.write(json.dumps(event)+'\n')
    (root/'submitted').write_text(json.dumps(response))
    config=None; os.write(1,b'\x1b[2J\x1b[HAgent working')
def handle(value):
    global selected,feedback
    labels=choices()
    if feedback is not None:
        if value==b'\r': resolve('Revise','rejected',feedback); feedback=None
        return
    if value.isdigit() and 0<int(value)<=len(labels):
        selected=int(value)-1
        if labels[selected]=='Revise': feedback=''
        else: resolve(labels[selected],'rejected' if labels[selected]=='Reject' else 'approved')
try:
    while True:
        value=read_input(fd)
        if not value: break
        with (root/'received').open('ab') as file: file.write(value)
        buffer+=value
        while buffer:
            if buffer.startswith(b'\x1b[200~'):
                end=buffer.find(b'\x1b[201~')
                if end<0: break
                if feedback is not None: feedback+=buffer[6:end].decode()
                buffer=buffer[end+6:]
            elif buffer[0]==27 and len(buffer)<6: break
            else:
                value,buffer=buffer[:1],buffer[1:]
                if config: handle(value)
            render()
finally: termios.tcsetattr(fd,termios.TCSANOW,original)
'''


def terminal_only(test, title):
    """An unrecognized startup chooser stays visible and read-only."""
    state=test.inspect()
    test.assertEqual(len(state['pending_questions']),1,state['pending_questions'])
    card=state['pending_questions'][0]
    test.assertEqual((card['source'],card['can_answer'],card['questions'][0]['question']),
                     ('startup_terminal_choice',False,title))
    test.assertEqual((state['attention_id'],state['phase']),(card['question_id'],'input'))
    test.assertIn('waiting for a choice in Terminal',state['status_detail'])
    payload=test.payload(question_id=card['question_id'],expected_question_hash=card['question_hash'])
    before=test.received()
    test.assertIn('refresh before answering',test.answer(payload,success=False))
    test.assertEqual(test.received(),before)
    return card


class Questions(unittest.TestCase):
    script=fixtures.InputTransport.script
    tmux=fixtures.InputTransport.tmux
    wait_for=fixtures.InputTransport.wait_for
    received=fixtures.InputTransport.received
    write_record=fixtures.InputTransport.write_record
    tearDown=fixtures.InputTransport.tearDown

    def setUp(self):
        original=fixtures.FAKE_AGENT
        try:
            fixtures.FAKE_AGENT=FAKE_QUESTION
            fixtures.InputTransport.setUp(self)
        finally: fixtures.FAKE_AGENT=original
        old_path=self.record_path
        self.tmux('rename-session','-t','='+self.name,'kimi/question-test')
        self.name='kimi/question-test'
        self.conversation_id='session_'+str(uuid.uuid4())
        self.record_path=self.state/(hashlib.sha256(self.name.encode()).hexdigest()+'.json')
        old_path.unlink()
        self.record.update(name=self.name,agent='kimi',conversation_id=self.conversation_id,
                           agent_home=str(self.root/'.kimi-code'),activity='busy',phase='input',
                           active_tools={'tool_test':{'name':'AskUserQuestion'}})
        self.write_record()
        self.wire=self.root/'.kimi-code/sessions/wd_fixture'/self.conversation_id/'agents/main/wire.jsonl'
        self.wire.parent.mkdir(parents=True)
        self.items=[dict(question='What should we build?',header='Task',options=[dict(label='A'),dict(label='B')]),
                    dict(question='Which checks?',header='Checks',multiSelect=True,options=[dict(label='Unit'),dict(label='Integration')]),
                    dict(question='Any additional detail?',header='Detail',options=[dict(label='Yes'),dict(label='No')])]
        self.event=dict(type='interaction.request',id='question_'+str(uuid.uuid4()),kind='question',agentId='main',
                        toolCallId='tool_test',request=dict(questions=self.items),time=int(time.time()*1000))
        self.wire.write_text(json.dumps(self.event)+'\n')
        (self.root/'question-config').write_text(json.dumps(dict(self.event,wire=str(self.wire))))
        os.kill(self.record['pid'],signal.SIGUSR1)
        self.wait_for(lambda:'What should we build?' in self.tmux('capture-pane','-p','-t',self.pane))
        self.card=self.inspect()['pending_questions'][0]

    def inspect(self):
        result=subprocess.run([str(HGS),'inspect',self.name],env=self.env,capture_output=True,text=True,timeout=5)
        self.assertEqual(result.returncode,0,result.stderr)
        return json.loads(result.stdout)

    def payload(self,**updates):
        result=dict(request_id=str(uuid.uuid4()),expected_run_id=self.run_id,
                    expected_conversation_id=self.conversation_id,question_id=self.card['question_id'],
                    expected_question_hash=self.card['question_hash'],answers=[
                        dict(question_id='q_0',selected_option_ids=['opt_0_1'],text=''),
                        dict(question_id='q_1',selected_option_ids=['opt_1_0','opt_1_1'],text=''),
                        dict(question_id='q_2',selected_option_ids=[],text='Привет from Zerus')])
        result.update(updates)
        return result

    def answer(self,payload=None,remote=False,success=True,timeout=15):
        result=subprocess.run([str(HGS),*(['@remote'] if remote else []),'answer',self.name,'--json'],
                              input=json.dumps(payload or self.payload()),text=True,capture_output=True,
                              env=self.env,timeout=timeout)
        if success:
            self.assertEqual(result.returncode,0,result.stderr)
            return json.loads(result.stdout)
        self.assertNotEqual(result.returncode,0)
        return result.stderr

    def hook(self,event):
        result=subprocess.run([str(HGS),'__state','hook'],env=dict(self.env,HGS_SESSION=self.name,HGS_RUN_ID=self.run_id),
                              input=json.dumps(dict(event,session_id=self.conversation_id)),text=True,capture_output=True,timeout=5)
        self.assertEqual(result.returncode,0,result.stderr)

    def test_recovers_running_native_question_without_hooks_or_input(self):
        self.assertEqual(self.card['source'],'kimi_wire')
        self.assertEqual(len(self.card['questions']),3)
        self.assertTrue(self.card['can_answer'],self.card['answer_unavailable_reason'])
        self.assertEqual(self.received(),b'')

    def test_answers_options_multi_and_other_then_confirms_native_wire_receipt(self):
        payload=self.payload()
        receipt=self.answer(payload)
        self.assertEqual(receipt['status'],'answered')
        self.assertEqual(receipt['question_hash'],self.card['question_hash'])
        self.assertEqual(json.loads((self.root/'submitted').read_text()),{
            'What should we build?':'B','Which checks?':'Unit, Integration','Any additional detail?':'Привет from Zerus'})
        self.assertIn(b'\x1b[200~'+ 'Привет from Zerus'.encode()+b'\x1b[201~\r',self.received())
        inspection=self.inspect()
        self.assertEqual(inspection['pending_questions'],[])
        answered=[event for event in inspection['events'] if event['type']=='QuestionAnswered']
        self.assertEqual(len(answered),1)
        self.assertEqual(answered[0]['question_id'],self.card['question_id'])
        self.assertIn('Which checks? → Unit, Integration',answered[0]['detail'])
        self.assertIn('Привет from Zerus',answered[0]['detail'])
        first=self.received()
        self.assertEqual(self.answer(payload),receipt)
        self.assertEqual(self.received(),first)
        self.assertEqual(sum(event['type']=='QuestionAnswered' for event in self.inspect()['events']),1)

    def test_stale_identity_hash_invalid_choices_and_commands_send_nothing(self):
        for update in (dict(expected_run_id='other'),dict(expected_conversation_id='other'),
                       dict(expected_question_hash='0'*64),dict(question_id='question_other')):
            self.answer(self.payload(**update),success=False)
        for answer in (dict(question_id='q_0',selected_option_ids=['not_an_option'],text=''),
                       dict(question_id='q_0',selected_option_ids=[],text='/exit'),
                       dict(question_id='q_0',selected_option_ids=[],text='a\nb'),
                       dict(question_id='q_0',selected_option_ids=[],text='ё'*2049)):
            payload=self.payload(); payload['answers'][0]=answer
            self.answer(payload,success=False)
        self.assertEqual(self.received(),b'')

    def test_hook_capture_and_native_resolution_remove_stale_hook_fallback(self):
        self.hook(dict(hook_event_name='PreToolUse',tool_name='AskUserQuestion',tool_call_id='tool_test',tool_input=dict(questions=self.items)))
        inspection=self.inspect()
        self.assertEqual(inspection['pending_questions'][0]['source'],'kimi_wire')
        self.assertIn('question_request',inspection['events'][-1])
        with self.wire.open('a') as file: file.write(json.dumps(dict(type='interaction.resolved',id=self.event['id'],agentId='main',response=dict(answers={})))+'\n')
        self.assertEqual(self.inspect()['pending_questions'],[])
        self.answer(success=False)
        self.assertEqual(self.received(),b'')

    def test_hook_without_native_wire_stays_read_only(self):
        self.wire.unlink()
        self.hook(dict(hook_event_name='PreToolUse',tool_name='AskUserQuestion',tool_call_id='tool_test',tool_input=dict(questions=self.items)))
        card=self.inspect()['pending_questions'][0]
        self.assertEqual(card['source'],'hook'); self.assertFalse(card['can_answer'])
        self.answer(self.payload(question_id=card['question_id'],expected_question_hash=card['question_hash']),success=False)
        self.assertEqual(self.received(),b'')

    def test_remote_answer_payload_remains_stdin(self):
        self.script('ssh',"""#!/usr/bin/env python3
import json,os,pathlib,sys
pathlib.Path(os.environ['INPUT_FIXTURE'],'ssh-args').write_text(json.dumps(sys.argv[1:]))
os.execv('/bin/sh',['sh','-c',sys.argv[-1]])
""")
        receipt=self.answer(remote=True)
        self.assertEqual(receipt['status'],'answered')
        args=json.loads((self.root/'ssh-args').read_text())
        self.assertNotIn('-t',args)
        self.assertNotIn('Привет',' '.join(args))
        self.assertEqual(args[-1],'~/.local/bin/hgs answer kimi/question-test --json')

    def test_wrong_native_acknowledgement_is_uncertain_and_not_retried(self):
        (self.root/'wrong-ack').touch()
        payload=self.payload()
        self.assertIn('delivery uncertain',self.answer(payload,success=False))
        before=self.received()
        self.assertFalse(any(event['type']=='QuestionAnswered' for event in self.inspect()['events']))
        self.assertIn('delivery uncertain',self.answer(payload,success=False))
        self.assertEqual(self.received(),before)

    def test_concurrent_distinct_requests_are_serialized_without_duplicate_submission(self):
        argv=[str(HGS),'answer',self.name,'--json']
        first=subprocess.Popen(argv,env=self.env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        first.stdin.write(json.dumps(self.payload())); first.stdin.close(); first.stdin=None
        second=subprocess.Popen(argv,env=self.env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        out2,err2=second.communicate(json.dumps(self.payload()),timeout=15)
        out1,err1=first.communicate(timeout=15)
        self.assertEqual(sorted([first.returncode,second.returncode]),[0,1],err1+err2)
        records=[json.loads(line) for line in self.wire.read_text().splitlines()]
        self.assertEqual(sum(row['type']=='interaction.resolved' for row in records),1)


class PlanReview(unittest.TestCase):
    """Kimi's ExitPlanMode approval: the wire holds the plan, the TUI its choices."""
    script=fixtures.InputTransport.script
    tmux=fixtures.InputTransport.tmux
    wait_for=fixtures.InputTransport.wait_for
    received=fixtures.InputTransport.received
    write_record=fixtures.InputTransport.write_record
    tearDown=fixtures.InputTransport.tearDown
    inspect=Questions.inspect
    answer=Questions.answer
    plan='# Rotate fixture keys\n\n1. Add the rotation script.\n2. Verify it.'

    def setUp(self):
        original=fixtures.FAKE_AGENT
        try:
            fixtures.FAKE_AGENT=FAKE_PLAN
            fixtures.InputTransport.setUp(self)
        finally: fixtures.FAKE_AGENT=original
        old_path=self.record_path
        self.tmux('rename-session','-t','='+self.name,'kimi/plan-test')
        self.name='kimi/plan-test'
        self.conversation_id='session_'+str(uuid.uuid4())
        self.record_path=self.state/(hashlib.sha256(self.name.encode()).hexdigest()+'.json')
        old_path.unlink()
        self.record.update(name=self.name,agent='kimi',conversation_id=self.conversation_id,
                           agent_home=str(self.root/'.kimi-code'),activity='busy',phase='approval',
                           active_tools={'tool_plan':{'name':'ExitPlanMode'}})
        self.write_record()
        self.wire=self.root/'.kimi-code/sessions/wd_fixture'/self.conversation_id/'agents/main/wire.jsonl'
        self.wire.parent.mkdir(parents=True)
        self.event=dict(type='interaction.request',id='approval_'+str(uuid.uuid4()),kind='approval',agentId='main',
                        toolCallId='tool_plan',request=dict(toolName='ExitPlanMode',toolCallId='tool_plan',
                        action='Presenting plan and exiting plan mode',display=dict(kind='plan_review',plan=self.plan,
                        path=str(self.root/'plans/example.md'))),time=int(time.time()*1000))
        self.wire.write_text(json.dumps(self.event)+'\n')
        self.show(self.event)
        self.card=self.inspect()['pending_questions'][0]

    def show(self,event):
        (self.root/'question-config').write_text(json.dumps(dict(event,wire=str(self.wire))))
        os.kill(self.record['pid'],signal.SIGUSR1)
        self.wait_for(lambda:'Ready to build' in self.tmux('capture-pane','-p','-t',self.pane))

    def payload(self,selected=('approve',),text=''):
        return dict(request_id=str(uuid.uuid4()),expected_run_id=self.run_id,expected_conversation_id=self.conversation_id,
                    question_id=self.card['question_id'],expected_question_hash=self.card['question_hash'],
                    answers=[dict(question_id='plan',selected_option_ids=list(selected),text=text)])

    def test_shows_the_plan_and_native_choices_without_input(self):
        card=self.card
        self.assertEqual((card['source'],card['plan_review'],card['can_answer']),('kimi_plan_review',True,True),card)
        item=card['questions'][0]
        self.assertEqual((item['question'],item['body'],item['other_label']),('Ready to build with this plan?',self.plan,'Revise'))
        self.assertEqual([option['label'] for option in item['options']],['Approve','Reject'])
        self.assertEqual(self.received(),b'')

    def test_approve_uses_one_number_key_and_confirms_the_wire(self):
        receipt=self.answer(self.payload())
        self.assertEqual(receipt['status'],'answered')
        self.assertEqual(self.received(),b'1')
        self.assertEqual(json.loads((self.root/'submitted').read_text()),{'decision':'approved','selectedLabel':'Approve'})
        inspection=self.inspect()
        self.assertEqual(inspection['pending_questions'],[])
        answered=[event for event in inspection['events'] if event['type']=='QuestionAnswered']
        self.assertEqual(len(answered),1)
        self.assertIn('Ready to build with this plan? → Approve',answered[0]['detail'])

    def test_revise_pastes_exact_feedback(self):
        feedback='Добавь шаг отката'
        receipt=self.answer(self.payload(selected=(),text=feedback))
        self.assertEqual(receipt['status'],'answered')
        self.assertEqual(self.received(),b'3\x1b[200~'+feedback.encode()+b'\x1b[201~\r')
        self.assertEqual(json.loads((self.root/'submitted').read_text()),
                         {'decision':'rejected','selectedLabel':'Revise','feedback':feedback})

    def test_different_chooser_or_acknowledgement_is_never_accepted(self):
        # The screen proposes approaches this request does not have.
        options=[dict(label='Script',description=''),dict(label='Manual',description='')]
        self.show(dict(self.event,request=dict(self.event['request'],display=dict(self.event['request']['display'],options=options))))
        card=self.inspect()['pending_questions'][0]
        self.assertFalse(card['can_answer'])
        self.assertIn('differ',card['answer_unavailable_reason'])
        self.answer(self.payload(),success=False)
        self.assertEqual(self.received(),b'')
        self.show(self.event)
        (self.root/'wrong-ack').touch()
        self.assertIn('delivery uncertain',self.answer(self.payload(),success=False))
        self.assertEqual(self.received(),b'1')
        self.assertFalse(any(event['type']=='QuestionAnswered' for event in self.inspect()['events']))


if __name__=='__main__': unittest.main()
