"""Stable candidate policy and private/public boundaries, without SDKs or keys."""
import importlib.util
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, call, patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
SPEC = importlib.util.spec_from_file_location('android_stable_candidate', ROOT / 'scripts/android-stable-candidate.py')
C = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(C)


class StablePolicy(unittest.TestCase):
    def setUp(self):
        self.env = dict(GITHUB_REPOSITORY='ufna/zerus', GITHUB_REF='refs/heads/main',
                        GITHUB_EVENT_NAME='workflow_dispatch', GITHUB_SHA='a' * 40,
                        REQUESTED_SOURCE_COMMIT='a' * 40, REQUESTED_VERSION='0.3.0',
                        REQUESTED_VERSION_CODE='15000', REQUESTED_CI_RUN_ID='123',
                        REQUESTED_SIGNING_CERT_SHA256='b' * 64, GITHUB_RUN_ID='124', GITHUB_RUN_ATTEMPT='1')
        self.request = C.context(self.env)
        self.run = dict(id=123, path='.github/workflows/ci.yml', event='push', head_branch='main',
                        head_sha='a' * 40, head_repository=dict(full_name='ufna/zerus'),
                        repository=dict(full_name='ufna/zerus'), status='completed', conclusion='success',
                        workflow_id=17, run_attempt=1)
        self.workflow = dict(id=17, path='.github/workflows/ci.yml')
        self.jobs = json.loads((ROOT / 'tests/fixtures/android-stable-main-ci.json').read_text())['jobs']

    def test_real_shaped_gate_fixture_agrees_with_both_checked_in_workflows(self):
        ci = (ROOT / '.github/workflows/ci.yml').read_text()
        checks = (ROOT / '.github/workflows/checks.yml').read_text()
        def name(text, key):
            return re.search(r'(?m)^  ' + key + r':\n    name: (.+)$', text)[1]
        expected = {name(ci, 'gate'), name(ci, 'automatic') + ' / ' + name(checks, 'gate')}
        self.assertTrue(expected <= {job['name'] for job in self.jobs})
        self.assertEqual(len(self.jobs), 16)
        self.assertEqual(C.validate_ci(self.run, self.workflow, self.jobs, self.request)['run_id'], 123)
        wrong_nested = [{**job, 'name': name(ci, 'automatic') + ' / ' + name(ci, 'gate')}
                        if job['name'] != name(ci, 'gate') else job for job in self.jobs]
        with self.assertRaises(ValueError):
            C.validate_ci(self.run, self.workflow, wrong_nested, self.request)

    def test_exact_trusted_main_dispatch_and_stable_values(self):
        self.assertEqual(C.context(self.env)['ci_run_id'], 123)
        C.stable_identity('val stableVersionName = "0.3.0"\n', '15000\n', self.request)
        for key, value in [('GITHUB_REPOSITORY', 'example/zerus'), ('GITHUB_REF', 'refs/pull/1/head'),
                           ('GITHUB_EVENT_NAME', 'pull_request_target'), ('GITHUB_SHA', 'c' * 40),
                           ('REQUESTED_VERSION', '0.3.1-dev.12'), ('REQUESTED_VERSION_CODE', '0'),
                           ('REQUESTED_VERSION_CODE', '2100000001'), ('REQUESTED_CI_RUN_ID', '../123'),
                           ('GITHUB_RUN_ATTEMPT', '0'), ('REQUESTED_SIGNING_CERT_SHA256', ''),
                           ('REQUESTED_SIGNING_CERT_SHA256', 'B' * 64)]:
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                C.context({**self.env, key: value})

    def test_committed_identity_rejects_dev_wrong_code_and_duplicate_selectors(self):
        for text, code in [('val stableVersionName = "0.3.1"\n', '15000'),
                           ('val stableVersionName = "0.3.0"\n', '15001'),
                           ('val stableVersionName = "0.3.0-dev.1"\n', '15000'),
                           ('val stableVersionName = "0.3.0"\n' * 2, '15000')]:
            with self.subTest(text=text, code=code), self.assertRaises(ValueError):
                C.stable_identity(text, code, self.request)

    def test_successful_exact_full_push_ci_and_both_gates(self):
        self.assertEqual(C.validate_ci(self.run, self.workflow, self.jobs, self.request),
                         dict(run_id=123, run_attempt=1, workflow_id=17))
        changes = [('id', 125), ('path', '.github/workflows/checks.yml'), ('event', 'workflow_dispatch'),
                   ('event', 'pull_request'), ('head_branch', 'feature'), ('head_sha', 'c' * 40),
                   ('head_repository', dict(full_name='example/zerus')), ('repository', {}),
                   ('status', 'in_progress'), ('conclusion', 'failure'), ('conclusion', None),
                   ('workflow_id', 18), ('run_attempt', True)]
        for key, value in changes:
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                C.validate_ci({**self.run, key: value}, self.workflow, self.jobs, self.request)
        for jobs in [[], self.jobs[:1], self.jobs + self.jobs[:1],
                     [{**j, 'conclusion': 'skipped'} for j in self.jobs],
                     [{**j, 'run_attempt': 2} for j in self.jobs]]:
            with self.subTest(jobs=jobs), self.assertRaises(ValueError):
                C.validate_ci(self.run, self.workflow, jobs, self.request)

    def test_reduced_dev_cannot_qualify_stable_and_full_nightly_release_can(self):
        for missing in ('Windows client, desktop and portable zip', 'iOS core tests and simulator build'):
            changed = [{**j, 'conclusion': 'skipped'} if j['name'].endswith(missing) else j for j in self.jobs]
            with self.subTest(missing=missing), self.assertRaisesRegex(ValueError, 'Every exact full-matrix'):
                C.validate_ci(self.run, self.workflow, changed, self.request)
        for file, event in [('nightly.yml', 'schedule'), ('nightly.yml', 'workflow_dispatch'),
                            ('release.yml', 'workflow_dispatch')]:
            run = {**self.run, 'path': '.github/workflows/' + file, 'event': event}
            workflow = {**self.workflow, 'path': run['path']}
            jobs = [{**j, 'name': j['name'].replace('Trusted main checks / ', 'verify / ')}
                    for j in self.jobs if j['name'] != 'CI gate']
            self.assertEqual(C.validate_ci(run, workflow, jobs, self.request)['run_id'], 123)
            if file == 'nightly.yml' and event == 'workflow_dispatch':
                reviewed = [{**j, 'name': j['name'].replace('verify / ', 'review / ')} for j in jobs]
                self.assertEqual(C.validate_ci(run, workflow, reviewed, self.request)['run_id'], 123)
            with self.assertRaises(ValueError):
                C.validate_ci(run, workflow, jobs[:-1], self.request)

    def test_full_ci_refusal_precedes_any_generation_or_signing_allocation(self):
        with patch.object(C, 'source_check'), patch.object(C, 'main_ci', side_effect=ValueError('CI refused')), \
                patch.object(C, 'run_root') as root, patch.object(C, 'signing_inputs') as secrets:
            with self.assertRaisesRegex(ValueError, 'CI refused'):
                C.admit(self.env)
            root.assert_not_called()
            secrets.assert_not_called()

    def test_incomplete_pagination_never_admits_ci(self):
        responses = [self.run, self.workflow, {'total_count': 101, 'jobs': self.jobs}]
        with patch.object(C, 'json_api', side_effect=responses), self.assertRaisesRegex(ValueError, 'Incomplete'):
            C.main_ci(self.request, float('inf'))

    def test_sealed_apk_requires_same_package_stable_version_code_and_reviewed_signer(self):
        info = dict(package_name='app.zerus.mobile', version_name='0.3.0', version_code=15000,
                    signing_cert_sha256='b' * 64)
        C.validate_apk(info, self.request)
        for key, value in [('package_name', 'example.app'), ('version_name', '0.3.1-dev.1'),
                           ('version_code', 15001), ('version_code', True),
                           ('signing_cert_sha256', 'c' * 64), ('signing_cert_sha256', None)]:
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                C.validate_apk({**info, key: value}, self.request)

    def task_log(self):
        return ('> Task :app:injectCrashlyticsMappingFileIdRelease\n'
                '> Task :app:uploadCrashlyticsMappingFileRelease\n'
                '> Task :app:assembleRelease\nBUILD SUCCESSFUL in 3m 12s\n')

    def test_mapping_upload_must_execute_once_in_this_successful_build(self):
        self.assertEqual(C.mapping_upload(self.task_log())['upload'], 'executed')
        for suffix in (' SKIPPED', ' UP-TO-DATE', ' FROM-CACHE', ' NO-SOURCE', ' FAILED'):
            changed = self.task_log().replace('uploadCrashlyticsMappingFileRelease\n',
                                              'uploadCrashlyticsMappingFileRelease' + suffix + '\n')
            with self.subTest(suffix=suffix), self.assertRaises(ValueError):
                C.mapping_upload(changed)
        for changed in [self.task_log().replace('> Task :app:uploadCrashlyticsMappingFileRelease\n', ''),
                        self.task_log() + '> Task :app:uploadCrashlyticsMappingFileRelease\n',
                        self.task_log().replace('BUILD SUCCESSFUL', 'BUILD FAILED'),
                        self.task_log().replace('injectCrashlyticsMappingFileIdRelease', 'assembleRelease', 1),
                        '\x1b[0m' + self.task_log()]:
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                C.mapping_upload(changed)

    def test_public_family_excludes_mapping_keys_configuration_and_raw_logs(self):
        family = ['release/zerus-0.3.0.apk', 'release/zerus-0.3.0-source.tar.gz', 'release/release-info.json',
                  'release/SHA256SUMS', 'candidate-provenance.json']
        C.public_family(family, '0.3.0')
        for extra in ['mapping.txt', 'signer.jks', 'google-services.json', 'signing.properties', 'gradle.log']:
            with self.subTest(extra=extra), self.assertRaises(ValueError):
                C.public_family(family + [extra], '0.3.0')
        for changed in [family[:-1], family + family[:1], ['release/zerus-0.3.1-dev.1.apk'] + family[1:]]:
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                C.public_family(changed, '0.3.0')

    def test_exact_source_archive_rejects_links_traversal_duplicate_and_overbound(self):
        root = tarfile.TarInfo('zerus-0.3.0');root.type = tarfile.DIRTYPE
        leaf = tarfile.TarInfo('zerus-0.3.0/mobile/android/VERSION_CODE');leaf.size = 6
        C.source_members([root, leaf], 'zerus-0.3.0')
        for name, kind, size in [('../outside', tarfile.REGTYPE, 1), ('/outside', tarfile.REGTYPE, 1),
                                 ('zerus-0.3.0/link', tarfile.SYMTYPE, 0),
                                 ('zerus-0.3.0/link', tarfile.LNKTYPE, 0),
                                 ('wrong/file', tarfile.REGTYPE, 1),
                                 ('zerus-0.3.0/huge', tarfile.REGTYPE, 512 * 1024**2 + 1)]:
            item = tarfile.TarInfo(name);item.type = kind;item.size = size
            with self.subTest(name=name, kind=kind), self.assertRaises(ValueError):
                C.source_members([root, item], 'zerus-0.3.0')
        with self.assertRaises(ValueError):
            C.source_members([root, leaf, leaf], 'zerus-0.3.0')

    def test_one_original_end_has_no_late_success(self):
        C.before(100, clock=lambda: 99)
        for now in (100, 101):
            with self.subTest(now=now), self.assertRaises(ValueError):
                C.before(100, clock=lambda: now)


