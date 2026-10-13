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
    def archive(self, *, name=compat.AGENTS['codex']['member'], symlink=False):
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

    def package(self, payload, agent='codex'):
        return {'url': compat.AGENTS[agent]['tarball'] + 'fixture.tgz',
                'integrity': 'sha512-' + base64.b64encode(hashlib.sha512(payload).digest()).decode()}

    def test_verified_archive_extracts_only_the_expected_executable(self):
        for agent, spec in compat.AGENTS.items():
            payload = self.archive(name=spec['member'])
            with self.subTest(agent), tempfile.TemporaryDirectory() as temporary, patch.object(
                    compat.urllib.request, 'urlopen', return_value=io.BytesIO(payload)):
                native = compat.install_native(agent, self.package(payload, agent), Path(temporary))
                self.assertEqual(native.name, agent)
                self.assertEqual(native.read_bytes(), b'synthetic-native-executable')
                self.assertEqual(native.stat().st_mode & 0o777, 0o755)
                self.assertEqual(list(Path(temporary).iterdir()), [native])

    def test_each_agent_accepts_only_its_own_official_distribution(self):
        payload = self.archive(name=compat.AGENTS['claude']['member'])
        for agent, other in (('codex', 'claude'), ('claude', 'codex')):
            with self.subTest(agent), tempfile.TemporaryDirectory() as temporary, patch.object(
                    compat.urllib.request, 'urlopen', return_value=io.BytesIO(payload)) as fetch:
                with self.assertRaisesRegex(ValueError, 'official ' + compat.AGENTS[agent]['name']):
                    compat.install_native(agent, self.package(payload, other), Path(temporary))
                fetch.assert_not_called()

    def test_reviewed_releases_match_the_pinned_packages(self):
        manifest = json.loads(compat.MANIFEST.read_text())
        reviewed = {row['name']: row['reviewed_versions'] for row in manifest['upstream']}
        for agent, spec in compat.AGENTS.items():
            pins = manifest[agent]
            self.assertTrue(pins, agent)
            self.assertEqual(reviewed[spec['name']], [package['version'] for package in pins])
            for package in pins:
                self.assertTrue(package['url'].startswith(spec['tarball'] + package['version']), package['url'])
                self.assertTrue(package['integrity'].startswith('sha512-'))

    def test_changed_package_bytes_fail_before_extraction(self):
        payload = self.archive()
        with tempfile.TemporaryDirectory() as temporary, patch.object(
                compat.urllib.request, 'urlopen', return_value=io.BytesIO(payload + b'changed')):
            with self.assertRaisesRegex(ValueError, 'integrity mismatch'):
                compat.install_native('codex', self.package(payload), Path(temporary))
            self.assertEqual(list(Path(temporary).iterdir()), [])

    def test_archive_paths_and_links_cannot_escape_the_fixture(self):
        for payload in (self.archive(name='../outside'), self.archive(symlink=True)):
            with self.subTest(), tempfile.TemporaryDirectory() as temporary, patch.object(
                    compat.urllib.request, 'urlopen', return_value=io.BytesIO(payload)):
                with self.assertRaises((ValueError, KeyError)):
                    compat.install_native('codex', self.package(payload), Path(temporary))
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

    def test_latest_claude_canary_uses_the_official_platform_package(self):
        manifest = json.loads(compat.MANIFEST.read_text())
        pinned = manifest['claude'][-1]['version']
        metadata = {'version': '9.8.7', 'dist': {
            'tarball': compat.AGENTS['claude']['tarball'] + '9.8.7.tgz', 'integrity': 'sha512-fixture'}}
        responses = {source['url']: {'version': '9.8.7'} if source['name'] == 'Claude Code' else OSError('fixture')
                     for source in manifest['upstream']}
        responses[compat.AGENTS['claude']['metadata'].format('9.8.7')] = metadata

        def fetch(url):
            if isinstance(responses[url], Exception):
                raise responses[url]
            return responses[url]

        def qualify(agent, package, work, results, hgs):
            directory = results / agent / package['version']
            directory.mkdir(parents=True)
            (directory / 'cli-help.txt').write_text('fixture help ' + package['version'] + '\n')
            return {'agent': agent, 'version': package['version'], 'status': 'passed',
                    'cli_help_sha256': package['version'], 'url': package['url']}

        with tempfile.TemporaryDirectory() as temporary, \
                patch.object(compat, 'fetch_json', side_effect=fetch), \
                patch.object(compat, 'qualify', side_effect=qualify), \
                patch.object(compat.sys, 'argv', ['agent-compatibility.py', '--latest', '--agent', 'claude',
                                                  '--results', temporary]):
            failed = compat.main()
            report = json.loads((Path(temporary) / 'report.json').read_text())
            summary = (Path(temporary) / 'summary.md').read_text()
        # Other discovery errors stay visible and fail the run.
        self.assertTrue(failed)
        self.assertEqual(report['canary_versions'], {'claude': '9.8.7'})
        self.assertEqual([(row['agent'], row['version']) for row in report['contracts']],
                         [('claude', pinned), ('claude', '9.8.7')])
        self.assertEqual(report['contracts'][-1]['url'], metadata['dist']['tarball'])
        self.assertEqual(report['cli_help_changed'], {'claude': True})
        self.assertIn('## Real Claude Code contracts', summary)
        self.assertNotIn('Codex contracts', summary)

    def test_native_runner_rejects_an_empty_or_skipped_suite(self):
        # Execute the actual CI runner with an unavailable native executable:
        # a unittest skip must never turn this compatibility gate green.
        import os
        import subprocess
        env = dict(os.environ, PYTHONPATH=str(ROOT / 'tests'))
        for spec in compat.AGENTS.values():
            env.pop(spec['variable'], None)
        for name, suites in [('empty', [])] + [(agent, spec['suites']) for agent, spec in compat.AGENTS.items()]:
            with self.subTest(name):
                result = subprocess.run([compat.sys.executable, '-c', compat.NATIVE_RUNNER, *suites], env=env,
                                        text=True, capture_output=True, timeout=30)
                self.assertNotEqual(result.returncode, 0)
                marker = next(line for line in result.stdout.splitlines() if line.startswith('ZERUS_NATIVE_RESULT='))
                counts = json.loads(marker.removeprefix('ZERUS_NATIVE_RESULT='))
                self.assertEqual(counts['skipped'], counts['tests'])

    def test_fixture_environment_does_not_inherit_live_accounts_or_session_selection(self):
        import os
        with tempfile.TemporaryDirectory() as temporary, patch.dict(os.environ, {
                'HGS_SESSION': 'fixture-live-session', 'HGS_RUN_ID': 'fixture-live-run',
                'HGS_ACCOUNT': 'fixture-account', 'CODEX_HOME': '/fixture-live-home',
                'CLAUDE_CONFIG_DIR': '/fixture-live-claude', 'CLAUDECODE': '1',
                'OPENAI_API_KEY': 'fixture-key', 'ANTHROPIC_API_KEY': 'fixture-key'}):
            home = Path(temporary) / 'home'
            env = compat.fixture_environment(home)
            self.assertEqual(env['HOME'], str(home))
            self.assertTrue(Path(env['CODEX_HOME']).is_dir())
            self.assertEqual(env['PYTHONPATH'], str(ROOT / 'tests'))
            self.assertEqual(env['CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC'], '1')
            for key in ('HGS_SESSION', 'HGS_RUN_ID', 'HGS_ACCOUNT', 'OPENAI_API_KEY', 'ANTHROPIC_API_KEY',
                        'CLAUDE_CONFIG_DIR', 'CLAUDECODE'):
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
