"""Exercise the Dev-only phase admission, dependency gates and bounded locks."""
import itertools
import os
from pathlib import Path
import re
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import test_ci_security as security
from test_ci_security import field, inline_python, job

ROOT = Path(__file__).resolve().parents[1]


def evaluate(expression, *, event='push', repository='ufna/zerus', ref='refs/heads/main',
             enabled='true', private=False, test_ref='', run_id=100, cancelled=False):
    expression = expression.removeprefix('${{ ').removesuffix(' }}')
    expression = expression.replace('&&', ' and ').replace('||', ' or ')
    expression = re.sub(r'!(?!=)', 'not ', expression)
    expression = re.sub(r'\bfalse\b', 'False', expression)
    return eval(expression, {'__builtins__': {}}, {
        'github': SimpleNamespace(repository=repository, ref=ref, event_name=event,
            sha='a' * 40, workflow='CI', run_id=run_id,
            event=SimpleNamespace(repository=SimpleNamespace(private=private))),
        'vars': SimpleNamespace(DEV_UPDATES_ENABLED=enabled),
        'inputs': SimpleNamespace(ref=test_ref), 'cancelled': lambda: cancelled,
        'format': lambda template, *values: template.format(*values),
    })


def concurrency_value(expression, **context):
    return evaluate(expression, **context) if expression.startswith('${{ ') else expression


