#!/usr/bin/env python3
"""Real Codex async question panels against a localhost Responses fixture, never a user's account.
Run with HGS_CODEX_TEST_BIN=/absolute/path/to/codex (otherwise skipped).
"""
import http.server
import json
import os
import re
import threading
import unittest

import test_clear_context_native as native
import test_codex_async_questions as synthetic
import test_codex_update

PROMPT = 'Ask questions with the tool now.'
CALL = 'call_native_question'
QUESTIONS = [dict(title='Which option?', options=['One', 'Two']), dict(title='Any notes?')]
# Taller than an unviewed 80x24 pane and than both CI geometries (95x47, 110x35).
TALL = [dict(title=' '.join(['Before releasing, which channel should this build target, given that stable needs '
                             'the full CI matrix, signed artifacts and the owner-triggered publication step?'] * 3),
             options=[f'Channel {i}: publish after the complete matrix, package checks and signed asset '
                      'verification succeed on this exact candidate' for i in range(1, 21)])]
REPLY = ('<send_user_message_question_reply>', '</send_user_message_question_reply>')
# Codex 0.160 prints "ctrl+] skip"; 0.162 abbreviates it as "^] skip".
SKIP_KEYS = ('ctrl+] skip', '^] skip')


def offered(tools):
    """Tool names; Codex 0.160-0.162 offer theirs in an additional_tools item's namespaces."""
    return {name for tool in tools if isinstance(tool, dict)
            for name in (offered(tool.get('tools') or []) if tool.get('type') == 'namespace' else [tool.get('name')])}


class Provider(http.server.BaseHTTPRequestHandler):
    """One request_user_input_async call; the turn that follows stays open until released.

    Codex shows its question panel only while a turn works. Codex 0.162 steers
    a reply entered there into that turn: it abandons the open response and asks
    again with the reply, which is held the same way. Codex 0.160 queues the
    reply natively and sends it once the turn ends.
    """
    def log_message(self, *args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0))) or b'{}')
        items = [item for item in body.get('input') or [] if isinstance(item, dict)]
        texts = [part.get('text', '') for item in items if item.get('role') == 'user'
                 for part in item.get('content') or [] if isinstance(part, dict)]
        tools = offered((body.get('tools') or []) + [tool for item in items if item.get('type') == 'additional_tools'
                                                     for tool in item.get('tools') or []])
        answering = any(item.get('type') == 'function_call_output' and item.get('call_id') == CALL for item in items)
        self.server.requests.append(dict(texts=texts, tools=tools, answering=answering))
        if not answering and texts and texts[-1] == PROMPT and 'request_user_input_async' in tools:
            item = dict(type='function_call', id='fc_question', call_id=CALL, name='request_user_input_async',
                        arguments=json.dumps(dict(questions=self.server.questions)), status='completed')
        else:
            item = dict(type='message', id='msg_fixture', role='assistant', status='completed',
                        content=[dict(type='output_text', text='Fixture turn complete.', annotations=[])])
        response = dict(id='resp_fixture', object='response', status='completed', output=[item],
                        usage=dict(input_tokens=1, output_tokens=5, total_tokens=6))
        try:
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.end_headers()
            self.emit('response.created', response=dict(response, status='in_progress', output=[]))
            if answering:
                self.server.release.wait(120)
            self.emit('response.output_item.added', output_index=0, item=dict(item, status='in_progress'))
            self.emit('response.output_item.done', output_index=0, item=item)
            self.emit('response.completed', response=response)
        except OSError:
            pass  # Codex abandoned this response for a steered reply, or exited.

    def emit(self, kind, **data):
        self.wfile.write(('event: ' + kind + '\ndata: ' + json.dumps(dict(type=kind, **data)) + '\n\n').encode())
        self.wfile.flush()


