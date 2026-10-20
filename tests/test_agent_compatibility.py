"""Distribution integrity, isolated contracts and upstream capability reporting."""
import base64
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('agent_compatibility', ROOT / 'scripts/ci/agent-compatibility.py')
compat = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compat)


class AgentCompatibility(unittest.TestCase):
    def archive(self, *, name=compat.AGENTS['codex']['members']['linux-x64'], symlink=False, others=()):
        stream = io.BytesIO()
        with tarfile.open(fileobj=stream, mode='w:gz') as tar:
            for other in others:
                info = tarfile.TarInfo(other)
                info.size = len(b'synthetic-helper')
                tar.addfile(info, io.BytesIO(b'synthetic-helper'))
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

    def package(self, payload, agent='codex', target='linux-x64'):
        return {'version': '1.2.3', 'target': target,
                'url': compat.AGENTS[agent]['tarball'].format(version='1.2.3', target=target),
                'integrity': 'sha512-' + base64.b64encode(hashlib.sha512(payload).digest()).decode()}

    def test_verified_archive_extracts_only_the_expected_executable(self):
        for agent, target in [(agent, target) for agent, spec in compat.AGENTS.items() for target in spec['members']]:
            payload = self.archive(name=compat.AGENTS[agent]['members'][target])
            with self.subTest(agent=agent, target=target), tempfile.TemporaryDirectory() as temporary, patch.object(
                    compat.urllib.request, 'urlopen', return_value=io.BytesIO(payload)):
                native = compat.install_native(agent, self.package(payload, agent, target), Path(temporary))
                self.assertEqual(native.name, agent + ('.exe' if target == 'win32-x64' else ''))
                self.assertEqual(native.read_bytes(), b'synthetic-native-executable')
                self.assertEqual(native.stat().st_mode & 0o777, 0o755)
                self.assertEqual(list(Path(temporary).iterdir()), [native])

    def test_windows_codex_helpers_are_never_extracted(self):
        # The sandbox setup program must never run; nothing but the CLI leaves the archive.
        vendor = 'package/vendor/x86_64-pc-windows-msvc/'
        helpers = [vendor + name for name in (
            'codex-resources/codex-windows-sandbox-setup.exe', 'codex-resources/codex-command-runner.exe',
            'bin/codex-code-mode-host.exe', 'codex-path/rg.exe', 'codex-resources/voice/bin/vcruntime140.dll',
            'codex-package.json')]
        payload = self.archive(name=compat.AGENTS['codex']['members']['win32-x64'], others=helpers)
        with tempfile.TemporaryDirectory() as temporary, patch.object(
                compat.urllib.request, 'urlopen', return_value=io.BytesIO(payload)):
            native = compat.install_native('codex', self.package(payload, 'codex', 'win32-x64'), Path(temporary))
            self.assertEqual(native, Path(temporary) / 'codex.exe')
            self.assertEqual([path.name for path in Path(temporary).rglob('*')], ['codex.exe'])

    def test_each_agent_accepts_only_its_own_official_distribution(self):
        payload = self.archive(name=compat.AGENTS['claude']['members']['linux-x64'])
        darwin = self.package(payload, 'claude', 'darwin-arm64')
        opencode = self.package(payload, 'opencode', 'linux-x64-baseline')
        cases = [('codex', self.package(payload, 'claude')), ('claude', self.package(payload, 'codex')),
                 ('opencode', self.package(payload, 'claude')), ('claude', opencode),
                 # A target must match its URL, and Codex has no reviewed macOS package.
                 ('claude', dict(darwin, target='linux-x64')), ('codex', dict(self.package(payload), target='darwin-arm64')),
                 # OpenCode's x86_64 Linux pin is the baseline package, and it has no Windows package.
                 ('opencode', dict(opencode, target='linux-x64')), ('opencode', dict(opencode, target='win32-x64'))]
        for agent, package in cases:
            with self.subTest(agent=agent, url=package['url'], target=package['target']), \
                    tempfile.TemporaryDirectory() as temporary, patch.object(
                    compat.urllib.request, 'urlopen', return_value=io.BytesIO(payload)) as fetch:
                with self.assertRaisesRegex(ValueError, 'official ' + compat.AGENTS[agent]['name']):
                    compat.install_native(agent, package, Path(temporary))
                fetch.assert_not_called()

    def test_reviewed_releases_match_the_pinned_packages(self):
        manifest = json.loads(compat.MANIFEST.read_text())
        reviewed = {row['name']: row['reviewed_versions'] for row in manifest['upstream']}
        compat.check_manifest(manifest)
        for agent, spec in compat.AGENTS.items():
            pins = manifest[agent]
            versions = list(dict.fromkeys(package['version'] for package in pins))
            self.assertTrue(pins, agent)
            self.assertEqual(reviewed[spec['name']], versions)
            # Every current version is pinned exactly once for each supported target.
            current = [version for version in versions
                       if not any(package.get('retained') for package in pins if package['version'] == version)]
            self.assertEqual(sorted((package['version'], package['target']) for package in pins
                                    if package['version'] in current),
                             sorted((version, target) for version in current for target in spec['members']))
            for package in pins:
                self.assertEqual(package['url'], spec['tarball'].format(**package))
                self.assertTrue(package['integrity'].startswith('sha512-'))
        # Codex 0.160.1 stays a Linux-only regression boundary for resident sessions.
        self.assertEqual([(package['version'], package['target']) for package in manifest['codex']
                          if package.get('retained')], [('0.160.1', 'linux-x64')])
        self.assertEqual({package['target'] for package in manifest['claude']}, {'linux-x64', 'darwin-arm64', 'win32-x64'})
        # OpenCode pins its reviewed minimum, which its canary never moves.
        self.assertEqual(sorted((package['version'], package['target']) for package in manifest['opencode']),
                         [('2.0.32', 'darwin-arm64'), ('2.0.32', 'linux-x64-baseline')])
        policies = {row['name']: row.get('policy') for row in manifest['upstream']}
        self.assertEqual(policies['OpenCode'], 'minimum')
        self.assertEqual({name for name, policy in policies.items() if policy}, {'OpenCode'})

    def test_only_an_older_retained_version_may_skip_a_target(self):
        def pin(version, target, **extra):
            return dict(version=version, target=target, **extra)

        retained = dict(retained='fixture regression boundary')
        claude = [pin('2.0.0', target) for target in compat.AGENTS['claude']['members']]
        codex = [pin('1.0.0', 'linux-x64', **retained), pin('1.1.0', 'linux-x64'), pin('1.1.0', 'win32-x64')]
        opencode = [pin('2.0.0', target) for target in compat.AGENTS['opencode']['members']]
        compat.check_manifest({'codex': codex, 'claude': claude, 'opencode': opencode})
        for label, pins, message in [
                ('current version without Windows', codex[:2], 'needs a pin for every target'),
                ('newest version retained', [pin('1.1.0', 'linux-x64', **retained)], 'only an older'),
                ('retained without a reason', [pin('1.0.0', 'linux-x64', retained=''), *codex[1:]], 'only an older'),
                ('retained on one target only', [*codex[:1], pin('1.0.0', 'win32-x64'), *codex[1:]],
                 'some targets only'),
                ('target pinned twice', [*codex, pin('1.1.0', 'win32-x64')], 'one target twice'),
                ('unsupported target', [*codex, pin('1.1.0', 'darwin-arm64')], 'unsupported target'),
                ('no version', [], 'no reviewed version')]:
            with self.subTest(label), self.assertRaisesRegex(ValueError, message):
                compat.check_manifest({'codex': pins, 'claude': claude, 'opencode': opencode})

    def test_changed_package_bytes_fail_before_extraction(self):
        payload = self.archive()
        with tempfile.TemporaryDirectory() as temporary, patch.object(
                compat.urllib.request, 'urlopen', return_value=io.BytesIO(payload + b'changed')):
            with self.assertRaisesRegex(ValueError, 'integrity mismatch'):
                compat.install_native('codex', self.package(payload), Path(temporary))
            self.assertEqual(list(Path(temporary).iterdir()), [])

    def test_a_dropped_download_is_retried_and_still_verified(self):
        payload = self.archive()
        dropped = ConnectionResetError(10054, 'An existing connection was forcibly closed by the remote host')
        with tempfile.TemporaryDirectory() as temporary, patch.object(compat.time, 'sleep') as sleep, \
                patch.object(compat.urllib.request, 'urlopen', side_effect=[dropped, io.BytesIO(payload)]) as fetch:
            native = compat.install_native('codex', self.package(payload), Path(temporary))
            self.assertEqual(native.read_bytes(), b'synthetic-native-executable')
            self.assertEqual(fetch.call_count, 2)
            sleep.assert_called_once()

    def test_refused_or_changed_downloads_are_not_retried(self):
        payload = self.archive()
        missing = compat.urllib.error.HTTPError('https://registry.example.test', 404, 'Not Found', {}, None)
        for response, error in [(missing, compat.urllib.error.HTTPError),
                                (io.BytesIO(payload + b'changed'), ValueError)]:
            with self.subTest(error=error.__name__), tempfile.TemporaryDirectory() as temporary, \
                    patch.object(compat.time, 'sleep') as sleep, \
                    patch.object(compat.urllib.request, 'urlopen', side_effect=[response]) as fetch:
                with self.assertRaises(error):
                    compat.install_native('codex', self.package(payload), Path(temporary))
                self.assertEqual(fetch.call_count, 1)
                sleep.assert_not_called()
                self.assertEqual(list(Path(temporary).iterdir()), [])

    def test_a_download_that_keeps_dropping_fails_after_its_attempts(self):
        payload = self.archive()
        with tempfile.TemporaryDirectory() as temporary, patch.object(compat.time, 'sleep'), \
                patch.object(compat.urllib.request, 'urlopen', side_effect=ConnectionResetError(10054, 'reset')) as fetch:
            with self.assertRaises(ConnectionResetError):
                compat.install_native('codex', self.package(payload), Path(temporary))
            self.assertEqual(fetch.call_count, compat.DOWNLOAD_ATTEMPTS)
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

    def test_opencode_releases_are_labelled_against_the_reviewed_minimum(self):
        [source] = [row for row in json.loads(compat.MANIFEST.read_text())['upstream'] if row['name'] == 'OpenCode']
        for published, review in [('2.0.32', 'same as the reviewed minimum'),
                                  ('2.1.0', 'newer than the reviewed minimum; see its canary'),
                                  ('2.0.9', 'older than the reviewed minimum'),
                                  ('3.0.0', 'another major version than the reviewed minimum; needs review'),
                                  ('1.18.35', 'another major version than the reviewed minimum; needs review'),
                                  ('2.0.33-beta.1', 'needs review')]:
            with self.subTest(published), patch.object(compat, 'fetch_json', return_value={'version': published}):
                [row] = compat.discover_upstream({'upstream': [source]})
                self.assertEqual(row['review'], review)

    def test_latest_claude_canary_uses_the_official_platform_package(self):
        manifest = json.loads(compat.MANIFEST.read_text())
        pinned = next(package['version'] for package in reversed(manifest['claude']) if package['target'] == 'linux-x64')
        metadata = {'version': '9.8.7', 'dist': {
            'tarball': compat.AGENTS['claude']['tarball'].format(version='9.8.7', target='linux-x64'),
            'integrity': 'sha512-fixture'}}
        responses = {source['url']: {'version': '9.8.7'} if source['name'] == 'Claude Code' else OSError('fixture')
                     for source in manifest['upstream']}
        responses[compat.AGENTS['claude']['metadata'].format(version='9.8.7', target='linux-x64')] = metadata

        def fetch(url):
            if isinstance(responses[url], Exception):
                raise responses[url]
            return responses[url]

        def qualify(agent, package, work, results, hgs):
            directory = results / agent / package['version']
            directory.mkdir(parents=True)
            (directory / 'cli-help.txt').write_text('fixture help ' + package['version'] + '\n')
            return {'agent': agent, 'version': package['version'], 'target': package['target'], 'status': 'passed',
                    'cli_help_sha256': package['version'], 'url': package['url']}

        with tempfile.TemporaryDirectory() as temporary, \
                patch.object(compat, 'fetch_json', side_effect=fetch), \
                patch.object(compat, 'qualify', side_effect=qualify), \
                patch.object(compat, 'host_target', return_value='linux-x64'), \
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

    def test_latest_opencode_canary_tests_the_newest_release_without_moving_its_minimum(self):
        manifest = json.loads(compat.MANIFEST.read_text())
        before = compat.MANIFEST.read_bytes()
        metadata = {'version': '2.1.0', 'dist': {
            'tarball': compat.AGENTS['opencode']['tarball'].format(version='2.1.0', target='linux-x64-baseline'),
            'integrity': 'sha512-fixture'}}

        def published(source):
            value = '2.1.0' if source['name'] == 'OpenCode' else (source['reviewed_versions'] or ['0.0.1'])[-1]
            for key in reversed(source['version_path']):
                value = {key: value}
            return value

        responses = {source['url']: published(source) for source in manifest['upstream']}
        responses[compat.AGENTS['opencode']['metadata'].format(version='2.1.0', target='linux-x64-baseline')] = metadata

        def qualify(agent, package, work, results, hgs):
            return {'agent': agent, 'version': package['version'], 'target': package['target'], 'status': 'passed'}

        with tempfile.TemporaryDirectory() as temporary, \
                patch.object(compat, 'fetch_json', side_effect=lambda url: responses[url]), \
                patch.object(compat, 'qualify', side_effect=qualify), \
                patch.object(compat, 'host_target', return_value='linux-x64'), \
                patch.object(compat.sys, 'argv', ['agent-compatibility.py', '--latest', '--agent', 'opencode',
                                                  '--results', temporary]):
            failed = compat.main()
            report = json.loads((Path(temporary) / 'report.json').read_text())
            summary = (Path(temporary) / 'summary.md').read_text()
        self.assertFalse(failed, report['errors'])
        self.assertEqual(report['canary_versions'], {'opencode': '2.1.0'})
        self.assertEqual([(row['version'], row['target']) for row in report['contracts']],
                         [('2.0.32', 'linux-x64-baseline'), ('2.1.0', 'linux-x64-baseline')])
        row = next(row for row in report['upstream'] if row['name'] == 'OpenCode')
        self.assertEqual(row['review'], 'newer than the reviewed minimum; see its canary')
        self.assertIn('## Real OpenCode contracts', summary)
        self.assertEqual(compat.MANIFEST.read_bytes(), before)

    def test_host_target_selects_its_pinned_agents_and_rejects_unreviewed_ones(self):
        qualified = []

        def qualify(agent, package, work, results, hgs):
            qualified.append((agent, package['version'], package['target']))
            return {'agent': agent, 'version': package['version'], 'target': package['target'], 'status': 'passed'}

        for target, arguments, expected in [
                # OpenCode runs its baseline x86_64 package on Linux.
                ('linux-x64', [], [('codex', '0.160.1', 'linux-x64'), ('codex', '0.162.0', 'linux-x64'),
                                   ('claude', '2.1.296', 'linux-x64'), ('opencode', '2.0.32', 'linux-x64-baseline')]),
                ('linux-x64', ['--agent', 'opencode'], [('opencode', '2.0.32', 'linux-x64-baseline')]),
                ('darwin-arm64', [], [('claude', '2.1.296', 'darwin-arm64'), ('opencode', '2.0.32', 'darwin-arm64')]),
                ('darwin-arm64', ['--agent', 'codex'], SystemExit),
                ('win32-x64', ['--agent', 'opencode'], SystemExit),
                # The retained Codex 0.160.1 has no Windows pin.
                ('win32-x64', [], [('codex', '0.162.0', 'win32-x64'), ('claude', '2.1.296', 'win32-x64')]),
                ('win32-arm64', [], SystemExit),
                (None, [], SystemExit)]:
            qualified.clear()
            with self.subTest(target=target, arguments=arguments), tempfile.TemporaryDirectory() as temporary, \
                    patch.object(compat, 'qualify', side_effect=qualify), \
                    patch.object(compat, 'host_target', return_value=target), \
                    patch.object(compat.sys, 'argv', ['agent-compatibility.py', '--results', temporary, *arguments]), \
                    patch('sys.stderr', io.StringIO()):
                if expected is SystemExit:
                    with self.assertRaises(SystemExit):
                        compat.main()
                    self.assertEqual(qualified, [])
                else:
                    self.assertFalse(compat.main())
                    self.assertEqual(qualified, expected)
                    summary = (Path(temporary) / 'summary.md').read_text()
                    for _, _, package_target in expected:
                        self.assertIn('(' + package_target + '): passed', summary)

    def test_host_target_names_64_bit_windows_as_its_npm_packages_do(self):
        for system, machine, expected in [('Windows', 'AMD64', 'win32-x64'), ('Windows', 'ARM64', 'win32-arm64'),
                                          ('Linux', 'x86_64', 'linux-x64'), ('Darwin', 'arm64', 'darwin-arm64'),
                                          ('FreeBSD', 'amd64', None)]:
            with self.subTest(system=system, machine=machine), \
                    patch.object(compat.platform, 'system', return_value=system), \
                    patch.object(compat.platform, 'machine', return_value=machine):
                self.assertEqual(compat.host_target(), expected)

    def test_host_targets_map_only_where_an_agent_names_another_package(self):
        self.assertEqual(compat.agent_target('opencode', 'linux-x64'), 'linux-x64-baseline')
        self.assertEqual(compat.agent_target('opencode', 'darwin-arm64'), 'darwin-arm64')
        for agent in ('codex', 'claude'):
            for target in ('linux-x64', 'darwin-arm64', 'win32-x64'):
                self.assertEqual(compat.agent_target(agent, target), target)

    def test_windows_runs_only_the_session_host_suites(self):
        classes = {'claude': 'NativeWindowsClaude', 'codex': 'NativeWindowsCodex'}
        self.assertEqual({agent for agent, spec in compat.AGENTS.items() if 'win32-x64' in spec['members']}, set(classes))
        for agent in classes:
            spec = compat.AGENTS[agent]
            windows = compat.target_spec(agent, 'win32-x64')
            self.assertEqual(windows['suites'], ['test_windows_native_agents.' + classes[agent]])
            self.assertEqual(windows['variable'], spec['variable'])
            self.assertFalse([name for name in spec['suites'] if 'windows' in name])
            for target in set(spec['members']) - {'win32-x64'}:
                with self.subTest(agent=agent, target=target):
                    self.assertIs(compat.target_spec(agent, target), spec)

    def qualify_fixture(self, agent, target):
        """Run qualify() with stand-ins for the package, the executable and the suite
        interpreter; returns the report row, the suite runs, the results folder and the
        profile each probe of the executable saw."""
        runs, profiles = [], []
        self.probe_environments = []

        def run(argv, **options):
            self.assertIs(options['stdin'], compat.subprocess.DEVNULL)
            runs.append((argv[3:], options['env'], options['timeout']))
            options['stdout'].write(b'ZERUS_NATIVE_RESULT={"tests": 2, "skipped": 0}\n')
            options['stderr'].write('ok: \u041f\u0440\u0438\u0432\u0435\u0442\n'.encode())
            return SimpleNamespace(returncode=0)

        def probe(native, arguments, env, timeout=60):
            profiles.append(env['USERPROFILE'])
            self.probe_environments.append(env)
            if arguments == ['--version']:
                return compat.AGENTS[agent]['version_output'].format('1.2.3') + '\n'
            if arguments[:2] == ['app-server', 'generate-json-schema']:
                Path(arguments[3]).mkdir(parents=True)
                (Path(arguments[3]) / 'ClientRequest.json').write_text(
                    json.dumps({'properties': {'method': {'const': 'thread/start'}}}))
            return 'fixture help\n'

        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        work, results = Path(temporary.name) / 'work', Path(temporary.name) / 'results'
        (work / agent / '1.2.3').mkdir(parents=True)
        package = {'version': '1.2.3', 'target': target, 'integrity': 'sha512-fixture'}
        with patch.object(compat, 'install_native', return_value=work / agent / '1.2.3' / 'native'), \
                patch.object(compat, 'probe', side_effect=probe), \
                patch.object(compat.subprocess, 'run', side_effect=run), \
                patch.dict(compat.os.environ, {'HGS_TEST_CONPTY_DIR': 'C:\\fixture\\conpty',
                                               'HGS_SESSION': 'fixture-live-session',
                                               'XDG_STATE_HOME': '/fixture-live-state',
                                               'USERPROFILE': 'C:\\Users\\fixture-runner'}), \
                patch('sys.stdout', io.StringIO()):
            row = compat.qualify(agent, package, work, results, Path('hgs-under-test'))
        return row, runs, results / agent / '1.2.3', profiles

    def test_windows_qualifies_each_agent_once_through_the_session_host(self):
        for agent, spec in compat.AGENTS.items():
            if 'win32-x64' not in spec['members']:
                continue
            with self.subTest(agent=agent):
                row, runs, directory, profiles = self.qualify_fixture(agent, 'win32-x64')
                self.assertEqual(row['status'], 'passed', row.get('error'))
                self.assertEqual(row['cases'], {'default': {'tests': 2, 'skipped': 0}})
                [(suites, env, timeout)] = runs
                self.assertEqual((suites, timeout), (spec['windows']['suites'], spec['windows']['timeout']))
                self.assertEqual(env['HGS_TEST_BIN'], 'hgs-under-test')
                self.assertEqual(Path(env[spec['variable']]).name, 'native')
                # The pinned ConPTY pair reaches the suite; the live session does not.
                self.assertEqual(env['HGS_TEST_CONPTY_DIR'], 'C:\\fixture\\conpty')
                self.assertNotIn('HGS_SESSION', env)
                self.assertNotIn('HGS_CODEX_TEST_SIZE', env)
                # The probes find a private profile; the suite gets the real one, by
                # which it places projects where Codex prints their full path.
                self.assertEqual(set(profiles), {env['HOME']})
                self.assertEqual(len(profiles), 4 if agent == 'codex' else 2)
                self.assertEqual(env['USERPROFILE'], 'C:\\Users\\fixture-runner')
                # The log keeps stdout, then stderr, as the suite wrote them.
                self.assertEqual((directory / 'default.log').read_bytes().decode(),
                                 'ZERUS_NATIVE_RESULT={"tests": 2, "skipped": 0}\nok: \u041f\u0440\u0438\u0432\u0435\u0442\n')

    def test_linux_codex_keeps_its_terminal_size_and_animation_cases(self):
        row, runs, directory, profiles = self.qualify_fixture('codex', 'linux-x64')
        self.assertEqual(row['status'], 'passed', row.get('error'))
        self.assertEqual(sorted(row['cases']), ['110x35-animated', '110x35-reduced', '95x47-animated', '95x47-reduced'])
        self.assertEqual([suites for suites, env, timeout in runs], [compat.AGENTS['codex']['suites']] * 4)
        self.assertEqual({env['HGS_CODEX_TEST_SIZE'] for suites, env, timeout in runs}, {'95x47', '110x35'})
        self.assertEqual(row['schema_stable']['methods'], ['thread/start'])
        self.assertEqual(set(profiles), {'C:\\Users\\fixture-runner'})

    def test_opencode_suite_is_opted_in_and_probes_keep_private_xdg_folders(self):
        for target in compat.AGENTS['opencode']['members']:
            with self.subTest(target=target):
                row, runs, directory, profiles = self.qualify_fixture('opencode', target)
                self.assertEqual(row['status'], 'passed', row.get('error'))
                [(suites, env, timeout)] = runs
                self.assertEqual((suites, timeout), (['test_opencode_native'], compat.AGENTS['opencode']['timeout']))
                self.assertEqual(env['ZERUS_TEST_OPENCODE_NATIVE'], '1')
                self.assertEqual(Path(env['ZERUS_TEST_OPENCODE_BIN']).name, 'native')
                # The suite builds its own private folders; the runner's version and help
                # probes never write OpenCode's logs or state into the caller's.
                self.assertEqual(len(self.probe_environments), 2)
                for probe_env in self.probe_environments:
                    for key in ('XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME', 'XDG_STATE_HOME'):
                        self.assertTrue(Path(probe_env[key]).is_relative_to(probe_env['HOME']), key)
        for agent in ('codex', 'claude'):
            self.qualify_fixture(agent, 'linux-x64')
            self.assertTrue(all(env.get('XDG_STATE_HOME') == '/fixture-live-state' for env in self.probe_environments))

    def test_a_suite_that_times_out_keeps_its_log(self):
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / 'default.log'

            def run(argv, **options):
                self.assertEqual(options['timeout'], 5)
                self.assertEqual(options['env']['ZERUS_NATIVE_DIAGNOSTIC_DEADLINE'], '105')
                options['stderr'].write(b'test_launch ... ')
                raise compat.subprocess.TimeoutExpired(argv, options['timeout'])

            with patch.object(compat.subprocess, 'run', side_effect=run), \
                    patch.object(compat.time, 'monotonic', return_value=100), patch('sys.stdout', io.StringIO()), \
                    self.assertRaises(compat.subprocess.TimeoutExpired):
                compat.run_suites(['fixture'], {}, 5, Path(temporary), log)
            self.assertEqual(log.read_bytes(), b'test_launch ... ')

    def test_native_runner_diagnostics_record_the_current_case_without_changing_outcomes(self):
        import faulthandler
        import os
        import threading
        import time
        for passing in (True, False):
            timer, invoked, namespace = SimpleNamespace(daemon=False), [], {}
            timer.start, timer.cancel = unittest.mock.Mock(), unittest.mock.Mock()

            def make_timer(delay, callback):
                timer.delay, timer.callback = delay, callback
                return timer

            def fixture():
                invoked.append('body')
                timer.callback()  # A memory-only watchdog observation during this exact case.
                self.assertEqual(namespace['current']['id'], case.id())
                self.assertTrue(passing)

            case = unittest.FunctionTestCase(fixture)
            output, error = io.StringIO(), io.StringIO()
            with self.subTest(passing=passing), \
                    patch.object(os, 'environ', {'ZERUS_NATIVE_DIAGNOSTIC_DEADLINE': '1000'}), \
                    patch.object(threading, 'Timer', side_effect=make_timer), \
                    patch.object(time, 'monotonic', side_effect=[100, 101, 105, 106, 110]), \
                    patch.object(faulthandler, 'dump_traceback') as stack, \
                    patch.object(unittest.defaultTestLoader, 'loadTestsFromNames', return_value=unittest.TestSuite([case])), \
                    patch('sys.stdout', output), patch('sys.stderr', error), \
                    self.assertRaises(SystemExit) as exited:
                exec(compile(compat.NATIVE_RUNNER, '<native-runner-control>', 'exec'), namespace)
            self.assertEqual(exited.exception.code, not passing)
            self.assertEqual(invoked, ['body'])
            self.assertEqual(timer.delay, 899)
            self.assertTrue(timer.daemon)
            timer.start.assert_called_once_with()
            timer.cancel.assert_called_once_with()
            stack.assert_called_once_with(file=error, all_threads=True)
            rows = [json.loads(line.removeprefix('ZERUS_NATIVE_DIAGNOSTIC=')) for line in error.getvalue().splitlines()
                    if line.startswith('ZERUS_NATIVE_DIAGNOSTIC=')]
            self.assertEqual([row['event'] for row in rows], ['start', 'before-aggregate-timeout', 'end'])
            self.assertEqual(rows[0], dict(event='start', id=case.id(), started_monotonic=101))
            self.assertEqual(rows[1], dict(event='before-aggregate-timeout', monotonic=105,
                                          current=dict(id=case.id(), started_monotonic=101), case_elapsed=4))
            self.assertEqual(rows[2], dict(event='end', id=case.id(), ended_monotonic=106, elapsed=5))
            self.assertIsNone(namespace['current'])
            self.assertEqual(output.getvalue(), 'ZERUS_NATIVE_RESULT={"tests": 1, "skipped": 0}\n')

    def test_native_runner_rejects_an_empty_or_skipped_suite(self):
        # Execute the actual CI runner with an unavailable native executable:
        # a unittest skip must never turn this compatibility gate green.
        import os
        import subprocess
        env = dict(os.environ, PYTHONPATH=str(ROOT / 'tests'))
        for spec in compat.AGENTS.values():
            env.pop(spec['variable'], None)
            for key in spec.get('environment', {}):
                env.pop(key, None)
        # macOS runs the Linux suites; Windows its session host suites.
        matrices = {}
        for agent, spec in compat.AGENTS.items():
            for target in spec['members']:
                matrices.setdefault(tuple(compat.target_spec(agent, target)['suites']), agent + ' ' + target)
        matrices = [(name, list(suites)) for suites, name in matrices.items()]
        for name, suites in [('empty', [])] + matrices:
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
                'OPENCODE_CONFIG': '/fixture-live-opencode.json', 'OPENCODE_SERVER_PASSWORD': 'fixture-password',
                'OPENAI_API_KEY': 'fixture-key', 'ANTHROPIC_API_KEY': 'fixture-key',
                'HGS_TEST_CONPTY_DIR': '/fixture-conpty', 'SYSTEMROOT': 'C:\\Windows', 'PATHEXT': '.COM;.EXE',
                'TEMP': '/fixture-temp', 'USERPROFILE': '/fixture-live-profile'}):
            home = Path(temporary) / 'home'
            env = compat.fixture_environment(home)
            self.assertEqual(env['HOME'], str(home))
            self.assertTrue(Path(env['CODEX_HOME']).is_dir())
            self.assertEqual(env['PYTHONPATH'], str(ROOT / 'tests'))
            self.assertEqual(env['PYTHONUTF8'], '1')
            self.assertEqual(env['CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC'], '1')
            for key in ('HGS_SESSION', 'HGS_RUN_ID', 'HGS_ACCOUNT', 'OPENAI_API_KEY', 'ANTHROPIC_API_KEY',
                        'CLAUDE_CONFIG_DIR', 'CLAUDECODE', 'OPENCODE_CONFIG', 'OPENCODE_SERVER_PASSWORD'):
                self.assertNotIn(key, env)
            # The suites' own inputs and what Windows programs need pass through. So
            # does the real profile: the Windows suites place their projects by it.
            for key in ('HGS_TEST_CONPTY_DIR', 'SYSTEMROOT', 'PATHEXT', 'TEMP', 'USERPROFILE'):
                self.assertEqual(env[key], os.environ[key])
            # The runner's own probes on Windows find a private profile instead.
            probes = compat.private_profile(env, home)
            self.assertEqual(probes['USERPROFILE'], str(home))
            for key in ('APPDATA', 'LOCALAPPDATA'):
                self.assertTrue(Path(probes[key]).is_dir())
                self.assertEqual(Path(probes[key]).parents[1], home)
            self.assertEqual(env['USERPROFILE'], '/fixture-live-profile')

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