class PhaseQueues(unittest.TestCase):
    def setUp(self):
        workflows = ROOT / '.github/workflows'
        self.ci = (workflows / 'ci.yml').read_text()
        self.qualification = (workflows / 'ci-dev-qualification.yml').read_text()
        self.tail = (workflows / 'ci-dev-tail.yml').read_text()
        self.publisher = (workflows / 'publish-dev.yml').read_text()
        self.helper = security.WorkflowSecurityTests()

    def selected(self, **context):
        return [name for name in ('automatic', 'review', 'qualification')
                if evaluate(field(job(self.ci, name), 'if'), **context)]

    def test_only_public_upstream_dev_main_push_enters_the_new_qualification(self):
        # Includes pre-rollout, private/fork pushes, PR/manual/full-Stable reuse
        # and a source override: none may acquire the new privileged Dev route.
        for overrides, expected in [({}, 'qualification'), ({'enabled': ''}, 'automatic'),
                ({'private': True}, 'automatic'), ({'repository': 'fork/zerus'}, 'review'),
                ({'event': 'pull_request', 'ref': 'refs/pull/7/merge'}, 'review'),
                ({'event': 'pull_request_target'}, 'review'),
                ({'event': 'workflow_dispatch'}, 'review'),
                ({'event': 'workflow_dispatch', 'test_ref': 'b' * 40}, 'review'),
                ({'event': 'schedule'}, 'automatic'),
                ({'ref': 'refs/heads/topic'}, 'review'),
                ({'test_ref': 'b' * 40}, 'review'),
                ({'test_ref': 'a' * 40}, 'qualification')]:
            with self.subTest(overrides=overrides):
                self.assertEqual(self.selected(**overrides), [expected])
        self.assertEqual(field(job(self.ci, 'review'), 'cache-mode'), 'read')
        for name in ('automatic', 'qualification'):
            self.assertEqual(field(job(self.ci, name), 'cache-mode'), 'write')

    def test_run_unique_top_group_is_limited_to_the_exact_dev_predicate(self):
        group = re.search(r'(?m)^  group: (.*)$', self.ci)[1]
        self.assertNotEqual(evaluate(group, run_id=100), evaluate(group, run_id=101))
        for overrides in ({'enabled': ''}, {'private': True}, {'repository': 'fork/zerus'},
                          {'event': 'workflow_dispatch'}, {'event': 'pull_request'},
                          {'event': 'schedule'}, {'ref': 'refs/heads/topic'}):
            with self.subTest(overrides=overrides):
                self.assertEqual(evaluate(group, run_id=100, **overrides),
                                 evaluate(group, run_id=101, **overrides))
                self.assertEqual(evaluate(group, **overrides), 'ci-CI-' + overrides.get('ref', 'refs/heads/main'))
        self.assertEqual(evaluate(group, event='workflow_dispatch', test_ref='b' * 40), 'ci-CI-' + 'b' * 40)

    def test_rollout_qualification_waits_on_the_legacy_whole_run_lock(self):
        caller = job(self.ci, 'qualification')
        qualification = re.search(r'(?m)^      group: (.*)$', caller)[1]
        top = re.search(r'(?m)^  group: (.*)$', self.ci)[1]
        tail = re.search(r'(?m)^      group: (.*)$', job(self.ci, 'dev-tail'))[1]
        publisher = re.search(r'(?m)^  group: (.*)$', self.publisher)[1]
        # These are the old CI key's two admitted source forms. The caller
        # shares that lock with an active old run, but never its own new top.
        for test_ref in ('', 'a' * 40):
            with self.subTest(test_ref=test_ref):
                legacy = 'ci-CI-' + (test_ref or 'refs/heads/main')
                self.assertEqual(self.selected(test_ref=test_ref), ['qualification'])
                self.assertEqual(concurrency_value(qualification, test_ref=test_ref), legacy)
                self.assertNotEqual(evaluate(top, test_ref=test_ref), legacy)
                self.assertNotIn(legacy, (tail, publisher))
        self.assertIn('      cancel-in-progress: false\n', caller)

    def test_non_dev_legacy_top_never_acquires_the_same_nested_lock(self):
        qualification = job(self.ci, 'qualification')
        child_group = re.search(r'(?m)^      group: (.*)$', qualification)[1]
        top_group = re.search(r'(?m)^  group: (.*)$', self.ci)[1]
        for overrides in ({'enabled': ''}, {'private': True}, {'repository': 'fork/zerus'},
                          {'event': 'pull_request', 'ref': 'refs/pull/7/merge'},
                          {'event': 'workflow_dispatch'},
                          {'event': 'workflow_dispatch', 'test_ref': 'b' * 40},
                          {'event': 'schedule'}, {'ref': 'refs/heads/topic'}):
            with self.subTest(overrides=overrides):
                # Reusing the legacy top is safe only because this entire
                # reusable caller is skipped before acquiring its job lock.
                self.assertEqual(concurrency_value(top_group, **overrides), concurrency_value(child_group, **overrides))
                self.assertFalse(evaluate(field(qualification, 'if'), **overrides))
                self.assertNotIn('qualification', self.selected(**overrides))
        self.assertEqual(self.selected(test_ref='b' * 40), ['review'])
        self.assertEqual(concurrency_value(child_group, test_ref='b' * 40), 'ci-CI-' + 'b' * 40)

    def test_distinct_caller_locks_span_whole_phases_without_nested_self_lock(self):
        groups = []
        for name, callee in (('qualification', self.qualification), ('dev-tail', self.tail)):
            caller = job(self.ci, name)
            groups.append(re.search(r'(?m)^      group: (.*)$', caller)[1])
            self.assertIn('      cancel-in-progress: false\n', caller)
            self.assertNotIn('concurrency:', callee)
            self.assertNotRegex(caller + callee, r'(?m)^\s*queue:')
            # The lock is on the reusable caller, not a short admission step.
            self.assertIn('    uses: ./.github/workflows/ci-dev-', caller)
            self.assertNotIn('    runs-on:', caller)
        groups.append(re.search(r'(?m)^  group: (.*)$', self.publisher)[1])
        self.assertEqual(len(set(groups)), 3)
        self.assertIn('  cancel-in-progress: false\n', self.publisher)
        self.assertEqual(field(job(self.qualification, 'gate'), 'needs'), '[checks, bundle]')
        self.assertEqual(field(job(self.tail, 'dev'), 'needs'), '[dev-android, dev-macos]')
        dev = (ROOT / '.github/workflows/dev.yml').read_text()
        self.assertEqual(field(job(dev, 'channel'), 'needs'), '[desktop]')
        self.assertIn('types: [completed]', self.publisher)

    def test_qualification_gate_fails_closed_on_missing_or_failed_selected_bundle(self):
        code = compile(inline_python(job(self.qualification, 'gate')), 'Dev-qualification-gate', 'exec')
        for macos in ('', 'true'):
            for checks, bundle in itertools.product(('success', 'failure', 'cancelled', 'skipped'), repeat=2):
                success = checks == 'success' and bundle == ('success' if macos == 'true' else 'skipped')
                with self.subTest(macos=macos, checks=checks, bundle=bundle), patch.dict(os.environ,
                        {'CHECKS': checks, 'BUNDLE': bundle, 'MACOS': macos}):
                    if success:
                        exec(code, {})
                    else:
                        with self.assertRaises(SystemExit):
                            exec(code, {})

    def test_checks_and_bundle_begin_without_credentials_or_each_others_result(self):
        self.assertNotIn('secrets:', self.qualification)
        self.assertNotIn('environment:', self.qualification)
        for name in ('checks', 'bundle'):
            caller = job(self.qualification, name)
            self.assertNotIn('needs:', caller)
            self.assertTrue(self.helper.dev_selected(self.qualification, name, {}))
            for override in (dict(event='pull_request'), dict(event='workflow_dispatch'),
                    dict(event='schedule'), dict(private=True), dict(enabled=''),
                    dict(repository='fork/zerus'), dict(cancelled=True)):
                with self.subTest(name=name, override=override):
                    self.assertFalse(self.helper.dev_selected(self.qualification, name, {}, **override))
        self.assertFalse(self.helper.dev_selected(self.qualification, 'bundle', {}, macos=''))

    def test_same_run_unsigned_output_passes_only_through_the_guarded_tail(self):
        self.assertIn('value: ${{ jobs.bundle.outputs.release }}', self.qualification)
        caller = job(self.ci, 'dev-tail')
        self.assertIn('unsigned_release: ${{ needs.qualification.outputs.unsigned_release }}', caller)
        self.assertIn('unsigned_release: ${{ inputs.unsigned_release }}', job(self.tail, 'dev-macos'))
        self.assertIn('reuse_unsigned: true', job(self.tail, 'dev-macos'))
        self.assertNotIn('workflow_run:', self.qualification + self.tail)
        self.assertNotIn('workflow_dispatch:', self.qualification + self.tail)
        self.assertNotIn('run_id:', self.qualification + self.tail)

    def test_only_named_signers_cross_the_main_only_tail_bridge(self):
        names = {'ANDROID_KEYSTORE_BASE64', 'ANDROID_STORE_PASSWORD', 'ANDROID_KEY_ALIAS',
                 'ANDROID_KEY_PASSWORD', 'ANDROID_GOOGLE_SERVICES_JSON',
                 'MACOS_DEVELOPER_ID_P12', 'MACOS_DEVELOPER_ID_P12_PASSWORD',
                 'APPLE_NOTARY_API_KEY_P8', 'APPLE_NOTARY_API_KEY_ID', 'APPLE_NOTARY_API_ISSUER_ID'}
        caller = job(self.ci, 'dev-tail')
        self.assertEqual(dict(re.findall(r'(?m)^      (\w+): \$\{\{ secrets\.(\w+) \}\}$', caller)),
                         {name: name for name in names})
        for text in (self.ci, self.qualification, self.tail):
            self.assertNotIn('secrets: inherit', text)
        self.assertNotIn('secrets.', self.ci.replace(caller, ''))
        declarations = self.tail.split('\npermissions:', 1)[0]
        for name in names:
            # Disabled Mac must not add a signing prerequisite. The selected
            # existing macos-cask workflow still requires its signing inputs.
            required = 'true' if name.startswith('ANDROID_') else 'false'
            self.assertIn('      ' + name + ':\n        required: ' + required, declarations)
        self.assertRegex(self.tail, r'(?m)^cache-mode: none$')
        self.assertNotIn('actions/cache', self.tail)

    def test_legacy_full_suite_caller_names_are_retained(self):
        self.assertEqual(field(job(self.ci, 'automatic'), 'name'), 'Trusted main checks')
        self.assertEqual(field(job(self.ci, 'review'), 'name'), 'PR and manual checks')
        for name in ('automatic', 'review'):
            self.assertEqual(field(job(self.ci, name), 'uses'), './.github/workflows/checks.yml')
        nightly = (ROOT / '.github/workflows/nightly.yml').read_text()
        stable = (ROOT / '.github/workflows/release.yml').read_text()
        for text in (nightly, stable):
            verify = job(text, 'verify')
            for name in ('linux', 'arch', 'windows', 'ios'):
                self.assertIn('run_' + name + ': true', verify)
            self.assertEqual(field(verify, 'uses'), './.github/workflows/checks.yml')


if __name__ == '__main__':
    unittest.main()
