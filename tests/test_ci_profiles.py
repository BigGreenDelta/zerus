"""Behavioral coverage, delivery ordering and read-only iOS preflight controls."""
import base64
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import test_ci_security as security

field, inline_python, job, steps = security.field, security.inline_python, security.job, security.steps

ROOT = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), ROOT / 'scripts/ci' / (name + '.py'))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


coverage = load('select-coverage')
numbers = load('ios-ci-number')
asc = load('ios-testflight-preflight')


class Coverage(unittest.TestCase):
    def test_paths_preserve_platform_specific_and_shared_coverage(self):
        for paths, expected in [
            (['docs/reference.md'], (False, False)), (['mobile/android/app/a.kt'], (False, False)),
            (['tray/src/NewSessionDialog.cpp'], (True, False)),
            (['mobile/ios/ZerusKit/a.swift'], (False, True)),
            (['tray/src/A.cpp', 'mobile/ios/A.swift'], (True, True)),
            (['src/state/input.rs'], (True, True)), (['services/mobile/relay/src/api.rs'], (True, True)),
            (['tests/fixtures/mobile-v1.json'], (True, True)), (['Cargo.lock'], (True, True)),
            (['.github/workflows/ci.yml'], (True, True)), (['new-build-system/config'], (True, True)),
            ([], (True, True)), (['../unknown'], (True, True)),
        ]:
            with self.subTest(paths=paths):
                self.assertEqual(coverage.platforms(paths), expected)

    def test_pr_diff_uses_ancestry_and_both_sides_of_renames(self):
        with tempfile.TemporaryDirectory() as root:
            def git(*argv):
                return subprocess.check_output(['git', '-C', root, *argv], stderr=subprocess.DEVNULL).decode().strip()
            git('init', '-q')
            git('config', 'user.name', 'fixture')
            git('config', 'user.email', 'fixture@example.invalid')
            (Path(root) / 'docs').mkdir()
            (Path(root) / 'docs/a.md').write_text('fixture\n')
            git('add', '.'); git('commit', '-qm', 'base')
            base = git('rev-parse', 'HEAD')
            (Path(root) / 'src').mkdir()
            git('mv', 'docs/a.md', 'src/new.rs'); git('commit', '-qm', 'rename')
            head = git('rev-parse', 'HEAD')
            old = os.getcwd()
            try:
                os.chdir(root)
                paths = coverage.changed_paths(base, head)
            finally:
                os.chdir(old)
            self.assertEqual(set(paths), {'docs/a.md', 'src/new.rs'})
            self.assertEqual(coverage.platforms(paths), (True, True))

    def test_unknown_pr_inventory_retains_extra_platforms_and_explicit_requests_win(self):
        env = dict(RUN_LINUX='true', RUN_ARCH='true', RUN_MACOS='true', RUN_WINDOWS='false', RUN_IOS='false',
                   FILTER_PR_PLATFORMS='true', EVENT_NAME='pull_request')
        with patch.object(coverage, 'changed_paths', return_value=['docs/a.md']):
            self.assertFalse(coverage.select(env)['run_windows'])
            self.assertFalse(coverage.select(env)['run_ios'])
            self.assertTrue(coverage.select({**env, 'RUN_WINDOWS': 'true'})['run_windows'])
        for error in (ValueError('missing revisions'), OSError('unavailable'), subprocess.TimeoutExpired('git', 30)):
            with self.subTest(error=error), patch.object(coverage, 'changed_paths', side_effect=error):
                chosen = coverage.select(env)
                self.assertTrue(chosen['run_windows'] and chosen['run_ios'])
        with self.assertRaises(ValueError):
            coverage.select({**env, 'RUN_IOS': ''})
        with self.assertRaises(ValueError):
            coverage.select({**env, 'EVENT_NAME': 'push'})

    def test_dev_full_and_manual_coverage_do_not_override_each_other(self):
        common = dict(RUN_LINUX='true', RUN_ARCH='true', RUN_MACOS='true', FILTER_PR_PLATFORMS='false')
        dev = coverage.select({**common, 'RUN_WINDOWS': 'false', 'RUN_IOS': 'false'})
        self.assertEqual(dev, dict(run_linux=True, run_arch=True, run_macos=True, run_windows=False, run_ios=False))
        full = coverage.select({**common, 'RUN_WINDOWS': 'true', 'RUN_IOS': 'true'})
        self.assertTrue(all(full.values()))
        manual = coverage.select({**common, 'RUN_MACOS': 'false', 'RUN_WINDOWS': 'false', 'RUN_IOS': 'true'})
        self.assertFalse(manual['run_macos'])
        self.assertTrue(manual['run_ios'])


