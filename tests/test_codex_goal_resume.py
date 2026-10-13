#!/usr/bin/env python3
"""Codex's paused-goal resume choice in an isolated native-like TUI after `codex resume`."""
import os
import signal
import sqlite3
import time
import unittest
import uuid

import test_input as fixtures
from test_questions import Questions

OBJECTIVE = ('Finish the example export: verify the staging copy, compare row counts with the source, '
             'publish the reviewed archive and keep the previous backup until the replacement is accepted.')

# A 100-cell pane: Codex centers the 72-cell dialog over the transcript, which
# stays visible in the 14 cells on each side.
FAKE = r'''
import os,pathlib,signal,sqlite3,sys,termios,textwrap,tty
root=pathlib.Path(os.environ['INPUT_FIXTURE']);fd=sys.stdin.fileno();original=termios.tcgetattr(fd);tty.setraw(fd)
os.write(1,b'\x1b[?2004h');selected=0;open_dialog=True;buf=b''
labels=['Resume goal','Leave paused']
transcript=['• Ran python3 -I -B - <<PY: print the conversation identity for this session',
 '  └ CODEX_THREAD_ID 12345678-1234-1234-1234-123456789abc and its saved history']*6
def render(*args):
 global selected,open_dialog
 if (root/'selection').exists():selected=int((root/'selection').read_text());(root/'selection').unlink()
 if (root/'reopen').exists():open_dialog=True;(root/'reopen').unlink()
 rows=list(transcript)+['','› Ask Codex to do anything','','  GPT-6.1-Sol xhigh · /work/example']
 if open_dialog and (root/'objective').exists():
  objective=(root/'objective').read_text()
  dialog=['','  Resume paused goal?']+['  '+line for line in textwrap.wrap('Goal: '+objective,68)]+['',
   ('› ' if selected==0 else '  ')+'1. Resume goal   Mark it active and continue when idle',
   ('› ' if selected==1 else '  ')+'2. Leave paused  Keep it paused; use /goal resume later','','  enter select · esc back','']
  for i,line in enumerate(dialog):rows[i]=rows[i][:14].ljust(14)+line.ljust(72)+rows[i][86:]
 os.write(1,b'\x1b[2J\x1b[H'+'\r\n'.join(row.rstrip() for row in rows).encode())
signal.signal(signal.SIGUSR1,render);render();(root/'agent-pid').write_text(str(os.getpid()))
try:
 while True:
  value=os.read(fd,65536)
  if not value:break
  with (root/'received').open('ab') as f:f.write(value)
  buf+=value
  while buf:
   if buf.startswith((b'\x1b[A',b'\x1bOA')):selected=(selected-1)%2;buf=buf[3:];render()
   elif buf.startswith((b'\x1b[B',b'\x1bOB')):selected=(selected+1)%2;buf=buf[3:];render()
   elif buf.startswith(b'\r'):
    buf=buf[1:];(root/'submitted').write_text(labels[selected])
    if (root/'hold-dialog').exists():continue
    if selected==0:
     db=sqlite3.connect(root/'.codex'/'goals_1.sqlite');db.execute("UPDATE thread_goals SET status='active'");db.commit();db.close()
    open_dialog=False;render()
   elif buf[0]==27 and len(buf)<3:break
   else:buf=buf[1:]
finally:termios.tcsetattr(fd,termios.TCSANOW,original)
'''


