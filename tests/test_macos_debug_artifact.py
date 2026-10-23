"""Pure debug-handoff controls; no compiler, uploaded binary or native agent runs."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import itertools
import test_ci_security as security
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('mac_debug', ROOT / 'scripts/ci/macos-debug-artifact.py')
artifact = importlib.util.module_from_spec(spec)
spec.loader.exec_module(artifact)


class Fixture:
    def __init__(self, root):
        self.root = root
        self.directory = root / 'handoff'
        self.bound = dict(source='a' * 40, repository='example/zerus', run_id=101, attempt=1,
                          platform=dict(system='Darwin', machine='arm64'))
        self.compiler = dict(release='1.99.1', verbose='rustc 1.99.1\nhost: aarch64-apple-darwin\nrelease: 1.99.1\n',
                             compiler=dict(bytes=16, sha256='b' * 64))
        for relative in artifact.HELPERS:
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('Synthetic source helper ' + relative)
        for relative in artifact.FILES.values():
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'\xcf\xfa\xed\xfe' + struct.pack('<I', 0x0100000C) + b'synthetic-not-executable')
        with patch.object(artifact, 'prerequisites', return_value=dict(python='fixture', node='v24.0.0', tmux='tmux 3.8')):
            self.sha = artifact.seal(root, self.directory, self.bound, self.compiler)

    def verify(self, **overrides):
        return artifact.verify(self.root, self.directory, overrides.get('bound', self.bound),
            overrides.get('sha', self.sha), overrides.get('attempt', '1'), overrides.get('release', '1.99.1'))

    def change(self, edit):
        path = self.directory / 'manifest.json'
        data = json.loads(path.read_text())
        edit(data)
        path.write_text(json.dumps(data))
        self.sha = hashlib.sha256(path.read_bytes()).hexdigest()


class ArtifactControls(unittest.TestCase):
    def test_read_only_cargo_hardlinks_become_exclusive_handoff_copies(self):
        with tempfile.TemporaryDirectory() as folder:
            f = Fixture(Path(folder))
            for name, relative in artifact.FILES.items():
                os.link(f.root / relative, f.root / (name + '-cargo-uplift'))
            second = f.root / 'second-handoff'
            with patch.object(artifact, 'prerequisites', return_value=dict(python='fixture', node='v24', tmux='3.8')):
                sha = artifact.seal(f.root, second, f.bound, f.compiler)
            data = artifact.verify(f.root, second, f.bound, sha, '1', '1.99.1')
            for name, relative in artifact.FILES.items():
                self.assertEqual((f.root / relative).stat().st_nlink, 2)
                self.assertEqual((second / name).stat().st_nlink, 1)
                self.assertEqual(data['files'][name], artifact.identity(f.root / relative, allow_linked=True))

    def test_exact_minimal_receipt_and_partial_rerun_keep_the_original_producer(self):
        with tempfile.TemporaryDirectory() as folder:
            f = Fixture(Path(folder))
            data = f.verify(bound={**f.bound, 'attempt': 3})
            self.assertEqual(data['producer_attempt'], 1)
            self.assertEqual(set(p.name for p in f.directory.iterdir()), {'manifest.json', 'hgs', 'fake_agent'})
            for name in ('hgs', 'fake_agent'):
                self.assertEqual(data['files'][name]['sha256'], hashlib.sha256((f.directory / name).read_bytes()).hexdigest())
            self.assertEqual(data['toolchain'], f.compiler)

    def test_prelookup_admission_rejects_empty_multiple_unknown_or_future_ids(self):
        valid = ['123', 'a' * 64, '1', '1.99.1', '2']
        artifact.admit(*valid)
        for index, values in [(0, ['', '0', '1,2', 'latest', str(2**53)]),
                              (1, ['', 'f' * 63, 'A' * 64]), (2, ['', '0', '3']),
                              (3, ['stable', 'nightly', '1.99.1;exit']), (4, ['', '0'])]:
            for value in values:
                args = valid.copy(); args[index] = value
                with self.subTest(index=index, value=value), self.assertRaises(ValueError):
                    artifact.admit(*args)

    def test_source_run_platform_attempt_toolchain_and_unknown_fields_refuse(self):
        mutations = [lambda d: d.update(source='b' * 40), lambda d: d.update(run_id=102),
            lambda d: d.update(run_id=True), lambda d: d.update(repository='other/zerus'),
            lambda d: d.update(producer_attempt=2), lambda d: d.update(schema=True),
            lambda d: d.update(unknown=True), lambda d: d['platform'].update(machine='x86_64'),
            lambda d: d['toolchain'].update(release='1.99.2'),
            lambda d: d['toolchain'].update(verbose='host: x86_64-apple-darwin\nrelease: 1.99.1\n'),
            lambda d: d['toolchain']['compiler'].update(sha256='unknown'),
            lambda d: d['helpers'].update({'unknown': 'a' * 64}),
            lambda d: d['prerequisites'].update(secret='no')]
        for edit in mutations:
            with self.subTest(edit=edit), tempfile.TemporaryDirectory() as folder:
                f = Fixture(Path(folder)); f.change(edit)
                with self.assertRaises(ValueError): f.verify()

    def test_missing_extra_tampered_linked_or_wrong_machine_bytes_refuse(self):
        def missing(f): (f.directory / 'fake_agent').unlink()
        def extra(f): (f.directory / 'private.json').write_text('must not be uploaded')
        def tampered(f): (f.directory / 'hgs').write_bytes(b'changed')
        def symlink(f):
            (f.directory / 'hgs').unlink(); (f.directory / 'hgs').symlink_to(f.root / 'target/debug/hgs')
        def hardlink(f): os.link(f.directory / 'hgs', f.root / 'alias')
        def wrong_macho(f):
            path=f.directory / 'hgs'; path.write_bytes(b'\xcf\xfa\xed\xfe' + struct.pack('<I', 0x01000007))
            f.change(lambda d: d['files'].update(hgs=artifact.identity(path)))
        for edit in (missing, extra, tampered, symlink, hardlink, wrong_macho):
            with self.subTest(edit=edit.__name__), tempfile.TemporaryDirectory() as folder:
                f = Fixture(Path(folder)); edit(f)
                with self.assertRaises((ValueError, OSError)): f.verify()

    def test_digest_helper_change_and_rebound_attempt_are_not_accepted(self):
        with tempfile.TemporaryDirectory() as folder:
            f = Fixture(Path(folder))
            with self.assertRaises(ValueError): f.verify(sha='c' * 64)
            with self.assertRaises(ValueError): f.verify(attempt='2')
            (f.root / artifact.HELPERS[0]).write_text('changed helper')
            with self.assertRaises(ValueError): f.verify()

    def test_actual_runtime_platform_disagrees_with_runner_label(self):
        env=dict(RUNNER_OS='macOS', RUNNER_ARCH='ARM64', SOURCE_COMMIT='a' * 40,
                 GITHUB_REPOSITORY='example/zerus', GITHUB_RUN_ID='101', GITHUB_RUN_ATTEMPT='1')
        with patch.dict(os.environ, env), patch.object(artifact.platform, 'system', return_value='Darwin'), \
             patch.object(artifact.platform, 'machine', return_value='arm64'), \
             patch.object(artifact.subprocess, 'check_output', side_effect=lambda argv, **kw:
                          'a' * 40 + '\n' if argv[1] == 'rev-parse' else ''):
            self.assertEqual(artifact.context()['platform'], {'system':'Darwin', 'machine':'arm64'})
            with patch.object(artifact.platform, 'machine', return_value='x86_64'), self.assertRaises(ValueError): artifact.context()
            with patch.object(artifact.platform, 'system', return_value='Linux'), self.assertRaises(ValueError): artifact.context()
            with patch.object(artifact.subprocess, 'check_output', side_effect=['a' * 40 + '\n', 'src/cli.rs\n']), self.assertRaises(ValueError): artifact.context()

    def test_install_cli_retains_public_input_receipt_without_running_a_subject(self):
        with tempfile.TemporaryDirectory() as folder:
            f=Fixture(Path(folder)); current={**f.bound,'attempt':2}
            args=['macos-debug-artifact.py','install','--directory',str(f.directory),'--artifact-id','123',
                  '--manifest-sha256',f.sha,'--producer-attempt','1','--rust-release','1.99.1']
            with patch.object(artifact,'ROOT',f.root),patch.object(artifact,'context',return_value=current), \
                 patch.object(artifact,'prerequisites',return_value={'python':'fixture','node':'v24','tmux':'3.8'}), \
                 patch.dict(os.environ,GITHUB_RUN_ATTEMPT='2'),patch.object(artifact.sys,'argv',args):
                artifact.main()
            record=json.loads((f.root/'artifacts/test-results/debug-input.json').read_text())
            self.assertEqual((record['producer_attempt'],record['consumer_attempt']),(1,2))
            self.assertEqual(record['artifact_id'],'123')
            self.assertEqual(record['files'],f.verify()['files'])
            self.assertNotIn('environment',record)

    def test_verified_install_checks_exact_compiler_and_refuses_cached_links(self):
        with tempfile.TemporaryDirectory() as folder:
            f=Fixture(Path(folder)); data=f.verify()
            with patch.object(artifact, 'toolchain', return_value={**f.compiler, 'release':'1.99.2'}), self.assertRaises(ValueError):
                artifact.install(f.root, f.directory, data, check_rust=True)
            fake=f.root / artifact.FILES['fake_agent']; fake.unlink(); fake.symlink_to(f.root / 'elsewhere')
            hgs=f.root / artifact.FILES['hgs']; before=hgs.read_bytes()
            with patch.object(artifact, 'toolchain', return_value=f.compiler), self.assertRaises(ValueError):
                artifact.install(f.root, f.directory, data, check_rust=True)
            self.assertEqual(hgs.read_bytes(), before)
            fake.unlink()
            with patch.object(artifact, 'toolchain', return_value=f.compiler):
                artifact.install(f.root, f.directory, data, check_rust=True)
            for relative in artifact.FILES.values(): self.assertEqual((f.root / relative).stat().st_mode & 0o777, 0o755)


class CliPhases(unittest.TestCase):
    def test_default_preserves_every_command_and_split_excludes_only_the_other_phase(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); script=root/'scripts/ci/cli.sh';script.parent.mkdir(parents=True)
            script.write_bytes((ROOT/'scripts/ci/cli.sh').read_bytes())
            binaries=root/'bin';binaries.mkdir()
            for name in ('cargo','python3','bash'):
                path=binaries/name
                path.write_text('#!/bin/sh\n[ -z "${HGS_SHOULD_NOT_EXIST:-}" ] || exit 88\nprintf "%s:%s\\n" '+name+' "$*" >> "$CALL_LOG"\n')
                path.chmod(0o755)
            results={}
            for phase in ('all','debug','remaining','unknown'):
                log=root/(phase+'.log');env=dict(os.environ, PATH=str(binaries)+':/usr/bin:/bin', CALL_LOG=str(log), HGS_SHOULD_NOT_EXIST='contaminated')
                result=subprocess.run(['/usr/bin/bash',str(script),*([phase] if phase!='all' else [])],env=env,capture_output=True,text=True)
                self.assertEqual(result.returncode, 2 if phase=='unknown' else 0,result.stderr)
                results[phase]=log.read_text().splitlines() if log.exists() else []
            debug=['python3:-','cargo:test --locked','cargo:build --locked','cargo:build --locked --example fake_agent']
            remaining=['python3:-','bash:tests/test_hgs.sh','python3:scripts/ci/python-tests.py','cargo:build --release --locked']
            self.assertEqual(results['debug'],debug);self.assertEqual(results['remaining'],remaining)
            self.assertEqual(results['all'],debug+remaining[1:]);self.assertEqual(results['unknown'],[])


class WorkflowContracts(unittest.TestCase):
    def setUp(self):
        self.checks=(ROOT/'.github/workflows/checks.yml').read_text()

    def test_completed_producer_forks_two_source_bound_siblings_without_polling(self):
        job,field=security.job,security.field
        producer=job(self.checks,'cli-macos-debug')
        self.assertEqual(field(producer,'needs'),'source')
        self.assertEqual(field(producer,'cache-mode'),'read')
        self.assertNotIn('actions/cache/save@',producer)
        self.assertLess(producer.index('scripts/ci/cli.sh debug'),producer.index('id: seal'))
        self.assertLess(producer.index('id: seal'),producer.index('id: debug-upload'))
        self.assertNotIn('cli.sh remaining',producer)
        for name in ('cli-macos-core','cli-macos-native'):
            body=job(self.checks,name)
            self.assertEqual(field(body,'needs'),'[source, cli-macos-debug]')
            self.assertIn("needs.cli-macos-debug.result == 'success'",field(body,'if'))
            self.assertIn('artifact-ids: ${{ needs.cli-macos-debug.outputs.artifact_id }}',body)
            self.assertNotIn('pattern:',body)
            self.assertNotIn('github-token:',body)
            self.assertLess(body.index('macos-debug-artifact.py admit'),body.index('actions/download-artifact@'))
            self.assertLess(body.index('macos-debug-artifact.py install'),body.index('cli.sh remaining' if name.endswith('core') else 'agent-compatibility.py --hgs'))
            self.assertNotIn('sleep',body)
            self.assertNotIn('while ',body)
        core=job(self.checks,'cli-macos-core');native=job(self.checks,'cli-macos-native')
        self.assertIn('rustup toolchain install "$MAC_DEBUG_RUST" --profile minimal',core)
        self.assertNotIn('rustup toolchain install stable',core)
        self.assertIn('--check-rust',core)
        self.assertLess(core.index('cli.sh remaining'),core.index('name: Save Rust cache'))
        self.assertEqual(field(native,'cache-mode'),'none')
        self.assertNotIn('actions/cache',native)
        self.assertNotIn('rustup',native)
        self.assertEqual(field(job(self.checks,'cli-macos'),'name'),'CLI (macos-15)')
        self.assertIn('run: bash scripts/ci/cli.sh\n',job(self.checks,'cli'))

    def test_public_macos_aggregate_rejects_every_incomplete_dependency(self):
        body=security.job(self.checks,'cli-macos')
        code=compile(security.inline_python(body),'mac-cli-aggregate','exec')
        names=('source','cli-macos-debug','cli-macos-core','cli-macos-native')
        for values in itertools.product(('success','failure','cancelled','skipped'),repeat=4):
            data={name:{'result':result} for name,result in zip(names,values)}
            with self.subTest(values=values),patch.dict(os.environ,RESULTS=json.dumps(data)):
                if all(v=='success' for v in values): exec(code,{})
                else:
                    with self.assertRaises(SystemExit): exec(code,{})
        for missing in names:
            with patch.dict(os.environ,RESULTS=json.dumps({n:{'result':'success'} for n in names if n!=missing})),self.assertRaises(SystemExit): exec(code,{})

    def test_selected_and_full_suite_gate_requires_new_native_even_if_aggregate_is_faked_success(self):
        body=security.job(self.checks,'gate');code=compile(security.inline_python(body),'suite-results','exec')
        mac=('cli-macos-debug','cli-macos-core','cli-macos-native','cli-macos','connector-macos','mac-desktop')
        always=('source','quick','relay','connector','android')
        for linux,macos in itertools.product((False,True),repeat=2):
            results={name:{'result':'success'} for name in always}
            results.update({name:{'result':'success' if macos else 'skipped'} for name in mac})
            results.update({name:{'result':'skipped'} for name in ('arch','arch-package','ios','windows-host','windows-desktop')})
            results['cli']={'result':'success' if linux else 'skipped'}
            env=dict(RESULTS=json.dumps(results),RUN_LINUX=json.dumps(linux),RUN_MACOS=json.dumps(macos),RUN_ARCH='false',RUN_WINDOWS='false',RUN_IOS='false')
            with patch.dict(os.environ,env):exec(code,{})
            for name in mac[:4]:
                for result in ('failure','cancelled','skipped' if macos else 'success'):
                    changed=copy.deepcopy(results);changed[name]={'result':result}
                    with self.subTest(linux=linux,macos=macos,name=name,result=result),patch.dict(os.environ,{**env,'RESULTS':json.dumps(changed)}),self.assertRaises(SystemExit):exec(code,{})
                changed=copy.deepcopy(results);del changed[name]
                with patch.dict(os.environ,{**env,'RESULTS':json.dumps(changed)}),self.assertRaises(KeyError):exec(code,{})


if __name__ == '__main__': unittest.main()