class IOSSequence(unittest.TestCase):
    def setUp(self):
        self.run = dict(path='.github/workflows/ci.yml', event='push', head_branch='main', head_sha='a' * 40,
                        head_repository=dict(full_name='ufna/zerus'), repository=dict(full_name='ufna/zerus'),
                        status='completed', conclusion='success', run_number=1215)

    def test_nightly_sequence_never_replaces_main_ci_sequence(self):
        nightly = {**self.run, 'path': '.github/workflows/nightly.yml', 'event': 'schedule', 'run_number': 25}
        self.assertEqual(numbers.number([nightly, self.run], 'a' * 40), 1215)
        self.assertEqual(numbers.number([self.run, {**self.run, 'run_number': 1216}], 'a' * 40), 1216)

    def test_missing_foreign_failed_partial_and_wrong_source_are_not_build_numbers(self):
        for change in [dict(event='pull_request'), dict(head_sha='b' * 40), dict(conclusion='failure'),
                       dict(status='in_progress'), dict(head_branch='feature'), dict(run_number=True),
                       dict(repository=dict(full_name='fork/zerus')), dict(head_repository={})]:
            with self.subTest(change=change), self.assertRaises(ValueError):
                numbers.number([{**self.run, **change}], 'a' * 40)
        with self.assertRaises(ValueError):
            numbers.number([], 'a' * 40)


class IOSDuplicate(unittest.TestCase):
    def setUp(self):
        self.apps = [dict(type='apps', id='123', attributes=dict(bundleId='app.zerus.mobile'))]
        self.build = dict(type='builds', attributes=dict(version='1215', expired=False, processingState='VALID'))

    def test_unused_build_uploads_once_existing_valid_or_processing_does_not(self):
        self.assertTrue(asc.upload_needed(self.apps, [], '1215'))
        self.assertFalse(asc.upload_needed(self.apps, [self.build], '1215'))
        processing = {**self.build, 'attributes': {**self.build['attributes'], 'processingState': 'PROCESSING'}}
        self.assertFalse(asc.upload_needed(self.apps, [processing], '1215'))

    def test_ambiguous_failed_expired_wrong_number_or_unknown_never_reupload(self):
        for builds in [[self.build, self.build], [dict(type='builds', attributes={})],
                       [{**self.build, 'attributes': {**self.build['attributes'], 'expired': True}}],
                       [{**self.build, 'attributes': {**self.build['attributes'], 'version': '1214'}}],
                       [{**self.build, 'attributes': {**self.build['attributes'], 'processingState': 'FAILED'}}],
                       [{**self.build, 'attributes': {**self.build['attributes'], 'processingState': 'INVALID'}}]]:
            with self.subTest(builds=builds), self.assertRaises(ValueError):
                asc.upload_needed(self.apps, builds, '1215')
        for apps in [[], self.apps * 2, [dict(type='apps', attributes=dict(bundleId='example.invalid'))]]:
            with self.subTest(apps=apps), self.assertRaises(ValueError):
                asc.upload_needed(apps, [], '1215')

    def test_preflight_only_reads_exact_app_version_and_number_and_never_logs_key(self):
        with tempfile.TemporaryDirectory() as root:
            output = Path(root) / 'output'
            env = dict(CHANNEL='dev', BUILD_NUMBER='1215', ASC_KEY_ID='ABCDEFGH12',
                       ASC_ISSUER_ID='11111111-1111-4111-8111-111111111111',
                       ASC_KEY='synthetic-private-key', GITHUB_OUTPUT=str(output))
            for builds, expected in [([], 'true'), ([self.build], 'false')]:
                with patch.dict(os.environ, env), patch.object(asc, 'token', return_value='private-bearer'), \
                        patch.object(asc, 'get', side_effect=[self.apps, builds]) as reads, \
                        contextlib.redirect_stdout(io.StringIO()) as log:
                    asc.main()
                    self.assertEqual(reads.call_count, 2)
                    query = reads.call_args_list[1].args[1]
                    self.assertEqual(query['filter[app]'], '123')
                    self.assertEqual(query['filter[version]'], '1215')
                    self.assertEqual(query['filter[preReleaseVersion.platform]'], 'IOS')
                    self.assertIn('filter[preReleaseVersion.version]', query)
                    self.assertNotIn('private', log.getvalue())
                self.assertTrue(output.read_text().endswith('upload=' + expected + '\n'))

    def test_api_uncertainty_does_not_emit_upload_admission(self):
        with tempfile.TemporaryDirectory() as root:
            output = Path(root) / 'output'
            env = dict(CHANNEL='dev', BUILD_NUMBER='1215', ASC_KEY_ID='ABCDEFGH12',
                       ASC_ISSUER_ID='11111111-1111-4111-8111-111111111111',
                       ASC_KEY='synthetic-key', GITHUB_OUTPUT=str(output))
            with patch.dict(os.environ, env), patch.object(asc, 'token', return_value='bearer'), \
                    patch.object(asc, 'get', side_effect=ValueError('read unavailable')), self.assertRaises(ValueError):
                asc.main()
            self.assertFalse(output.exists())

    def test_get_only_token_has_real_valid_signature_and_no_write_scope(self):
        with tempfile.TemporaryDirectory() as root:
            key, public = Path(root) / 'key.pem', Path(root) / 'pub.pem'
            subprocess.run(['openssl', 'ecparam', '-name', 'prime256v1', '-genkey', '-noout', '-out', str(key)],
                           check=True, capture_output=True)
            subprocess.run(['openssl', 'pkey', '-in', str(key), '-pubout', '-out', str(public)],
                           check=True, capture_output=True)
            jwt = asc.token(key, 'ABCDEFGH12', 'issuer-fixture', 1000)
            header, payload, signature = jwt.split('.')
            decode = lambda value: base64.urlsafe_b64decode(value + '=' * (-len(value) % 4))
            claims = json.loads(decode(payload))
            self.assertEqual(claims['scope'], ['GET /v1/apps', 'GET /v1/builds'])
            self.assertEqual(claims['exp'] - claims['iat'], 120)
            raw = decode(signature)
            self.assertEqual(len(raw), 64)
            def integer(value):
                value = value.lstrip(b'\0') or b'\0'
                if value[0] & 0x80:
                    value = b'\0' + value
                return b'\x02' + bytes([len(value)]) + value
            content = integer(raw[:32]) + integer(raw[32:])
            der = b'\x30' + bytes([len(content)]) + content
            sig = Path(root) / 'sig.der'; sig.write_bytes(der)
            result = subprocess.run(['openssl', 'dgst', '-sha256', '-verify', str(public), '-signature', str(sig)],
                                    input=(header + '.' + payload).encode(), capture_output=True)
            self.assertEqual(result.returncode, 0)
            self.assertEqual(asc.raw_signature(der), raw)
            for bad in [der + b'\0', b'\x30\x00', b'', b'\x30\x06\x02\x01\x80\x02\x01\x01']:
                with self.subTest(bad=bad), self.assertRaises(ValueError):
                    asc.raw_signature(bad)


