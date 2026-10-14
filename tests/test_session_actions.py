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
    def request(self,command,data,ok=True):
        args=[str(HGS),command,self.name]
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
        pending()
        # The panel, then Yes before the command shows its elapsed time.
        for column,fixture in enumerate(['claude-bash-safety.txt','claude-approval-starting.txt'],start=2):
            show(fixture,column)
            self.assertEqual((listed()['phase'],inspected()['phase']),('approval','approval'),fixture)
            self.assertIn('pending_approval',json.loads(self.record_path.read_text()))
        show('claude-approval-running.txt',5)
        self.assertEqual(listed()['phase'],'tool')
        state=json.loads(self.record_path.read_text())
        self.assertEqual((state['phase'],state['activity'],'pending_approval' in state),('tool','busy',False))
        # No or Esc, with the composer or the one-time auto-mode nudge below.
        for column,fixture in enumerate(['claude-approval-denied.txt','claude-approval-denied-nudge.txt'],start=6):
            pending();show(fixture,column)
            inspection=inspected()
            self.assertEqual((inspection['phase'],inspection['pending_questions']),('interrupted',[]),fixture)
            state=json.loads(self.record_path.read_text())
            self.assertEqual((state['phase'],state['activity'],state['active_tools'],'pending_approval' in state),
                             ('interrupted','unknown',{},False))
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

if __name__=='__main__':unittest.main()