@unittest.skipUnless(sys.platform == 'linux', 'Ubuntu hosted candidate filesystem/process primitives')
class PrivateCleanup(unittest.TestCase):
    def test_owned_cleanup_unlinks_links_without_following_or_touching_public_candidate(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'generation';root.mkdir(mode=0o700)
            info = root.stat();data = dict(root_dev=info.st_dev, root_ino=info.st_ino, uid=info.st_uid, end=float('inf'), admitted=1.0)
            private = root / 'private';private.mkdir(mode=0o700)
            outside = Path(tmp) / 'outside';outside.write_bytes(b'keep')
            (private / 'link').symlink_to(outside)
            candidate = root / 'staged-candidate';candidate.mkdir();(candidate / 'apk').write_bytes(b'public')
            with patch.object(C, 'generation_survivors', return_value=[]):
                C.cleanup_private(root, data)
            self.assertFalse(private.exists());self.assertEqual(outside.read_bytes(), b'keep')
            self.assertEqual((candidate / 'apk').read_bytes(), b'public')

    def test_changed_identity_late_or_cleanup_error_refuses_and_preserves_unknown_state(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'generation';root.mkdir(mode=0o700)
            info = root.stat();data = dict(root_dev=info.st_dev, root_ino=info.st_ino, uid=info.st_uid, end=float('inf'), admitted=1.0)
            private = root / 'private';private.mkdir(mode=0o700);(private / 'key').write_bytes(b'synthetic')
            for changed in [{**data, 'root_ino': info.st_ino + 1}, {**data, 'end': 0}]:
                with self.subTest(changed=changed), self.assertRaises(ValueError):
                    C.cleanup_private(root, changed)
                self.assertTrue((private / 'key').exists())
            with patch.object(C, 'generation_survivors', return_value=[]), \
                    patch.object(C.shutil, 'rmtree', side_effect=OSError('synthetic cleanup failure')), self.assertRaises(OSError):
                C.cleanup_private(root, data)
            self.assertTrue((private / 'key').exists())


    def test_generation_scan_detects_escaped_private_process_without_signalling(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp);proc = root / 'proc';proc.mkdir()
            first = C.os.getpid() + 1000000
            for pid, command, environment, cwd in [(str(first), b'java\0', b'HOME=/owned/private/home\0', '/elsewhere'),
                                                    (str(first + 1), b'other\0', b'HOME=/other\0', '/elsewhere')]:
                directory = proc / pid;directory.mkdir()
                (directory / 'cmdline').write_bytes(command);(directory / 'environ').write_bytes(environment)
                (directory / 'cwd').symlink_to(cwd)
            with patch.object(C.os, 'killpg') as kill:
                self.assertEqual(C.generation_survivors(Path('/owned/private'), float('inf'), proc), [first])
                kill.assert_not_called()

    def test_uninspectable_process_is_admitted_only_when_it_predates_the_generation(self):
        tick = C.os.sysconf('SC_CLK_TCK')
        original = Path.open
        with tempfile.TemporaryDirectory() as tmp:
            proc = Path(tmp) / 'proc';proc.mkdir()
            pid = str(C.os.getpid() + 1000000)
            directory = proc / pid;directory.mkdir()
            (directory / 'cmdline').write_bytes(b'manager\0');(directory / 'environ').write_bytes(b'HOME=/elsewhere\0')
            (directory / 'cwd').symlink_to('/')
            def denied(path, *args, **kwargs):
                if path == directory / 'environ':
                    raise PermissionError('capability-holding same-user process')
                return original(path, *args, **kwargs)
            def started(seconds):
                fields = ['S'] + ['0'] * 18 + [str(int(seconds * tick))] + ['0'] * 10
                (directory / 'stat').write_bytes(f'{pid} (user manager) '.encode() + ' '.join(fields).encode())
            with patch.object(Path, 'open', denied), patch.object(C.os, 'killpg') as kill:
                started(10)
                self.assertEqual(C.generation_survivors(Path('/owned/private'), float('inf'), proc, admitted=50.0), [])
                for admitted in (5.0, None):
                    with self.subTest(admitted=admitted), self.assertRaisesRegex(ValueError, 'Uninspectable'):
                        C.generation_survivors(Path('/owned/private'), float('inf'), proc, admitted=admitted)
                kill.assert_not_called()

    def test_cleanup_scans_with_the_recorded_admission_time(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'generation';root.mkdir(mode=0o700)
            info = root.stat();data = dict(root_dev=info.st_dev, root_ino=info.st_ino, uid=info.st_uid, end=float('inf'), admitted=42.5)
            (root / 'private').mkdir(mode=0o700)
            with patch.object(C, 'generation_survivors', return_value=[]) as scan:
                C.cleanup_private(root, data)
            self.assertEqual(scan.call_args.kwargs, dict(admitted=42.5))
            self.assertFalse((root / 'private').exists())

    def test_surviving_or_unknown_process_prevents_private_removal(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'generation';root.mkdir(mode=0o700)
            info = root.stat();data = dict(root_dev=info.st_dev, root_ino=info.st_ino, uid=info.st_uid, end=float('inf'), admitted=1.0)
            private = root / 'private';private.mkdir(mode=0o700);(private / 'key').write_bytes(b'synthetic')
            with patch.object(C, 'generation_survivors', return_value=[888881]), self.assertRaisesRegex(ValueError, 'survives'):
                C.cleanup_private(root, data)
            self.assertTrue((private / 'key').exists())
            with patch.object(C, 'generation_survivors', side_effect=PermissionError('unknown')), self.assertRaises(PermissionError):
                C.cleanup_private(root, data)
            self.assertTrue((private / 'key').exists())
            (private / 'PROCESS_CUSTODY_UNKNOWN').write_bytes(b'synthetic uncertainty')
            with self.assertRaisesRegex(ValueError, 'Unknown command cleanup'):
                C.cleanup_private(root, data)
            self.assertTrue((private / 'key').exists())


@unittest.skipUnless(sys.platform == 'linux', 'Ubuntu hosted candidate filesystem/process primitives')
class FrozenMapping(unittest.TestCase):
    def sealer(self):
        spec = importlib.util.spec_from_file_location('stable_mapping_sealer', ROOT / 'scripts/prepare-mobile-release.py')
        module = importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        return module

    def files(self, root):
        mapping = root / 'raw-mapping.txt';mapping.write_text('# pg_map_id: abcdef0\nFixture -> a:\n')
        sealed = root / 'sealed.apk'
        with zipfile.ZipFile(sealed, 'w') as archive:
            archive.writestr('classes.dex', b'fixture~~R8{"pg-map-id":"abcdef0"}\0')
        return mapping, sealed, root / 'frozen-mapping.txt'

    def test_snapshot_hash_and_identity_use_sealed_apk_not_later_mutable_build_mapping(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp);mapping, sealed, snapshot = self.files(root)
            original = mapping.read_bytes();sealer = self.sealer();observe = sealer.mapping_identity
            def observed(apk, frozen):
                self.assertEqual(apk, sealed);self.assertEqual(frozen, snapshot)
                mapping.write_text('# pg_map_id: 1234567\nChanged -> b:\n')
                return observe(apk, frozen)
            with patch.object(sealer, 'mapping_identity', side_effect=observed):
                result = C.frozen_mapping(sealer, sealed, mapping, snapshot, float('inf'))
            self.assertEqual(result, dict(mapping_sha256=hashlib.sha256(original).hexdigest(), r8_map_id='abcdef0'))
            self.assertEqual(snapshot.read_bytes(), original)
            self.assertEqual(snapshot.stat().st_mode & 0o777, 0o400)

    def test_mutation_after_identity_before_hash_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp);mapping, sealed, snapshot = self.files(root)
            sealer = self.sealer();observe = sealer.mapping_identity
            def changed(apk, frozen):
                identity = observe(apk, frozen)
                frozen.chmod(0o600);frozen.write_text('# pg_map_id: abcdef0\nDifferent body -> b:\n');frozen.chmod(0o400)
                return identity
            with patch.object(sealer, 'mapping_identity', side_effect=changed), \
                    self.assertRaisesRegex(ValueError, 'changed during APK identity observation'):
                C.frozen_mapping(sealer, sealed, mapping, snapshot, float('inf'))

    def test_replacement_during_identity_and_wrong_sealed_apk_are_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp);mapping, sealed, snapshot = self.files(root)
            sealer = self.sealer();observe = sealer.mapping_identity
            def replaced(apk, frozen):
                identity = observe(apk, frozen)
                frozen.rename(root / 'retained-original');frozen.write_bytes(mapping.read_bytes());frozen.chmod(0o400)
                return identity
            with patch.object(sealer, 'mapping_identity', side_effect=replaced), self.assertRaisesRegex(ValueError, 'changed during'):
                C.frozen_mapping(sealer, sealed, mapping, snapshot, float('inf'))
            snapshot.unlink()
            with zipfile.ZipFile(sealed, 'w') as archive:
                archive.writestr('classes.dex', b'fixture~~R8{"pg-map-id":"1234567"}\0')
            with self.assertRaisesRegex(ValueError, 'does not match APK'):
                C.frozen_mapping(sealer, sealed, mapping, snapshot, float('inf'))


@unittest.skipUnless(sys.platform == 'linux', 'Ubuntu hosted candidate filesystem/process primitives')
class CommandCustody(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'generation';self.root.mkdir(mode=0o700)
        info = self.root.stat()
        self.data = dict(root_dev=info.st_dev, root_ino=info.st_ino, uid=info.st_uid, end=float('inf'), admitted=1.0)
        self.private = self.root / 'private';self.private.mkdir(mode=0o700)
        self.log = self.private / 'fake.log'
        self.child = Mock(pid=987654, returncode=0)
        self.popen = self.enterContext(patch.object(C.subprocess, 'Popen', return_value=self.child))
        self.waitid = self.enterContext(patch.object(C.os, 'waitid', return_value=object()))
        self.signal = self.enterContext(patch.object(C.os, 'killpg'))
        self.sleep = self.enterContext(patch.object(C.time, 'sleep'))

    def run_command(self):
        C.command(['synthetic-only'], self.private, {}, self.log, float('inf'))

    def assert_all_owned_cleanup(self):
        self.assertEqual(self.signal.call_args_list, [call(self.child.pid, C.signal.SIGTERM),
                                                      call(self.child.pid, C.signal.SIGKILL)])
        self.child.wait.assert_called_once_with(timeout=10)
        self.popen.assert_called_once()
        self.assertFalse((self.root / 'candidate').exists())

    def assert_unknown_retained(self):
        self.assertTrue((self.private / 'command-fake.log.pending.json').is_file())
        self.assertFalse((self.private / 'command-fake.log.complete.json').exists())
        with self.assertRaisesRegex(ValueError, 'Unknown command cleanup|Unfinished command custody'):
            C.cleanup_private(self.root, self.data)
        self.assertTrue(self.private.exists())

    def test_success_keeps_unreaped_owner_until_both_actions_then_reaps_and_commits(self):
        events, state = [], {'reaped': False}
        real_json_new = C.json_new
        def durable(path, value):
            if path.name.endswith('.complete.json'):
                self.assertTrue(state['reaped'], 'complete record must follow owned reap')
            real_json_new(path, value)
            if path.name.endswith('.pending.json'):
                events.append('durable-pending')
            elif path.name.endswith('.complete.json'):
                events.append('durable-complete')
        def spawned(*args, **kwargs):
            self.assertEqual(events, ['durable-pending'])
            self.assertFalse(state['reaped'])
            events.append('Popen')
            return self.child
        def observed(kind, pid, flags):
            self.assertEqual((kind, pid, flags), (C.os.P_PID, self.child.pid,
                                                C.os.WEXITED | C.os.WNOHANG | C.os.WNOWAIT))
            self.assertFalse(state['reaped'])
            events.append('waitid-WNOWAIT')
            return object()
        def signalled(pid, sig):
            self.assertEqual(pid, self.child.pid)
            self.assertFalse(state['reaped'], 'a reaped PID cannot authorize a group signal')
            events.append('TERM' if sig == C.signal.SIGTERM else 'KILL')
        def waited(*, timeout):
            self.assertEqual(timeout, 10)
            self.assertFalse(state['reaped'])
            events.append('wait-reap')
            state['reaped'] = True
            return 0
        self.popen.side_effect = spawned
        self.waitid.side_effect = observed
        self.signal.side_effect = signalled
        self.child.wait.side_effect = waited
        with patch.object(C, 'json_new', side_effect=durable):
            self.run_command()
        self.assert_all_owned_cleanup()
        self.assertEqual(events, ['durable-pending', 'Popen', 'waitid-WNOWAIT',
                                  'TERM', 'KILL', 'wait-reap', 'durable-complete'])
        self.assertTrue(C.completed_commands(self.private))
        self.waitid.assert_called_once_with(C.os.P_PID, self.child.pid,
                                            C.os.WEXITED | C.os.WNOHANG | C.os.WNOWAIT)

    def test_timeout_still_attempts_both_owned_signals_and_wait_without_follow_on(self):
        self.waitid.return_value = None
        with patch.object(C, 'before', side_effect=[None, ValueError('original timeout')]), \
                self.assertRaisesRegex(ValueError, 'original timeout'):
            self.run_command()
        self.assert_all_owned_cleanup();self.assertTrue(C.completed_commands(self.private))

    def test_log_cap_stops_before_any_other_command_and_cleans_owned_child(self):
        self.waitid.return_value = None
        def started(*args, **kwargs):
            kwargs['stdout'].write(b'x');kwargs['stdout'].flush();return self.child
        self.popen.side_effect = started
        with patch.object(C, 'MAX_LOG', 0), self.assertRaisesRegex(ValueError, 'log exceeded'):
            self.run_command()
        self.assert_all_owned_cleanup()

    def test_waitid_error_still_attempts_owned_cleanup_and_keeps_primary_failure(self):
        self.waitid.side_effect = OSError('synthetic waitid failure')
        with self.assertRaisesRegex(OSError, 'synthetic waitid failure'):
            self.run_command()
        self.assert_all_owned_cleanup()

    def test_lost_unreaped_owner_never_signals_reused_numeric_pid_and_retains_private(self):
        self.waitid.side_effect = ChildProcessError('synthetic ECHILD')
        with self.assertRaisesRegex(ValueError, 'cleanup unknown'):
            self.run_command()
        self.signal.assert_not_called();self.child.wait.assert_called_once_with(timeout=10)
        self.assert_unknown_retained()

    def test_terminate_failure_does_not_skip_kill_or_wait_and_is_durable_unknown(self):
        self.signal.side_effect = [PermissionError('synthetic term refusal'), None]
        with self.assertRaisesRegex(ValueError, 'cleanup unknown'):
            self.run_command()
        self.assert_all_owned_cleanup();self.assert_unknown_retained()

    def test_kill_failure_still_waits_and_retains_private(self):
        self.signal.side_effect = [None, PermissionError('synthetic kill refusal')]
        with self.assertRaisesRegex(ValueError, 'cleanup unknown'):
            self.run_command()
        self.assert_all_owned_cleanup();self.assert_unknown_retained()

    def test_wait_failure_after_both_signals_retains_private_and_denies_artifact(self):
        self.child.wait.side_effect = subprocess.TimeoutExpired('synthetic-only', 10)
        with self.assertRaisesRegex(ValueError, 'cleanup unknown'):
            self.run_command()
        self.assert_all_owned_cleanup();self.assert_unknown_retained()

    def test_unknown_record_failure_still_leaves_durable_pending_private_retained(self):
        self.signal.side_effect = [PermissionError('synthetic term refusal'), None]
        real = C.json_new
        def fail_unknown(path, value):
            if path.name == 'PROCESS_CUSTODY_UNKNOWN':
                raise OSError('synthetic UNKNOWN fsync failure')
            return real(path, value)
        with patch.object(C, 'json_new', side_effect=fail_unknown), self.assertRaisesRegex(OSError, 'UNKNOWN fsync'):
            self.run_command()
        self.assert_all_owned_cleanup();self.assert_unknown_retained()

    def test_spawn_failure_retains_pre_spawn_intent_and_never_guesses_child(self):
        self.popen.side_effect = OSError('synthetic spawn failure')
        with self.assertRaisesRegex(OSError, 'spawn failure'):
            self.run_command()
        self.signal.assert_not_called();self.child.wait.assert_not_called();self.assert_unknown_retained()

    def test_first_command_refusal_prevents_signing_sealing_or_follow_on_command(self):
        request = dict(source_commit='a' * 40, version='0.3.0', version_code=15000,
                       signing_cert_sha256='b' * 64, run_id=124, run_attempt=1, ci_run_id=123)
        # build() must allocate its own fresh private root, just as production does.
        self.private.rmdir()
        data = {**self.data, 'request': request, 'ci': {}}
        env = dict(PATH='/synthetic/bin', JAVA_HOME_17_X64='/synthetic/jdk', ANDROID_HOME='/synthetic/sdk')
        with patch.object(C, 'admission', return_value=(self.root, data)), patch.object(C, 'source_check'), \
                patch.object(C, 'command', side_effect=ValueError('first refused')) as command, \
                patch.object(C, 'signing_inputs') as signing, patch.object(C, 'generation_survivors', return_value=[]), \
                patch.object(C, 'frozen_mapping') as mapping, self.assertRaisesRegex(ValueError, 'first refused'):
            C.build(env)
        command.assert_called_once();signing.assert_not_called();mapping.assert_not_called()
        self.assertFalse((self.root / 'candidate').exists())

class WorkflowBoundary(unittest.TestCase):
    def test_signer_exists_only_after_admission_in_protected_read_only_no_cache_job(self):
        text = (ROOT / '.github/workflows/android-release.yml').read_text()
        self.assertIn('  workflow_dispatch:\n', text)
        self.assertNotRegex(text, r'(?m)^  (push|pull_request|workflow_run|workflow_call):')
        for condition in ["github.repository == 'ufna/zerus'", "github.ref == 'refs/heads/main'",
                          "github.event_name == 'workflow_dispatch'"]:
            self.assertIn(condition, text)
        self.assertIn('environment: release', text)
        self.assertIn('cache-mode: none', text)
        self.assertNotRegex(text, r'actions/cache|contents: write|actions: write|secrets: inherit|self-hosted')
        self.assertIn('runs-on: ubuntu-24.04', text)
        self.assertIn('ref: ${{ github.sha }}', text)
        self.assertIn('persist-credentials: false', text)
        self.assertLess(text.index('android-stable-candidate.py admit'), text.index('secrets.ANDROID_'))
        self.assertEqual(set(re.findall(r'secrets\.(\w+)', text)), set(C.SECRETS))
        self.assertIn("if: always() && steps.admission.outcome == 'success'\n        run: python3 -B scripts/android-stable-candidate.py cleanup", text)
        self.assertLess(text.index('android-stable-candidate.py cleanup'), text.index('actions/upload-artifact@'))
        self.assertNotIn('if: always()\n        uses: actions/upload-artifact', text)
        self.assertIn('/candidate\n', text)
        self.assertNotIn('prepare-dev-update.py', text)

    def test_build_has_fixed_tools_tasks_no_dev_override_and_cleanup_before_public_admission(self):
        source = (ROOT / 'scripts/android-stable-candidate.py').read_text()
        for item in ['--max-workers=2', '--no-parallel', '--no-build-cache', 'assembleRelease',
                     'lintRelease', '-PzerusFirebase=true', "'platforms;android-36'", "'build-tools;36.0.0'"]:
            self.assertIn(item, source)
        for forbidden in ['-PzerusDevBuildNumber', '-PzerusVersionCode', '-PzerusVersionName',
                          'gh release', 'make_latest', 'ANDROID_STORE_PASSWORD=']:
            self.assertNotIn(forbidden, source)
        build = source.split('\ndef build(env):\n', 1)[1].split('\ndef cleanup(env):\n', 1)[0]
        self.assertLess(build.index('mapping_upload('), build.index('sealer.seal('))
        self.assertLess(build.index('validate_apk(info, request)'), build.index('frozen_mapping(sealer, candidate / apk.name'))
        self.assertLess(build.index('frozen_mapping(sealer, candidate / apk.name'), build.index('cleanup_private('))
        self.assertLess(build.index("json_new(root / 'mapping-receipt.json', provenance)"), build.index('cleanup_private('))
        self.assertIn('finally:\n        cleanup_private(root, data)', build)
        self.assertLess(build.index("json_new(root / 'build-started.json'"), build.index('private.mkdir('))
        self.assertLess(build.index('cleanup_private('), build.index("provenance['private_cleanup'] = 'complete'"))
        self.assertLess(build.index('public_family('), build.index("candidate.rename(public / 'release')"))
        self.assertNotIn("(candidate / 'SHA256SUMS').unlink()", build)


if __name__ == '__main__':
    unittest.main()