class CodexGoalResume(unittest.TestCase):
    script = fixtures.InputTransport.script
    tmux = fixtures.InputTransport.tmux
    wait_for = fixtures.InputTransport.wait_for
    received = fixtures.InputTransport.received
    write_record = fixtures.InputTransport.write_record
    tearDown = fixtures.InputTransport.tearDown
    inspect = Questions.inspect
    answer = Questions.answer

    def setUp(self):
        original = fixtures.FAKE_AGENT
        try:
            fixtures.FAKE_AGENT = FAKE
            fixtures.InputTransport.setUp(self)
        finally:
            fixtures.FAKE_AGENT = original
        self.native_home = self.root / '.codex'
        self.native_home.mkdir()
        self.db = sqlite3.connect(self.native_home / 'goals_1.sqlite')
        self.addCleanup(self.db.close)
        self.db.execute('CREATE TABLE thread_goals(thread_id TEXT PRIMARY KEY, goal_id TEXT, objective TEXT, status TEXT, '
                        'token_budget INTEGER, tokens_used INTEGER, time_used_seconds INTEGER, created_at_ms INTEGER, '
                        'updated_at_ms INTEGER)')
        self.db.execute('INSERT INTO thread_goals VALUES (?, ?, ?, ?, NULL, 10, 10, 1000, 2000)',
                        (self.conversation_id, str(uuid.uuid4()), OBJECTIVE, 'usage_limited'))
        self.db.commit()
        # A resumed conversation before its first turn confirms it.
        self.record.update(agent='codex', run_identity_version=1, supervisor={}, startup_kind='resume',
                           expected_id=self.conversation_id, agent_home=str(self.native_home),
                           launch_dir='/work/example', phase='unknown', activity='unknown', last_event_at=0)
        self.write_record()
        self.redraw('objective', OBJECTIVE, 'Resume paused goal?')
        self.card = self.inspect()['pending_questions'][0]

    def payload(self, choice='leave_paused', **updates):
        result = dict(request_id=str(uuid.uuid4()), expected_run_id=self.run_id,
                      expected_conversation_id=self.conversation_id, question_id=self.card['question_id'],
                      expected_question_hash=self.card['question_hash'],
                      answers=[dict(question_id='codex_goal_resume', selected_option_ids=[choice], text='')])
        result.update(updates)
        return result

    def status(self):
        return self.db.execute('SELECT status FROM thread_goals').fetchone()[0]

    def redraw(self, file, value, expected):
        (self.root / file).write_text(value)
        os.kill(self.record['pid'], signal.SIGUSR1)
        self.wait_for(lambda: expected in self.tmux('capture-pane', '-p', '-t', self.pane))

    def test_card_names_the_goal_and_blocks_messages(self):
        state = self.inspect()
        self.assertEqual((state['phase'], state['attention_id']), ('input', self.card['question_id']))
        self.assertEqual(state['activity_detail'], 'Codex is waiting for your paused goal choice')
        self.assertTrue(self.card['question_id'].startswith('codex-goal-resume:'))
        self.assertEqual((self.card['source'], self.card['can_answer']), ('codex_goal_resume', True))
        item = self.card['questions'][0]
        self.assertEqual((item['question'], item['body']), ('Resume paused goal?', 'Goal: ' + OBJECTIVE))
        self.assertEqual([option['id'] for option in item['options']], ['resume_goal', 'leave_paused'])
        self.assertIn('paused goal', fixtures.InputTransport.send(self, fixtures.InputTransport.payload(self), success=False))
        self.assertEqual(self.received(), b'')

    def test_leave_paused_keeps_the_goal_and_closes_the_dialog(self):
        receipt = self.answer(self.payload('leave_paused'))
        self.assertEqual(receipt['status'], 'answered')
        self.assertEqual((self.root / 'submitted').read_text(), 'Leave paused')
        self.assertEqual(self.received().count(b'\r'), 1)
        self.assertEqual(self.status(), 'usage_limited')
        self.assertEqual(self.inspect()['pending_questions'], [])

    def test_resume_goal_from_either_highlight(self):
        self.redraw('selection', '1', '› 2. Leave paused')
        self.card = self.inspect()['pending_questions'][0]
        receipt = self.answer(self.payload('resume_goal'))
        self.assertEqual(receipt['status'], 'answered')
        self.assertEqual((self.root / 'submitted').read_text(), 'Resume goal')
        self.assertEqual(self.status(), 'active')
        self.assertEqual(self.inspect()['pending_questions'], [])

    def test_changed_goal_or_run_rejects_without_input(self):
        self.answer(self.payload(expected_run_id='other'), success=False)
        self.db.execute("UPDATE thread_goals SET objective='Another objective'")
        self.db.commit()
        self.assertIn('question', self.answer(self.payload(), success=False))
        self.assertEqual(self.received(), b'')
        self.db.execute('UPDATE thread_goals SET objective=?', (OBJECTIVE,))
        self.db.commit()
        for field, value in [('last_event_at', time.time()), ('startup_kind', 'new'), ('expected_id', str(uuid.uuid4()))]:
            with self.subTest(field=field):
                previous = self.record.get(field)
                self.record[field] = value
                self.write_record()
                self.assertEqual(self.inspect()['pending_questions'], [])
                self.record[field] = previous
                self.write_record()

    def test_no_acknowledgement_is_uncertain_and_never_retried(self):
        (self.root / 'hold-dialog').touch()
        request = self.payload('leave_paused')
        self.assertIn('delivery uncertain', self.answer(request, success=False))
        before = self.received()
        self.assertIn('delivery uncertain', self.answer(request, success=False))
        self.assertEqual(self.received(), before)


if __name__ == '__main__':
    unittest.main()
