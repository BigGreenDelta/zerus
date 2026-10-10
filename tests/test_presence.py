#!/usr/bin/env python3
"""Desktop presence and shared preferences through the real CLI with isolated state."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

REPO = Path(__file__).resolve().parents[1]
HGS = Path(os.environ.get('HGS_TEST_BIN', REPO / 'target/debug/hgs')).resolve()


class Presence(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='hgs-presence-')
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        (self.root / 'bin').mkdir()
        (self.root / 'config').mkdir()
        tmux = self.root / 'bin/tmux'
        tmux.write_text('#!/bin/sh\nif [ "$1" = -V ]; then echo "tmux 3.7"; fi\n')
        tmux.chmod(0o755)
        self.state = self.root / 'state'
        self.file = self.state / 'desktop-presence.json'
        self.env = dict(os.environ, HOME=str(self.root), HGS_CONFIG_DIR=str(self.root / 'config'),
                        HGS_STATE_DIR=str(self.state), HGS_TAB='0',
                        PATH=str(self.root / 'bin') + ':' + os.environ['PATH'])
        for key in ('HGS_PEERS', 'HGS_SELF', 'TMUX', 'TMUX_PANE', 'HGS_RUN_ID', 'HGS_SESSION', 'HGS_EXECUTABLE'):
            self.env.pop(key, None)

    def hgs(self, *args, data=None, ok=True):
        result = subprocess.run([str(HGS), *args], input=None if data is None else json.dumps(data),
                                env=self.env, text=True, capture_output=True, timeout=20)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(result.stdout)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        return result.stderr

    def listing(self):
        return self.hgs('ls', '--json', '--local')

    def test_presence_is_written_atomically_aged_and_expires(self):
        self.assertNotIn('desktop_presence', self.listing())
        before = time.time()
        self.assertEqual(self.hgs('presence', 'set', '--json', data={'idle_seconds': 3, 'locked': False}), {'ok': True})
        record = json.loads(self.file.read_text())
        self.assertEqual(set(record), {'idle_seconds', 'locked', 'locked_since', 'written_at'})
        self.assertEqual((record['idle_seconds'], record['locked'], record['locked_since']), (3, False, None))
        self.assertGreaterEqual(record['written_at'], before - 1)
        self.assertEqual([p.name for p in self.state.iterdir() if p.name.startswith('.tmp')], [])
        presence = self.listing()['desktop_presence']
        self.assertEqual(set(presence), {'idle_seconds', 'locked', 'locked_seconds'})
        self.assertGreaterEqual(presence['idle_seconds'], 3)
        self.assertLess(presence['idle_seconds'], 33)
        self.assertEqual((presence['locked'], presence['locked_seconds']), (False, None))
        for written_at in (time.time() - 121, time.time() + 600):
            self.file.write_text(json.dumps({**record, 'written_at': written_at}))
            self.assertNotIn('desktop_presence', self.listing())

    def test_lock_time_survives_consecutive_locked_writes(self):
        self.hgs('presence', 'set', '--json', data={'idle_seconds': 0, 'locked': True})
        first = json.loads(self.file.read_text())['locked_since']
        self.assertIsNotNone(first)
        time.sleep(0.05)
        self.hgs('presence', 'set', '--json', data={'idle_seconds': 40, 'locked': True})
        self.assertEqual(json.loads(self.file.read_text())['locked_since'], first)
        listed = self.listing()['desktop_presence']
        self.assertTrue(listed['locked'])
        self.assertGreaterEqual(listed['locked_seconds'], 0)
        self.hgs('presence', 'set', '--json', data={'idle_seconds': 1, 'locked': False})
        self.assertIsNone(json.loads(self.file.read_text())['locked_since'])

    def test_invalid_input_and_shapes_leave_the_record_unchanged(self):
        self.hgs('presence', 'set', '--json', data={'idle_seconds': 5, 'locked': False})
        original = self.file.read_bytes()
        for data in ({'idle_seconds': -1, 'locked': False}, {'idle_seconds': 1}, {'idle_seconds': '1', 'locked': False},
                     {'idle_seconds': 1, 'locked': 'no'}, {'idle_seconds': 1, 'locked': False, 'extra': 1}, [],
                     {'idle_seconds': 1e12, 'locked': False}):
            self.hgs('presence', 'set', '--json', data=data, ok=False)
        self.hgs('presence', 'set', data={'idle_seconds': 1, 'locked': False}, ok=False)
        self.hgs('--dry-run', 'presence', 'set', '--json', data={'idle_seconds': 1, 'locked': False}, ok=False)
        self.assertEqual(self.file.read_bytes(), original)


if __name__ == '__main__':
    unittest.main()
