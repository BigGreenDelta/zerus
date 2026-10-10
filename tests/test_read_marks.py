"""Shared read marks on the session's host: `hgs ls --json` fields and `hgs read`."""
import json
import re
import sqlite3
import subprocess
import unittest

import test_input as fixtures

HGS = fixtures.HGS
HEX = re.compile(r'[0-9a-f]{64}')


class ReadMarks(unittest.TestCase):
    setUp = fixtures.InputTransport.setUp
    tearDown = fixtures.InputTransport.tearDown
    script = fixtures.InputTransport.script
    tmux = fixtures.InputTransport.tmux
    wait_for = fixtures.InputTransport.wait_for
    write_record = fixtures.InputTransport.write_record

    def listing(self):
        result = subprocess.run([str(HGS), 'ls', '--json', '--local'], env=self.env,
                                text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        return next(row for row in json.loads(result.stdout)['sessions'] if row['name'] == self.name)

    def reply(self, at):
        db = sqlite3.connect(self.state / 'events.sqlite3')
        with db:
            db.execute('CREATE TABLE IF NOT EXISTS events (seq INTEGER PRIMARY KEY, name TEXT, conversation TEXT, payload TEXT)')
            db.execute('INSERT INTO events(name, conversation, payload) VALUES (?, ?, ?)',
                       (self.name, self.conversation_id, json.dumps({'type': 'Stop', 'agent_id': '', 'at': at})))
        db.close()
        return self.listing()['reply_id']

    def waiting(self, tool='call-1'):
        self.record.update(phase='input', activity='busy',
                           active_tools={tool: dict(name='request_user_input', started=1.0, detail='')})
        self.write_record()

    def test_idle_session_lists_null_read_and_signature(self):
        row = self.listing()
        self.assertIn('read', row)
        self.assertIsNone(row['read'])
        self.assertIsNone(row['attention_signature'])

    def test_signature_ignores_status_updates_and_changes_for_a_new_request(self):
        self.waiting()
        first = self.listing()['attention_signature']
        self.assertRegex(first, HEX)
        self.record.update(last_event_at=self.record['last_event_at'] + 50, prompt='A different status line',
                           subagents={'worker': dict(state='working', name='Worker', updated=5.0)})
        self.write_record()
        self.assertEqual(self.listing()['attention_signature'], first)
        self.waiting('call-2')
        second = self.listing()['attention_signature']
        self.assertRegex(second, HEX)
        self.assertNotEqual(second, first)
        self.record.update(phase='working', active_tools={})
        self.write_record()
        self.assertIsNone(self.listing()['attention_signature'])

    def test_repeated_identity_less_errors_have_distinct_signatures(self):
        self.record.update(phase='error', activity='attention', active_tools={}, turn_started=100.0)
        self.write_record()
        first = self.listing()['attention_signature']
        self.assertRegex(first, HEX)
        self.record.update(phase='working', activity='busy', turn_started=200.0)
        self.write_record()
        self.assertIsNone(self.listing()['attention_signature'])
        self.record.update(phase='error', activity='attention')
        self.write_record()
        second = self.listing()['attention_signature']
        self.assertRegex(second, HEX)
        self.assertNotEqual(second, first)


if __name__ == '__main__':
    unittest.main()
