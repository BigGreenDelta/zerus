"""Distribution integrity, isolated contracts and upstream capability reporting."""
import base64
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('agent_compatibility', ROOT / 'scripts/ci/agent-compatibility.py')
compat = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compat)


class AgentCompatibility(unittest.TestCase):
    def archive(self, *, name=compat.NATIVE_MEMBER, symlink=False):
        stream = io.BytesIO()
        with tarfile.open(fileobj=stream, mode='w:gz') as tar:
            info = tarfile.TarInfo(name)
            payload = b'synthetic-native-executable'
            if symlink:
                info.type = tarfile.SYMTYPE
                info.linkname = '../../outside'
                tar.addfile(info)
            else:
                info.size = len(payload)
                tar.addfile(info, io.BytesIO(payload))
        return stream.getvalue()

    def package(self, payload):
        return {'url': 'https://registry.npmjs.org/@openai/codex/-/codex-fixture.tgz',
                'integrity': 'sha512-' + base64.b64encode(hashlib.sha512(payload).digest()).decode()}

    def test_verified_archive_extracts_only_the_expected_executable(self):
        payload = self.archive()
        with tempfile.TemporaryDirectory() as temporary, patch.object(
                compat.urllib.request, 'urlopen', return_value=io.BytesIO(payload)):
            native = compat.install_native(self.package(payload), Path(temporary))
            self.assertEqual(native.read_bytes(), b'synthetic-native-executable')
            self.assertEqual(native.stat().st_mode & 0o777, 0o755)
            self.assertEqual(list(Path(temporary).iterdir()), [native])

    def test_changed_package_bytes_fail_before_extraction(self):
        payload = self.archive()
        with tempfile.TemporaryDirectory() as temporary, patch.object(
                compat.urllib.request, 'urlopen', return_value=io.BytesIO(payload + b'changed')):
            with self.assertRaisesRegex(ValueError, 'integrity mismatch'):
                compat.install_native(self.package(payload), Path(temporary))
            self.assertEqual(list(Path(temporary).iterdir()), [])

    def test_archive_paths_and_links_cannot_escape_the_fixture(self):
        for payload in (self.archive(name='../outside'), self.archive(symlink=True)):
            with self.subTest(), tempfile.TemporaryDirectory() as temporary, patch.object(
                    compat.urllib.request, 'urlopen', return_value=io.BytesIO(payload)):
                with self.assertRaises((ValueError, KeyError)):
                    compat.install_native(self.package(payload), Path(temporary))
                self.assertEqual(list(Path(temporary).iterdir()), [])

    def test_schema_diff_finds_new_methods_and_changes_to_existing_fields(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            path = directory / 'ClientRequest.json'
            path.write_text(json.dumps({'oneOf': [{'properties': {
                'method': {'enum': ['thread/start']}, 'params': {'type': 'object'}}}]}))
            before = compat.schema_snapshot(directory)
            path.write_text(json.dumps({'oneOf': [
                {'properties': {'method': {'enum': ['thread/start']}, 'params': {'required': ['cwd']}}},
                {'properties': {'method': {'const': 'thread/newCapability'}}}]}))
            changes = compat.schema_changes(before, compat.schema_snapshot(directory))
            self.assertEqual(changes['added_methods'], ['thread/newCapability'])
            self.assertEqual(changes['changed_schemas'], ['ClientRequest.json'])
            self.assertEqual(changes['removed_methods'], [])

    def test_release_discovery_keeps_errors_and_unreviewed_versions_visible(self):
        sources = json.loads(compat.MANIFEST.read_text())['upstream'][:2]
        with patch.object(compat, 'fetch_json', side_effect=[{'version': '9.8.7'}, OSError('fixture unavailable')]):
            rows = compat.discover_upstream({'upstream': sources})
        self.assertEqual(rows[0]['review'], 'needs review')
        self.assertEqual(rows[0]['reviewed_versions'], ['0.160.1', '0.162.0'])
        self.assertEqual(rows[1]['error'], 'fixture unavailable')

    def test_native_runner_rejects_an_empty_or_skipped_suite(self):
        # Execute the actual CI runner with an unavailable native executable:
        # a unittest skip must never turn this compatibility gate green.
        import os
        import subprocess
        env = dict(os.environ, PYTHONPATH=str(ROOT / 'tests'))
        env.pop('HGS_CODEX_TEST_BIN', None)
        result = subprocess.run([compat.sys.executable, '-c', compat.NATIVE_RUNNER], env=env,
                                text=True, capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('ZERUS_NATIVE_RESULT=', result.stdout)

    def test_fixture_environment_does_not_inherit_live_accounts_or_session_selection(self):
        import os
        with tempfile.TemporaryDirectory() as temporary, patch.dict(os.environ, {
                'HGS_SESSION': 'fixture-live-session', 'HGS_RUN_ID': 'fixture-live-run',
                'HGS_ACCOUNT': 'fixture-account', 'CODEX_HOME': '/fixture-live-home',
                'OPENAI_API_KEY': 'fixture-key', 'ANTHROPIC_API_KEY': 'fixture-key'}):
            home = Path(temporary) / 'home'
            env = compat.fixture_environment(home)
            self.assertEqual(env['HOME'], str(home))
            self.assertTrue(Path(env['CODEX_HOME']).is_dir())
            self.assertEqual(env['PYTHONPATH'], str(ROOT / 'tests'))
            for key in ('HGS_SESSION', 'HGS_RUN_ID', 'HGS_ACCOUNT', 'OPENAI_API_KEY', 'ANTHROPIC_API_KEY'):
                self.assertNotIn(key, env)

    def test_canary_has_no_publication_credentials_or_write_access(self):
        workflow = (ROOT / '.github/workflows/agent-upstream.yml').read_text()
        self.assertIn('contents: read', workflow)
        self.assertIn('cache-mode: none', workflow)
        self.assertIn("github.repository == 'ufna/zerus'", workflow)
        self.assertIn("github.ref == 'refs/heads/main'", workflow)
        self.assertNotIn('pull_request', workflow)
        self.assertNotIn('secrets.', workflow)
        self.assertNotRegex(workflow, r'actions/cache[@/]')
        self.assertIn('persist-credentials: false', workflow)
        self.assertIn('retention-days: 3', workflow)


if __name__ == '__main__':
    unittest.main()
