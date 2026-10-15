"""Native interrupt and terminal upload contracts on a private raw PTY."""
import base64,json,subprocess,time,unittest,uuid
import test_input as fixtures
HGS=fixtures.HGS

class SessionActions(unittest.TestCase):
    setUp=fixtures.InputTransport.setUp
    tearDown=fixtures.InputTransport.tearDown
    script=fixtures.InputTransport.script
    tmux=fixtures.InputTransport.tmux
    wait_for=fixtures.InputTransport.wait_for
    write_record=fixtures.InputTransport.write_record
    screen=fixtures.InputTransport.screen
    received=fixtures.InputTransport.received
    def request(self,command,data,ok=True,name=None):
        args=[str(HGS),command,name or self.name]
        if command=='attachment':args+=['--stage']
        result=subprocess.run(args+['--json'],input=json.dumps(data),env=self.env,text=True,capture_output=True,timeout=10)
        if ok:self.assertEqual(result.returncode,0,result.stderr)
        else:self.assertNotEqual(result.returncode,0)
        return json.loads(result.stdout) if result.stdout.strip() else result.stderr
    def identity(self):return dict(request_id=str(uuid.uuid4()),expected_run_id=self.run_id,expected_conversation_id=self.conversation_id)
    def working(self,agent):
        self.record.update(agent=agent,run_identity_version=1,supervisor=dict(pid=self.record['pid'],start=self.record['process_start']),phase='working',activity='busy',turn_started=time.time())
        self.write_record()
    def test_interrupt_sends_one_escape_preserves_process_and_draft(self):
        for agent in ('claude','kimi','codex'):
            with self.subTest(agent=agent):
                self.working(agent)
                self.screen('\x1b[2J\x1b[HUnsent terminal draft','21:0')
                before=self.received();data=dict(self.identity(),expected_turn_started=self.record['turn_started'])
                self.request('interrupt',data);self.wait_for(lambda:len(self.received())>len(before))
                self.assertEqual(self.received()[len(before):],b'\x1b')
                self.request('interrupt',data,ok=False);self.assertEqual(self.received()[len(before):],b'\x1b')
                self.assertIn('Unsent terminal draft',self.tmux('capture-pane','-p','-t',self.pane))
    def test_codex_stop_leaves_ordinary_follow_ups_as_unsent_terminal_draft(self):
        # Screens of the pinned Codex (110x35) before and after its Escape with only
        # ordinary (Tab) follow-ups queued. Escape interrupts the turn and returns both
        # to Codex's input as one unsent draft, then Codex reports Interrupt. Nothing in
        # the Stop path may submit them or report them as started or sent.
        from pathlib import Path
        transport=fixtures.InputTransport
        def drawn(name,x,y):
            rows=(Path(__file__).resolve().parent/'fixtures'/name).read_text().split('\n')[:-1]
            return '\x1b[2J\x1b[H'+'\r\n'.join(rows)+f'\x1b[{y+1};{x+1}H'
        self.tmux('resize-window','-t','='+self.name+':','-x','110','-y','35')
        for suffix in ('-0.160.1',''):
            with self.subTest(version=suffix or '0.162.0'):
                self.working('codex')
                self.screen(drawn(f'codex-queued-follow-ups{suffix}.txt',2,31),'2:31')
                queue=transport.inspect(self)['input_queue']
                self.assertFalse(queue['can_send_now'])
                self.assertEqual(queue['hint'],'Use Terminal to edit or submit these queued follow-ups')
                self.assertIn('Queued follow-up one',queue['text']);self.assertIn('Queued follow-up two',queue['text'])
                (self.root/'on-escape').write_bytes(drawn(f'codex-queued-follow-ups-escape{suffix}.txt',22,31).encode())
                before=self.received()
                receipt=self.request('interrupt',dict(self.identity(),expected_turn_started=self.record['turn_started']))
                # The receipt reports the interrupt request only, never a delivered message.
                self.assertEqual(set(receipt),{'request_id','name','run_id','conversation_id','status'})
                self.assertEqual(receipt['status'],'submitted')
                self.wait_for(lambda:'Queued follow-up two' in self.tmux('capture-pane','-p','-t',self.pane)
                              and 'Queued follow-up inputs' not in self.tmux('capture-pane','-p','-t',self.pane))
                # Before Codex's Interrupt hook the turn still counts as busy, so only the
                # screen can hide the queue: no queue panel is left to show or send.
                self.assertIsNone(transport.inspect(self).get('input_queue'))
                transport.interrupted_turn(self)
                info=transport.inspect(self)
                self.assertEqual((info['phase'],info['activity']),('interrupted','unknown'))
                self.assertIsNone(info.get('input_queue'))
                # Activity cannot send over the draft: it stays in Terminal until submitted,
                # edited or cleared there.
                self.assertFalse(info['interrupted_message_can_send'])
                self.assertEqual(info['interrupted_message_reason'],'terminal has a draft, dialog, or unrecognized prompt; '
                                 'open Terminal and return to an empty agent prompt')
                self.assertIn('terminal has a draft',transport.send(self,transport.payload(self,text='next'),success=False))
                # One Escape and nothing else: no Enter submitted the draft and no text was pasted.
                self.assertEqual(self.received()[len(before):],b'\x1b')
                self.assertIn('\n› Queued follow-up one\n  Queued follow-up two\n',self.tmux('capture-pane','-p','-t',self.pane))
                (self.root/'on-escape').unlink()
    def test_claude_interrupt_is_recorded_once_claude_shows_it(self):
        # Claude emits no hook after Escape; its own transcript line confirms it.
        self.working('claude');self.record.update(active_tools=dict(tool=dict(name='Bash',detail='sleep 30')));self.write_record()
        self.screen('\x1b[2J\x1b[H  \u23bf  Interrupted \u00b7 What should Claude do instead?\r\n','0:1')
        (self.root/'on-escape').write_bytes('\r\n  \u23bf  Interrupted \u00b7 What should Claude do instead?\r\n\u276f '.encode())
        receipt=self.request('interrupt',dict(self.identity(),expected_turn_started=self.record['turn_started']))
        self.assertTrue(receipt['confirmed'])
        state=json.loads(self.record_path.read_text())
        self.assertEqual((state['phase'],state['activity'],state['active_tools']),('interrupted','unknown',{}))
        # An earlier interruption still on screen is not a confirmation.
        (self.root/'on-escape').unlink();self.working('claude')
        receipt=self.request('interrupt',dict(self.identity(),expected_turn_started=self.record['turn_started']))
        self.assertFalse(receipt['confirmed'])
        self.assertEqual(json.loads(self.record_path.read_text())['phase'],'working')
    def test_claude_rewound_prompt_moves_from_terminal_to_activity(self):
        # Escape before Claude's first reply rewinds the turn: no interruption line, the
        # prompt returns to Claude's input. It leaves Terminal row by row for Activity.
        rule='\u2500'*30
        def box(rows,x,y):return ('\x1b[2J\x1b[H'+rule+'\r\n'+'\r\n'.join(rows)+'\r\n'+rule+f'\x1b[{y+1};{x+1}H').encode()
        self.working('claude');self.record.update(prompt='first line\nsecond');self.write_record()
        self.screen(box(['\u276f\u00a0'],2,1).decode(),'2:1')
        (self.root/'on-escape').write_bytes(box(['\u276f\u00a0first line','  second'],8,2))
        (self.root/'replies').write_text(json.dumps([['15',box(['\u276f\u00a0first line','  '],2,2).decode()],
            ['7f',box(['\u276f\u00a0first line'],12,1).decode()],['15',box(['\u276f\u00a0'],2,1).decode()]]))
        before=self.received()
        receipt=self.request('interrupt',dict(self.identity(),expected_turn_started=self.record['turn_started']))
        self.assertTrue(receipt['confirmed'])
        self.assertEqual(self.received()[len(before):],b'\x1b\x15\x7f\x15')
        self.assertEqual(json.loads(self.record_path.read_text())['phase'],'interrupted')
        # Text that is not exactly the prompt stays in Terminal, and the turn stays working.
        (self.root/'replies').unlink();self.working('claude');self.record.update(prompt='first line');self.write_record()
        (self.root/'on-escape').write_bytes(box(['\u276f\u00a0first line and an edit'],25,1))
        before=self.received()
        receipt=self.request('interrupt',dict(self.identity(),expected_turn_started=self.record['turn_started']))
        self.assertFalse(receipt['confirmed']);self.assertEqual(self.received()[len(before):],b'\x1b')
        self.assertEqual(json.loads(self.record_path.read_text())['phase'],'working')
    def test_claude_terminal_approval_answers_settle_from_the_screen(self):
        # Claude Code 2.1.296 sends no hook after an approval is answered in Terminal.
        # Its real screens settle the request; anything short of proof keeps it waiting.
        from pathlib import Path
        command=('sleep 6 && printf %s fixture-output-with-a-rather-long-name-to-wrap-the-row > '
                 '/work/example/projects/a-really-long-directory-name/approved.txt\necho second line')
        def pending():
            self.record.update(agent='claude',activity='busy',phase='approval',turn_started=time.time(),
                active_tools=dict(toolu_fixture=dict(name='Bash',detail='sleep 6',started=1.0)),pending_approval_at=time.time(),
                pending_approval=dict(hook_event_name='PermissionRequest',tool_name='Bash',tool_input=dict(command=command)))
            self.write_record()
        def show(fixture,column):
            rows=(Path(__file__).resolve().parent/'fixtures'/fixture).read_text().split('\n')[:-1]
            self.tmux('resize-window','-t','='+self.name+':','-x',str(max(map(len,rows))),'-y',str(len(rows)))
            self.screen('\x1b[2J\x1b[H'+'\r\n'.join(rows)+f'\x1b[1;{column}H',f'{column-1}:0')
        def listed():
            result=subprocess.run([str(HGS),'ls','--json','--local'],env=self.env,text=True,capture_output=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stderr)
            return next(row for row in json.loads(result.stdout)['sessions'] if row['name']==self.name)
        def inspected():
            result=subprocess.run([str(HGS),'inspect',self.name],env=self.env,text=True,capture_output=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stderr)
            return json.loads(result.stdout)
        def marked(signature):
            # `hgs read` (desktop mark-read, phone mark_read) acknowledges only the current attention.
            data=dict(run_id=self.run_id,conversation_id=self.conversation_id,reply_id=None,attention_signature=signature)
            return self.request('read',data)['applied']['attention_signature']
        pending()
        # The panel, then Yes before the command shows its elapsed time.
        for column,fixture in enumerate(['claude-bash-safety.txt','claude-approval-starting.txt'],start=2):
            show(fixture,column)
            self.assertEqual((listed()['phase'],inspected()['phase']),('approval','approval'),fixture)
            self.assertIn('pending_approval',json.loads(self.record_path.read_text()))
            signature=listed()['attention_signature']
            self.assertTrue(marked(signature),fixture)
            self.assertIn('pending_approval',json.loads(self.record_path.read_text()))
        show('claude-approval-running.txt',5)
        # Marking read settles it first, so the waiting request is no longer current.
        self.assertFalse(marked(signature))
        state=json.loads(self.record_path.read_text())
        self.assertEqual((state['phase'],state['activity'],'pending_approval' in state),('tool','busy',False))
        self.assertEqual((listed()['phase'],listed()['attention_signature']),('tool',None))
        # No or Esc, with the composer or the one-time auto-mode nudge below.
        for column,fixture in enumerate(['claude-approval-denied.txt','claude-approval-denied-nudge.txt'],start=6):
            pending();show(fixture,column)
            inspection=inspected()
            self.assertEqual((inspection['phase'],inspection['pending_questions']),('interrupted',[]),fixture)
            state=json.loads(self.record_path.read_text())
            self.assertEqual((state['phase'],state['activity'],state['active_tools'],'pending_approval' in state),
                             ('interrupted','unknown',{},False))
        pending();show('claude-approval-denied.txt',8)
        self.assertFalse(marked('0'*64))
        state=json.loads(self.record_path.read_text())
        self.assertEqual((state['phase'],'pending_approval' in state),('interrupted',False))
        self.assertEqual(self.received(),b'')
    def test_changed_turn_and_idle_state_receive_nothing(self):
        self.working('codex');data=dict(self.identity(),expected_turn_started=self.record['turn_started']-1)
        self.request('interrupt',data,ok=False)
        data['expected_turn_started']=self.record['turn_started'];self.record.update(activity='idle',phase='idle');self.write_record()
        self.request('interrupt',data,ok=False);self.assertEqual(self.received(),b'')
    def test_upload_pins_run_and_stages_without_pasting_or_submitting(self):
        data=dict(self.identity(),attachments=[dict(name='report \'one.pdf',mime='application/pdf',data_base64=base64.b64encode(b'%PDF fixture').decode())])
        staged=self.request('attachment',data)
        from pathlib import Path
        path=Path(staged['files'][0]['path']);self.assertEqual(path.read_bytes(),b'%PDF fixture')
        self.assertTrue(path.is_relative_to(self.state/'attachments'));self.assertEqual(path.stat().st_mode&0o777,0o600)
        self.assertEqual(self.received(),b'')
        data['expected_run_id']='another';self.request('attachment',data,ok=False);self.assertEqual(self.received(),b'')
    def test_untracked_shell_upload_is_bound_to_its_live_session(self):
        # A shell from `hgs sh -d` has no run identity: the session's creation time
        # and the absence of a run marker bind the copy instead.
        shell='sh/upload-test';self.tmux('new-session','-d','-s',shell,'-x','80','-y','24','sleep','300')
        created=int(self.tmux('display-message','-p','-t','='+shell+':','#{session_created}'))
        data=dict(request_id=str(uuid.uuid4()),expected_run_id='',expected_conversation_id='',expected_created=created,
                  attachments=[dict(name='notes.txt',mime='text/plain',data_base64=base64.b64encode(b'untracked fixture').decode())])
        staged=self.request('attachment',data,name=shell)
        self.assertEqual((staged['status'],staged['name'],staged['run_id'],staged['conversation_id'],staged['created']),('staged',shell,'','',created))
        from pathlib import Path
        path=Path(staged['files'][0]['path']);self.assertEqual(path.read_bytes(),b'untracked fixture')
        self.assertTrue(path.is_relative_to(self.state/'attachments'));self.assertEqual(path.stat().st_mode&0o777,0o600)
        # Another session under that name, an unknown name, or a missing creation time.
        self.assertIn('Session changed',self.request('attachment',dict(data,expected_created=created-1),ok=False,name=shell))
        self.assertIn('Session changed',self.request('attachment',data,ok=False,name='sh/unknown'))
        self.assertIn('no saved binding',self.request('attachment',{k:v for k,v in data.items() if k!='expected_created'},ok=False,name=shell))
        # A tracked run whose identity the caller has not loaded yet.
        tracked=int(self.tmux('display-message','-p','-t','='+self.name+':','#{session_created}'))
        self.assertIn('Wait for the session identity',self.request('attachment',dict(data,expected_created=tracked),ok=False))
        # The shell ended after the listing that named it.
        self.tmux('kill-session','-t','='+shell)
        self.assertIn('Session changed',self.request('attachment',data,ok=False,name=shell))
        self.assertEqual(self.received(),b'')

if __name__=='__main__':unittest.main()