class DeliveryGraph(unittest.TestCase):
    def test_only_scheduled_upstream_nightly_refreshes_caches_and_one_matrix_is_required(self):
        nightly = (ROOT / '.github/workflows/nightly.yml').read_text()
        automatic, manual = job(nightly, 'verify'), job(nightly, 'review')
        self.assertEqual(field(automatic, 'cache-mode'), 'write')
        self.assertEqual(field(manual, 'cache-mode'), 'read')
        self.assertIn('save_caches: true', automatic)
        self.assertNotIn('save_caches', manual)
        for event in ('schedule', 'workflow_dispatch', 'pull_request', 'push'):
            for repository in ('ufna/zerus', 'fork/zerus'):
                for ref in ('refs/heads/main', 'refs/heads/other'):
                    context = {'__builtins__': {}, 'github': SimpleNamespace(
                        event_name=event, repository=repository, ref=ref,
                        event=SimpleNamespace(repository=SimpleNamespace(private=False)))}
                    for text, expected_event in ((automatic, 'schedule'), (manual, 'workflow_dispatch')):
                        expression = field(text, 'if').replace('&&', ' and ').replace('false', 'False')
                        self.assertEqual(bool(eval(expression, context)),
                                         event == expected_event and repository == 'ufna/zerus' and ref == 'refs/heads/main')
        code = compile(inline_python(job(nightly, 'gate')), 'nightly-verification-gate', 'exec')
        for first, second, passed in [('success', 'skipped', True), ('skipped', 'success', True),
                                      ('failure', 'skipped', False), ('success', 'success', False),
                                      ('skipped', 'skipped', False), ('cancelled', 'success', False)]:
            env = {'RESULTS': json.dumps({'verify': {'result': first}, 'review': {'result': second}})}
            with self.subTest(first=first, second=second), patch.dict(os.environ, env):
                if passed:
                    exec(code, {})
                else:
                    with self.assertRaises(AssertionError):
                        exec(code, {})

    def test_unsigned_mac_can_overlap_checks_but_signing_requires_both_successes(self):
        ci = (ROOT / '.github/workflows/ci.yml').read_text()
        qualification = (ROOT / '.github/workflows/ci-dev-qualification.yml').read_text()
        helper = security.WorkflowSecurityTests()
        pending = {'gate': 'failure', 'qualification': 'failure'}
        self.assertTrue(helper.dev_selected(qualification, 'bundle', {}))
        self.assertFalse(helper.dev_selected(ci, 'dev-tail', pending))
        passed = dict.fromkeys(pending, 'success')
        self.assertTrue(helper.dev_selected(ci, 'dev-tail', passed))
        for name in passed:
            for result in ('failure', 'cancelled', 'skipped'):
                with self.subTest(name=name, result=result):
                    self.assertFalse(helper.dev_selected(ci, 'dev-tail', {**passed, name: result}))
        unsigned = job(qualification, 'bundle')
        self.assertNotIn('secrets', unsigned)
        self.assertNotIn('needs:', unsigned)
        bundle = (ROOT / '.github/workflows/macos-bundle.yml').read_text()
        self.assertNotIn('secrets', bundle.replace('Uses no secrets', '').replace('no secrets', ''))
        self.assertNotIn('environment:', bundle)

    def test_reused_bundle_does_not_depend_on_success_of_an_intentionally_skipped_build(self):
        cask = (ROOT / '.github/workflows/macos-cask.yml').read_text()
        # Explicit status functions also keep the install check alive after the
        # intentionally skipped bundle ancestor in Dev reuse mode.
        install = field(job(cask, 'verify'), 'if')
        self.assertIn('!cancelled()', install)
        self.assertIn("needs.sign.result == 'success'", install)
        nightly = (ROOT / '.github/workflows/nightly.yml').read_text()
        macos = field(job(nightly, 'macos'), 'if')
        self.assertIn('!cancelled()', macos)
        self.assertIn("needs.gate.result == 'success'", macos)
        expression = field(job(cask, 'sign'), 'if')
        expression = expression.replace('&&', ' and ').replace('||', ' or ').replace('!inputs.', 'not inputs.')
        expression = expression.replace('!cancelled()', 'not cancelled()')
        for reused in (True, False):
            for result in ('success', 'skipped', 'failure', 'cancelled'):
                with self.subTest(reused=reused, result=result):
                    selected = eval(expression, {'__builtins__': {}}, {
                        'inputs': SimpleNamespace(reuse_unsigned=reused),
                        'needs': SimpleNamespace(bundle=SimpleNamespace(result=result)),
                        'cancelled': lambda: False})
                    self.assertEqual(bool(selected), result == ('skipped' if reused else 'success'))

    def test_parallel_version_refuses_wrong_run_channel_or_event_before_key_import(self):
        listed = steps(job((ROOT / '.github/workflows/macos-cask.yml').read_text(), 'sign'))
        bind = next(s for s in listed if 'name: Bind the parallel candidate' in s)
        key = next(s for s in listed if 'name: Prepare a temporary signing keychain' in s)
        self.assertLess(listed.index(bind), listed.index(key))
        code = compile(inline_python(bind), 'parallel-mac-version', 'exec')
        release = (ROOT / 'DEV_VERSION').read_text().strip() + '-dev.1219'
        env = dict(CHANNEL='dev', EVENT_NAME='push', RUN_NUMBER='1219', RELEASE=release)
        old = os.getcwd()
        try:
            os.chdir(ROOT)
            with patch.dict(os.environ, env):
                exec(code, {})
            for change in [dict(CHANNEL='nightly'), dict(EVENT_NAME='pull_request'),
                           dict(RELEASE=release + '0'), dict(RUN_NUMBER='1218')]:
                with self.subTest(change=change), patch.dict(os.environ, {**env, **change}), self.assertRaises(AssertionError):
                    exec(code, {})
        finally:
            os.chdir(old)

    def test_stable_cannot_disable_desktop_mac_and_ios_is_independent(self):
        release = (ROOT / '.github/workflows/release.yml').read_text()
        verify = job(release, 'verify')
        self.assertIn('run_macos: true', verify)
        self.assertIn('run_ios: true', verify)
        code = compile(inline_python(job(release, 'version')), 'stable-version-gate', 'exec')
        env = dict(REQUESTED_VERSION=(ROOT / 'VERSION').read_text().strip(), MACOS_CASK='false', RUN_MACOS='false')
        old = os.getcwd()
        try:
            os.chdir(ROOT)
            with patch.dict(os.environ, env), self.assertRaisesRegex(AssertionError, 'full macOS'):
                exec(code, {})
            with patch.dict(os.environ, {**env, 'RUN_MACOS': 'true'}):
                exec(code, {})
        finally:
            os.chdir(old)

    def test_ios_archive_is_conditional_on_the_read_only_duplicate_check(self):
        workflow = (ROOT / '.github/workflows/ios-testflight.yml').read_text()
        upload = job(workflow, 'upload')
        listed = steps(upload)
        preflight = next(s for s in listed if 'id: preflight' in s)
        archive = next(s for s in listed if 'name: Archive, sign and upload' in s)
        self.assertLess(listed.index(preflight), listed.index(archive))
        self.assertIn("if: steps.preflight.outputs.upload == 'true'", archive)
        self.assertNotIn('IOS_DEVELOPMENT_P12', preflight)
        self.assertNotIn('security ', preflight)
        self.assertIn('run_ios: true', job(workflow, 'verify'))


if __name__ == '__main__':
    unittest.main()