@unittest.skipUnless(os.environ.get('HGS_CODEX_TEST_BIN'), 'Set HGS_CODEX_TEST_BIN for the isolated native Codex question test')
class NativeCodexAsyncQuestions(unittest.TestCase):
    """The resumed clear fixture, served by Provider with the async question feature enabled."""
    script = native.NativeCodexClear.script
    write_record = native.NativeCodexClear.write_record
    tmux = native.NativeCodexClear.tmux
    tearDown = native.NativeCodexClear.tearDown
    fail = native.NativeCodexClear.fail
    wait_for = test_codex_update.NativeCodexUpdate.wait_for
    inspect = synthetic.CodexAsync.inspect
    native_features = 'send_message_to_user_async=true\n'
    questions = QUESTIONS

    def setUp(self):
        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Provider)
        self.server.requests, self.server.release = [], threading.Event()
        self.server.questions = self.questions
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.addCleanup(self.server.release.set)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.provider_url = 'http://127.0.0.1:' + str(self.server.server_port) + '/v1'
        native.NativeCodexClear.setUp(self)
        self.ids = ['codex-async:' + CALL + ':' + str(index) for index in range(len(self.questions))]
        self.tmux('send-keys', '-t', self.pane, '-l', PROMPT)
        self.wait_for(lambda: PROMPT in self.screen())
        self.tmux('send-keys', '-t', self.pane, 'Enter')
        self.wait_for(lambda: any(request['texts'][-1:] == [PROMPT] for request in self.server.requests))
        tools = next(request['tools'] for request in self.server.requests if request['texts'][-1:] == [PROMPT])
        self.assertIn('request_user_input_async', tools, 'Codex did not offer its async question tool')
        # Codex accepted the call and asks again: that turn now stays open.
        self.wait_for(lambda: any(request['answering'] for request in self.server.requests))
        self.wait_for(lambda: 'shift+← to answer' in self.screen())

    def screen(self):
        return self.tmux('capture-pane', '-p', '-t', self.pane)

    def panel(self):
        """The open question panel below Codex's queued inputs, or '' without one."""
        screen = self.screen()
        rows = screen.rpartition('Queued follow-up inputs')[2]
        return rows if 'Queued follow-up inputs' in screen and any(key in rows for key in SKIP_KEYS) else ''

    def open_panel(self):
        self.tmux('send-keys', '-t', self.pane, 'S-Left')
        self.wait_for(lambda: 'Which option?' in self.panel())
        # The layout hgs must recognize: this card's position above its title.
        self.assertEqual([row.strip() for row in self.panel().splitlines() if row.strip()][:2], ['1 of 2', 'Which option?'])

    def cards(self):
        return {card['question_id']: card for card in self.inspect()['pending_questions']}

    def answer(self, card, answer, request_id=None):
        result, payload = synthetic.CodexAsync.answer(self, card, answer, request_id)
        self.assertEqual(result.returncode, 0, result.stderr + '\n' + self.screen())
        return json.loads(result.stdout), payload

    def replies(self):
        """Question replies in the conversation Codex last sent, decoded from its own envelopes."""
        turn = next((request for request in reversed(self.server.requests) if request['answering']), dict(texts=[]))
        return [json.loads(text[len(REPLY[0]):-len(REPLY[1])]) for text in turn['texts']
                if text.startswith(REPLY[0]) and text.endswith(REPLY[1])]

    def reply(self, card, answer):
        return [dict(questionItemId=card['native_question_id'], question=card['questions'][0]['question'], answer=answer)]

    def deliver(self, card, answer):
        """Answer through the open panel, as steered (0.162) or natively queued (0.160) input."""
        receipt, payload = self.answer(card, answer)
        if receipt['status'] == 'submitted':
            self.assertIn('Messages to be submitted after next tool call', self.screen())
            locked = self.cards()[card['question_id']]
            self.assertFalse(locked['can_answer'])
            self.assertEqual(locked['answer_delivery']['request_id'], payload['request_id'])
        else:
            self.assertEqual(receipt['status'], 'answered')
        return receipt, payload

    def finish(self, replies):
        """End the held turn; Codex sends any queued reply and hgs resolves each answered card."""
        self.server.release.set()
        self.wait_for(lambda: self.replies() == replies)
        answered = {reply[0]['questionItemId'] for reply in replies}
        self.wait_for(lambda: not [card for card in self.cards().values() if card['native_question_id'] in answered])

    def test_progress_panel_answers_an_option_once(self):
        pending = self.cards()
        self.assertEqual(sorted(pending), self.ids)
        first = pending[self.ids[0]]
        self.assertEqual(first['source'], 'codex_async')
        self.assertEqual(first['native_question_id'], json.dumps(['request_user_input_async', CALL, 0], separators=(',', ':')))
        self.assertEqual(first['questions'][0]['question'], 'Which option?')
        self.assertEqual([option['label'] for option in first['questions'][0]['options']], ['One', 'Two'])
        self.assertTrue(first['optional'])
        self.open_panel()
        first = self.cards()[self.ids[0]]
        self.assertTrue(first['can_answer'], first['answer_unavailable_reason'])
        self.assertEqual(first['answer_transport'], 'codex_tui')
        receipt, payload = self.deliver(first, dict(question_id='q_0', selected_option_ids=['opt_1']))
        replay, _ = self.answer(first, payload['answers'][0], payload['request_id'])
        self.assertEqual(replay, receipt)
        self.finish([self.reply(first, 'Two')])
        self.assertEqual(sorted(self.cards()), self.ids[1:])

    def test_other_and_free_form_answers_follow_the_panel(self):
        self.open_panel()
        first = self.cards()[self.ids[0]]
        self.deliver(first, dict(question_id='q_0', text='Accepted answer'))
        self.wait_for(lambda: 'Which option?' not in self.panel() and 'Any notes?' in self.panel())
        second = self.cards()[self.ids[1]]
        self.assertTrue(second['can_answer'], second['answer_unavailable_reason'])
        self.deliver(second, dict(question_id='q_0', text='Free notes'))
        self.finish([self.reply(first, 'Accepted answer'), self.reply(second, 'Free notes')])
        self.assertEqual(self.cards(), {})

    def test_skip_dismisses_each_question_without_a_reply(self):
        self.open_panel()
        for index, remaining in [(0, 'Any notes?'), (1, None)]:
            card = self.cards()[self.ids[index]]
            receipt, _ = self.answer(card, dict(question_id='q_0', skip=True))
            self.assertEqual(receipt['status'], 'answered')
            self.assertTrue(receipt['skipped'])
            # Only Codex's own skip key moves its panel on, or closes it.
            if remaining:
                self.wait_for(lambda: 'Which option?' not in self.panel() and remaining in self.panel())
            else:
                self.wait_for(lambda: not self.panel())
            self.assertEqual(sorted(self.cards()), self.ids[index + 1:])
        # Ending the turn sends nothing either, and the skips stay recorded.
        self.server.release.set()
        self.wait_for(lambda: 'Fixture turn complete.' in self.screen())
        self.assertEqual(self.replies(), [])
        self.assertEqual(self.cards(), {})

    def test_native_draft_is_never_overwritten(self):
        self.open_panel()
        self.tmux('send-keys', '-t', self.pane, 'Down', 'Down')
        self.wait_for(lambda: '› 3. Other' in self.panel())
        self.tmux('send-keys', '-t', self.pane, '-l', 'Terminal draft')
        self.wait_for(lambda: '› 3. Terminal draft' in self.panel())
        first = self.cards()[self.ids[0]]
        self.assertFalse(first['can_answer'])
        self.assertIn('draft in Terminal', first['answer_unavailable_reason'])
        result, _ = synthetic.CodexAsync.answer(self, first, dict(question_id='q_0', text='Accepted answer'))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('draft in Terminal', result.stderr)
        self.assertIn('› 3. Terminal draft', self.panel())
        self.assertEqual(self.replies(), [])


