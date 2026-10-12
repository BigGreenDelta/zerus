#!/usr/bin/env python3
"""Remote file downloads with isolated OpenSSH/profile/target fixtures."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
HGS = Path(os.environ.get('HGS_TEST_BIN', REPO / 'target/debug/hgs')).resolve()
FAKE_SCP = r'''#!/usr/bin/env python3
import json, os, pathlib, sys
root = pathlib.Path(os.environ['DOWNLOAD_FIXTURE'])
(root/'arguments.json').write_text(json.dumps(sys.argv[1:]))
assert '-s' in sys.argv and '-O' not in sys.argv
path = pathlib.Path(sys.argv[-1])
if os.environ.get('DOWNLOAD_FAIL'):
    path.write_bytes(b'partial transfer')
    print('Permission denied (publickey).', file=sys.stderr)
    sys.exit(1)
with path.open('wb') as out:
    with (root/'source').open('rb') as src:
        import shutil
        shutil.copyfileobj(src, out, 65536)
'''


class Downloads(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='zerus-download-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        config = self.root/'config'
        config.mkdir()
        (config/'config').write_text('HGS_SELF="fixture-local"\nHGS_PEERS="fixture-remote"\n')
        (config/'machines.local.json').write_text(json.dumps({'machines': {
            'fixture-remote': {'hostname': 'files.example', 'user': 'reader', 'port': 2222,
                               'identity_file': '~/Keys/test key', 'enabled': True}}}))
        self.env = {k: v for k, v in os.environ.items() if not k.startswith('HGS_')}
        self.env.update(HOME=str(self.root), HGS_CONFIG_DIR=str(config), HGS_STATE_DIR=str(self.root/'state'),
                        HGS_SSH=str(self.root/'ssh'), DOWNLOAD_FIXTURE=str(self.root))
        scp = self.root/'scp'
        scp.write_text(FAKE_SCP)
        scp.chmod(0o700)
        self.destination = self.root/'local copy.png'
        (self.root/'source').write_bytes(b'\x89PNG\r\n\x00\xffbinary\n')

    def invoke(self, path, success=True, extra=(), host='fixture-remote'):
        result = subprocess.run([str(HGS), '@'+host, 'download', '--path', path,
                                 '--output', str(self.destination), *extra], env=self.env,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode == 0, success, result.stderr)
        return result

    def test_literal_path_and_configured_ssh_profile_with_binary_file(self):
        path = "/remote/project/résumé ' $(literal); image.png"
        receipt = json.loads(self.invoke(path).stdout)
        args = json.loads((self.root/'arguments.json').read_text())
        self.assertEqual(args[-2], 'fixture-remote:'+path)
        self.assertIn('HostName=files.example', args)
        self.assertIn('User=reader', args)
        self.assertIn('StrictHostKeyChecking=yes', args)
        self.assertEqual(args[args.index('-P')+1], '2222')
        self.assertEqual(args[args.index('-i')+1], str(self.root/'Keys/test key'))
        self.assertEqual(args[args.index('-S')+1], str(self.root/'ssh'))
        self.assertEqual(self.destination.read_bytes(), (self.root/'source').read_bytes())
        self.assertEqual(self.destination.stat().st_mode & 0o777, 0o600)
        self.assertEqual(receipt['host'], 'fixture-remote')
        self.assertEqual(receipt['path'], path)
        self.assertEqual(receipt['local_path'], str(self.destination))
        self.assertEqual(receipt['bytes'], self.destination.stat().st_size)
        self.assertEqual(list(self.root.glob('.tmp*')), [])

    def test_failure_preserves_existing_copy_and_removes_partial_file(self):
        self.destination.write_bytes(b'previous copy')
        self.env['DOWNLOAD_FAIL'] = '1'
        result = self.invoke('/remote/missing.png', success=False)
        self.assertIn('Permission denied', result.stderr)
        self.assertEqual(self.destination.read_bytes(), b'previous copy')
        self.destination.unlink()
        self.invoke('/remote/missing.png', success=False)
        self.assertFalse(self.destination.exists())
        self.assertEqual(list(self.root.glob('.tmp*')), [])

    def test_empty_and_large_files_are_streamed_to_disk(self):
        for data in (b'', b'x'*(12*1024*1024)):
            (self.root/'source').write_bytes(data)
            result = self.invoke('~/reports/summary.bin')
            self.assertEqual(self.destination.read_bytes(), data)
            self.assertLess(len(result.stdout), 4096)

    def test_invalid_or_unconfigured_reference_never_starts_transfer(self):
        for path in ('relative.txt', '/file\ncommand'):
            self.invoke(path, success=False)
        self.invoke('/file', success=False, host='other-machine')
        self.invoke('/file', success=False, extra=('--path', '/other'))
        self.invoke('/file', success=False, extra=('--dry-run',))
        self.assertFalse((self.root/'arguments.json').exists())
        self.assertFalse(self.destination.exists())


if __name__ == '__main__':
    unittest.main()
