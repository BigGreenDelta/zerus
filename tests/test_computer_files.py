"""Native document reads in private state; never opens an owner's session or files."""
import base64
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import uuid

HGS = Path(os.environ.get('HGS_TEST_BIN', Path(__file__).resolve().parents[1] / 'target/debug/hgs')).resolve()


class ComputerFiles(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='zerus-file-read-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.state = self.root / 'state'; self.state.mkdir()
        self.work = self.root / 'project'; self.work.mkdir()
        self.home = self.root / 'home'; self.home.mkdir()
        self.name = 'codex/example/files'
        self.run = str(uuid.uuid4()); self.conversation = str(uuid.uuid4())
        self.record_path = self.state / (hashlib.sha256(self.name.encode()).hexdigest()+'.json')
        self.record = dict(version=1, name=self.name, agent='codex', run_id=self.run,
                           conversation_id=self.conversation, cwd=str(self.work), subagents={})
        self.record_path.write_text(json.dumps(self.record))
        self.env = {k:v for k,v in os.environ.items() if not k.startswith('HGS_')}
        self.env.update(HOME=str(self.home), HGS_STATE_DIR=str(self.state), HGS_CONFIG_DIR=str(self.root/'config'))

    def read(self, path, success=True, **change):
        payload = dict(request_id=str(uuid.uuid4()), expected_run_id=self.run,
                       expected_conversation_id=self.conversation, path=path)
        payload.update(change)
        result = subprocess.run([str(HGS), 'file-read', self.name, '--json'], input=json.dumps(payload),
                                env=self.env, text=True, capture_output=True, timeout=8)
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(result.stdout)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, '')
        return result.stderr

    def test_exact_binary_unicode_and_literal_shell_characters(self):
        path = self.work / 'Сводная $(literal); итог.xlsx'
        data = bytes(range(256))*37
        path.write_bytes(data)
        value = self.read(str(path))
        self.assertEqual(base64.b64decode(value['data_base64']), data)
        self.assertEqual(value['requested_path'], str(path))
        self.assertEqual(value['path'], str(path))
        self.assertEqual(value['file_name'], path.name)
        self.assertEqual(value['mime'], 'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet')
        self.assertEqual(value['sha256'], hashlib.sha256(data).hexdigest())
        self.assertFalse((self.state/'attachments').exists())

    def test_relative_path_and_home_are_resolved_by_original_computer(self):
        (self.work/'file.txt').write_bytes(b'project')
        (self.home/'file.txt').write_bytes(b'home')
        self.assertEqual(base64.b64decode(self.read('file.txt')['data_base64']), b'project')
        self.assertEqual(base64.b64decode(self.read('~/file.txt')['data_base64']), b'home')

    def test_stale_run_conversation_digest_and_unknown_child_return_no_bytes(self):
        path = self.work/'file.txt'; path.write_bytes(b'first')
        first = self.read(str(path))
        for change in [dict(expected_run_id='other'), dict(expected_conversation_id='other'), dict(agent_id='unknown'), dict(command='cat')]:
            self.read(str(path), success=False, **change)
        path.write_bytes(b'changed')
        self.assertIn('changed', self.read(str(path), success=False, sha256=first['sha256']))

    def test_missing_nonregular_empty_and_too_large_paths_are_clear_errors(self):
        self.assertIn('no longer exists', self.read(str(self.work/'missing.txt'), success=False))
        self.assertIn('regular file', self.read(str(self.work), success=False))
        path = self.work/'file.txt'; path.write_bytes(b'')
        self.assertIn('empty', self.read(str(path), success=False))
        with path.open('wb') as out: out.truncate(10*1024*1024+1)
        self.assertIn('10 MiB', self.read(str(path), success=False))


if __name__ == '__main__':
    unittest.main()
