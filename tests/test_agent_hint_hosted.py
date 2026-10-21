"""SDK-free contracts for the opt-in hosted gate; no compiler/native execution."""
import base64
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import re
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from test_ci_security import field, job

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('prepare_hint', ROOT / 'scripts/ci/prepare-agent-hint.py')
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


class HostedHintContract(unittest.TestCase):
    def workflow(self):
        return (ROOT / '.github/workflows/agent-hint.yml').read_text()

    def test_only_the_qualification_build_argv_optimize_the_existing_sha2_dependency(self):
        cargo = Path('/fixture/toolchain/cargo')
        base = [cargo, 'build', '--locked', '--offline', '-j2', '-vv', '--config',
                'profile.dev.package.sha2.opt-level=3', '--bin', 'hgs']
        self.assertEqual(prepare.build_arguments(cargo), base)
        self.assertEqual(prepare.build_arguments(cargo, '--example', 'session_start_observer'),
                         base + ['--example', 'session_start_observer'])
        self.assertEqual((prepare.PREPARATION_SECONDS, prepare.FETCH_SECONDS, prepare.BUILD_SECONDS),
                         (2400, 600, 900))

    def test_five_public_disposable_jobs_are_explicitly_opted_in(self):
        text = self.workflow()
        self.assertIn('  workflow_dispatch:', text)
        self.assertIn("contains(github.event.pull_request.labels.*.name, 'native-agent-hint')", text)
        self.assertIn("github.repository == 'ufna/zerus' && github.event.repository.private == false", text)
        self.assertEqual(re.findall(r'- os: (\S+)\n            agent: (\S+)', text), [
            ('ubuntu-24.04', 'claude'), ('ubuntu-24.04', 'codex'), ('macos-15', 'claude'),
            ('windows-2025', 'claude'), ('windows-2025', 'codex')])
        self.assertEqual(field(job(text, 'native'), 'timeout-minutes'), '120')
        self.assertIn('cache-mode: none', text)
        self.assertIn('permissions:\n  contents: read', text)
        for forbidden in ('secrets.', 'secrets: inherit', 'actions/cache', 'self-hosted', '--latest',
                          'workflow_call:', 'workflow_run:', 'schedule:', 'contents: write'):
            self.assertNotIn(forbidden, text)

    def test_rows_keep_source_association_and_separate_windows_host_budget(self):
        text = self.workflow()
        self.assertIn('ref: ${{ env.HINT_SOURCE }}', text)
        self.assertIn('ref: ' + prepare.PRE_FEATURE, text)
        self.assertEqual(text.count('persist-credentials: false'), 2)
        self.assertEqual(text.count('--hgs-source "$HINT_SOURCE"'), 2)
        self.assertEqual(text.count('--observer-source "$HINT_SOURCE"'), 2)
        self.assertEqual(text.count('--contract agent-hint --agent "$HINT_AGENT"'), 2)
        self.assertIn('outside-job.py --timeout 1320', text)
        self.assertIn('timeout-minutes: 28', text)
        self.assertIn('artifacts/results', text)
        self.assertIn('artifacts/inputs/*.log', text)
        self.assertLess(text.index('Fresh private caches and runtime'), text.index('actions/setup-python@'))
        self.assertIn('test "${#hint_tmp}" -le 24', text)

    def test_package_fetch_checks_exact_bytes_and_keeps_a_mismatch(self):
        package = dict(url='https://registry.npmjs.org/@openai/codex/-/codex-0.162.0-linux-x64.tgz',
                       integrity='sha512-' + base64.b64encode(hashlib.sha512(b'expected').digest()).decode())
        with tempfile.TemporaryDirectory() as temporary:
            response = io.BytesIO(b'changed')
            response.geturl = lambda: package['url']
            with patch.object(prepare, 'open_package', return_value=response) as request:
                with self.assertRaisesRegex(ValueError, 'integrity mismatch'):
                    prepare.fetch_package(package, Path(temporary), prepare.time.monotonic() + 10)
            self.assertEqual(request.call_count, 1)
            self.assertEqual(len(list(Path(temporary).glob('*.partial'))), 1)
            self.assertFalse(list(Path(temporary).glob('*.tgz')))

    def test_package_fetch_has_no_late_attempt_or_redirect_fallback(self):
        package = dict(url='https://registry.npmjs.org/@openai/codex/-/codex-0.162.0-linux-x64.tgz',
                       integrity='sha512-' + base64.b64encode(hashlib.sha512(b'expected').digest()).decode())
        with tempfile.TemporaryDirectory() as temporary:
            with patch.object(prepare, 'open_package') as request:
                with self.assertRaises(TimeoutError):
                    prepare.fetch_package(package, Path(temporary), prepare.time.monotonic() - 1)
                request.assert_not_called()
            response = io.BytesIO(b'expected')
            response.geturl = lambda: 'https://example.invalid/changed'
            with patch.object(prepare, 'open_package', return_value=response) as request:
                with self.assertRaisesRegex(ValueError, 'redirected'):
                    prepare.fetch_package(package, Path(temporary), prepare.time.monotonic() + 10)
                self.assertEqual(request.call_count, 1)

    def test_build_children_drop_credentials_and_owner_context(self):
        environment = dict(PATH='/fixture/bin', HOME='/fixture/home', CARGO_HOME='/fixture/cargo',
                           GITHUB_TOKEN='secret', ACTIONS_RUNTIME_TOKEN='secret', SSH_AUTH_SOCK='/owner/socket',
                           HGS_SESSION='owner', OPENAI_API_KEY='secret', CUSTOM_PASSWORD='secret')
        self.assertEqual(prepare.build_environment(environment),
                         dict(PATH='/fixture/bin', HOME='/fixture/home', CARGO_HOME='/fixture/cargo'))
        self.assertIsNone(prepare.NoRedirect().redirect_request(None, None, 302, '', {}, 'https://example.invalid'))

    def test_source_receipt_refuses_dirty_wrong_or_private_dependency_inputs(self):
        source = '1' * 40
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'Cargo.lock').write_text('[[package]]\nname="example"\nversion="1.0.0"\n')
            with patch.object(prepare.subprocess, 'check_output', side_effect=[b'2' * 40]):
                with self.assertRaisesRegex(ValueError, 'exact reviewed source'):
                    prepare.source_receipt(root, source)
            with patch.object(prepare.subprocess, 'check_output', side_effect=[source.encode(), b' M src/main.rs']):
                with self.assertRaisesRegex(ValueError, 'dirty'):
                    prepare.source_receipt(root, source)
            (root / 'Cargo.lock').write_text('[[package]]\nname="example"\nversion="1.0.0"\nsource="git+https://example.invalid/private"\n')
            with patch.object(prepare.subprocess, 'check_output', side_effect=[source.encode(), b'']):
                with self.assertRaisesRegex(ValueError, 'public crates.io'):
                    prepare.source_receipt(root, source)

    def test_preparation_refusal_keeps_an_incomplete_receipt_without_starting_work(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'inputs'
            args = SimpleNamespace(output=output, source='1' * 40, pre_feature=Path(temporary))
            with patch.object(prepare, 'source_receipt', side_effect=ValueError('wrong source')), \
                    patch.object(prepare.subprocess, 'run') as command, \
                    patch.object(prepare, 'open_package') as download:
                with self.assertRaisesRegex(ValueError, 'wrong source'):
                    prepare.prepare(args)
                command.assert_not_called()
                download.assert_not_called()
            record = json.loads((output / 'build-receipt.json').read_text())
            self.assertEqual(record['status'], 'incomplete')
            self.assertEqual(record['steps'], [])
            self.assertEqual(record['error'], 'wrong source')


if __name__ == '__main__':
    unittest.main()