@unittest.skipUnless(os.environ.get('HGS_CODEX_TEST_BIN'), 'Set HGS_CODEX_TEST_BIN for the isolated native Codex question test')
class NativeCodexTallQuestion(unittest.TestCase):
    """A panel taller than its unviewed pane, opened in Terminal and left there."""
    questions = TALL
    script = NativeCodexAsyncQuestions.script
    write_record = NativeCodexAsyncQuestions.write_record
    tmux = NativeCodexAsyncQuestions.tmux
    tearDown = NativeCodexAsyncQuestions.tearDown
    fail = NativeCodexAsyncQuestions.fail
    wait_for = NativeCodexAsyncQuestions.wait_for
    inspect = NativeCodexAsyncQuestions.inspect
    native_features = NativeCodexAsyncQuestions.native_features
    setUp = NativeCodexAsyncQuestions.setUp
    screen = NativeCodexAsyncQuestions.screen
    cards = NativeCodexAsyncQuestions.cards
    answer = NativeCodexAsyncQuestions.answer
    replies = NativeCodexAsyncQuestions.replies
    reply = NativeCodexAsyncQuestions.reply
    deliver = NativeCodexAsyncQuestions.deliver
    finish = NativeCodexAsyncQuestions.finish

    def test_scrolled_answers_grow_the_unviewed_pane_and_answer_from_activity(self):
        height = lambda: int(self.tmux('display-message', '-p', '-t', self.pane, '#{window_height}').strip())
        before = height()
        self.tmux('send-keys', '-t', self.pane, 'S-Left')
        # Codex keeps the title and scrolls the answers, naming the visible row.
        self.wait_for(lambda: re.search(r'enter submit .*option 1/21', self.screen()))
        card = self.cards()[self.ids[0]]
        self.assertTrue(card['can_answer'], card['answer_unavailable_reason'])
        self.assertEqual(card['answer_transport'], 'codex_tui')
        self.assertGreater(height(), before)
        self.assertNotRegex(self.screen(), r'option \d+/\d+')
        self.deliver(card, dict(question_id='q_0', selected_option_ids=['opt_1']))
        self.finish([self.reply(card, TALL[0]['options'][1])])


if __name__ == '__main__':
    unittest.main()
