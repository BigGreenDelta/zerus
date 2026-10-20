"""Hosted Kimi helper controls: data, mocked HTTP/processes, no native agent."""
from email.message import Message
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import tarfile
from types import SimpleNamespace
import unittest
from unittest import mock
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('kimi_native_ci', ROOT / 'scripts/ci/kimi-native.py')
ci = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ci)


def headers(**values):
    result = Message()
    for name, value in values.items():
        result[name.replace('_', '-')] = str(value)
    return result


class Response:
    def __init__(self, body, declared=None):
        self.body, self.status, self.url, self.reads = body, 200, 'https://cdn.kimi.com/kimi-code/binaries/2.2.0/fixture', 0
        self.headers = headers(Content_Length=len(body) if declared is None else declared)

    def __enter__(self):
        return self

    def __exit__(self, *args):
        pass

    def read(self, bound):
        self.reads += 1
        value, self.body = self.body[:bound], self.body[bound:]
        return value


class KimiNativeCI(unittest.TestCase):
    def root(self):
        temporary = tempfile.TemporaryDirectory(prefix='hgs-kimi-ci-control-')
        self.addCleanup(temporary.cleanup)
        return Path(temporary.name)

    def test_official_manifests_match_all_four_frozen_pins(self):
        for platform in ('linux-x64', 'darwin-arm64'):
            pins = ci.checked_pins(platform)
            self.assertEqual(tuple(pins), ci.VERSIONS)
            self.assertTrue(all(pins[v][platform]['size'] < ci.IMAGE_LIMIT for v in ci.VERSIONS))
        for content in ('{"a":1,"a":2}', '{"object":{"a":1,"a":2}}'):
            with self.assertRaisesRegex(ValueError, 'duplicate'):
                ci.strict(content)

    def test_tmux_installer_keeps_linux_defaults_and_explicitly_enables_macos_unicode(self):
        # Only configure argv is under test. All network, archive, hash, make
        # and tmux commands are stand-ins; no compiler or native image runs.
        base = self.root()
        tools, runtime, prefix = base / 'bin', base / 'tmp', base / 'prefix'
        tools.mkdir(); runtime.mkdir(); (prefix / 'bin').mkdir(parents=True)
        arguments = base / 'arguments'
        configure = base / 'configure'
        configure.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$FIXTURE_ARGUMENTS"\n')
        configure.chmod(0o700)
        scripts = dict(curl='exit 0', sha256sum='cat >/dev/null', make='exit 0',
                       realpath='printf "%s\\n" "$2"',
                       tar='mkdir tmux-3.7c; cp "$FIXTURE_CONFIGURE" tmux-3.7c/configure')
        for name, body in scripts.items():
            path = tools / name
            path.write_text('#!/bin/sh\n' + body + '\n'); path.chmod(0o700)
        tmux = prefix / 'bin/tmux'
        tmux.write_text('#!/bin/sh\nprintf "tmux 3.7c\\n"\n'); tmux.chmod(0o700)
        for system in ('Linux', 'Darwin'):
            (tools / 'uname').write_text('#!/bin/sh\nprintf "' + system + '\\n"\n')
            (tools / 'uname').chmod(0o700)
            env = dict(PATH=str(tools) + ':/usr/bin:/bin', RUNNER_TEMP=str(runtime),
                       FIXTURE_CONFIGURE=str(configure), FIXTURE_ARGUMENTS=str(arguments))
            result = subprocess.run([shutil.which('bash'), str(ROOT / 'scripts/ci/install-tmux.sh'), str(prefix)],
                                    env=env, text=True, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(arguments.read_text().splitlines(), ['--prefix=' + str(prefix)] +
                             (['--enable-utf8proc', '--enable-jemalloc'] if system == 'Darwin' else []))
            self.assertEqual(list(runtime.iterdir()), [])

    def test_only_reviewed_architectures_and_exact_eight_methods_are_selected(self):
        self.assertEqual(ci.target('Linux', 'x86_64'), 'linux-x64')
        self.assertEqual(ci.target('Darwin', 'arm64'), 'darwin-arm64')
        for pair in (('Linux', 'aarch64'), ('Darwin', 'x86_64'), ('Windows', 'AMD64')):
            with self.assertRaises(ValueError):
                ci.target(*pair)
        self.assertEqual(ci.checked_methods(), ci.METHODS)
        self.assertEqual(len(ci.METHODS), len(set(ci.METHODS)))

    def test_the_only_redirect_is_one_exact_302_to_the_cdn(self):
        source = 'https://code.kimi.com/kimi-code/binaries/2.2.0/manifest.json'
        target = source.replace('code.kimi.com', 'cdn.kimi.com')
        redirect = ci.OfficialRedirect()
        accepted = redirect.redirect_request(urllib.request.Request(source), None, 302, '', headers(Location=target), target)
        self.assertEqual((accepted.full_url, accepted.get_method()), (target, 'GET'))
        with self.assertRaises(ValueError):
            redirect.redirect_request(accepted, None, 302, '', headers(Location=target), target)
        for changed in (target.replace('https:', 'http:'), target + '?query', target + '#fragment',
                        target.replace('cdn.kimi.com', 'cdn.kimi.com:443'), target.replace('https://', 'https://user@'),
                        target.replace('/2.2.0/', '/2.1.1/'), target.replace('cdn.kimi.com', 'other.invalid')):
            with self.subTest(target=changed), self.assertRaises(ValueError):
                ci.OfficialRedirect().redirect_request(urllib.request.Request(source), None, 302, '', headers(Location=changed), changed)
        for code in (301, 303, 307, 308):
            with self.assertRaises(ValueError):
                ci.OfficialRedirect().redirect_request(urllib.request.Request(source), None, code, '', headers(Location=target), target)
        repeated = headers(Location=target)
        repeated['Location'] = target
        with self.assertRaises(ValueError):
            ci.OfficialRedirect().redirect_request(urllib.request.Request(source), None, 302, '', repeated, target)

    def fetch(self, response, expected=b'image', sha=None):
        output = self.root() / 'image'
        opener = mock.Mock()
        opener.open.return_value = response
        record = ci.fetch('https://code.kimi.com/kimi-code/binaries/2.2.0/fixture', output,
                          len(expected), sha or hashlib.sha256(expected).hexdigest(), opener=opener)
        return output, record, opener

    def test_download_is_one_exclusive_nonexecutable_exact_file(self):
        output, record, opener = self.fetch(Response(b'image'))
        self.assertEqual(output.read_bytes(), b'image')
        self.assertEqual(output.stat().st_mode & 0o777, 0o400)
        self.assertEqual(record['sha256'], hashlib.sha256(b'image').hexdigest())
        opener.open.assert_called_once()
        with self.assertRaises(FileExistsError):
            ci.fetch(record['url'], output, 5, record['sha256'], opener=opener)

    def test_status_length_duplicates_and_encoding_refuse_before_body(self):
        for change in ('status', 'short', 'oversized', 'duplicate', 'compressed'):
            response = Response(b'image')
            if change == 'status':
                response.status = 206
            elif change == 'duplicate':
                response.headers['Content-Length'] = '5'
            elif change == 'compressed':
                response.headers['Content-Encoding'] = 'gzip'
            else:
                response.headers.replace_header('Content-Length', '4' if change == 'short' else '6')
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.fetch(response)
            self.assertEqual(response.reads, 0)

    def test_short_extra_and_wrong_hash_never_become_executable(self):
        for body, checksum in ((b'ima', None), (b'images', None), (b'image', '0' * 64)):
            output = self.root() / 'image'
            opener = mock.Mock()
            opener.open.return_value = Response(body, declared=5)
            with self.subTest(body=body), self.assertRaises(ValueError):
                ci.fetch('https://code.kimi.com/kimi-code/binaries/2.2.0/fixture', output, 5,
                         checksum or hashlib.sha256(b'image').hexdigest(), opener=opener)
            self.assertEqual(output.stat().st_mode & 0o111, 0)

    def test_a_late_read_is_refused(self):
        with mock.patch.object(ci.time, 'monotonic', side_effect=[0, 1, 181]):
            with self.assertRaisesRegex(TimeoutError, 'late'):
                self.fetch(Response(b'image'))

    def test_exit_zero_without_exact_cases_and_clean_result_is_not_a_pass(self):
        good = dict(tests=8, started=list(ci.METHODS), completed=list(ci.METHODS), failures=0,
                    errors=0, skipped=0, expected_failures=0, unexpected_successes=0)
        self.assertTrue(ci.outcome(good, 0))
        self.assertFalse(ci.outcome(good, 1))
        for name in ('failures', 'errors', 'skipped', 'expected_failures', 'unexpected_successes'):
            for value in (None, False, 0.0, 1):
                with self.subTest(name=name, value=value):
                    self.assertFalse(ci.outcome(dict(good, **{name: value}), 0))
        for value in (0, 7, 9, 8.0, '8'):
            self.assertFalse(ci.outcome(dict(good, tests=value), 0))
        for name in ('started', 'completed'):
            self.assertFalse(ci.outcome(dict(good, **{name: list(ci.METHODS[:-1])}), 0))
            self.assertFalse(ci.outcome(dict(good, **{name: list(reversed(ci.METHODS))}), 0))

    def test_child_environment_is_private_and_does_not_inherit_credentials(self):
        root = self.root()
        with mock.patch.dict(os.environ, {'OPENAI_API_KEY': 'synthetic', 'GITHUB_TOKEN': 'synthetic',
                                         'KIMI_CODE_BASE_URL': 'https://other.invalid', 'HGS_SESSION': 'old'}):
            env = ci.environment(root, root / 'helpers', root / 'hgs')
        self.assertFalse(set(env) & {'OPENAI_API_KEY', 'GITHUB_TOKEN', 'KIMI_CODE_BASE_URL', 'HGS_SESSION'})
        self.assertEqual(env['PATH'], str(root / 'helpers') + ':/usr/bin:/bin')
        self.assertTrue(all(Path(env[key]).is_dir() for key in ('HOME', 'TMPDIR', 'XDG_STATE_HOME')))

    def test_owner_machine_and_nonprivate_build_paths_refuse_before_allocation(self):
        temporary = self.root()
        args = SimpleNamespace(hgs=temporary / 'hgs', tmux=temporary / 'tmux',
                               output=ci.ROOT / 'artifacts/test-results/kimi-native')
        env = dict(GITHUB_ACTIONS='true', RUNNER_ENVIRONMENT='github-hosted',
                   RUNNER_TEMP=str(temporary), GITHUB_WORKSPACE=str(ci.ROOT))
        ci.hosted_paths(args, env)
        for changes in (dict(GITHUB_ACTIONS=''), dict(RUNNER_ENVIRONMENT='self-hosted'),
                        dict(GITHUB_WORKSPACE=str(temporary))):
            with self.assertRaises(ValueError):
                ci.hosted_paths(args, dict(env, **changes))
        args.hgs = Path('/usr/bin/hgs')
        with self.assertRaisesRegex(ValueError, 'private'):
            ci.hosted_paths(args, env)
        self.assertFalse(args.output.exists())

    def test_timeout_stops_only_the_owned_child_and_reports_unknown(self):
        log = self.root() / 'log'
        log.write_bytes(b'')
        child = mock.Mock()
        child.poll.return_value = None
        child.wait.side_effect = [subprocess.TimeoutExpired('suite', 10), 0]
        with mock.patch.object(ci.time, 'monotonic', side_effect=[0, 2]):
            with self.assertRaisesRegex(TimeoutError, 'UNKNOWN'):
                ci.await_child(child, log, seconds=1)
        child.terminate.assert_called_once_with()
        child.kill.assert_called_once_with()
        self.assertEqual(child.wait.call_count, 2)

    def test_custody_uses_only_declared_private_root_attributes(self):
        base = self.root()
        root = base / 'hgs-kimi-native-example'
        root.mkdir(mode=0o700)
        case = mock.Mock(root=root, images=None, image=None, kimi=None)
        case.id.return_value = 'fixture.case'
        output = base / 'custody.jsonl'
        ci.record_roots(case, output)
        row = json.loads(output.read_text())
        self.assertEqual((row['path'], row['inode'], row['case']), (str(root.resolve()), root.stat().st_ino, 'fixture.case'))
        # macOS's temporary directory also has a /var -> /private/var spelling.
        alias = base / 'alias'
        alias.symlink_to(base, target_is_directory=True)
        case.root = alias / root.name
        ci.record_roots(case, output)
        alias_row = json.loads(output.read_text().splitlines()[-1])
        self.assertEqual((alias_row['path'], alias_row['inode']), (row['path'], row['inode']))
        case.root = base
        with self.assertRaisesRegex(ValueError, 'declared'):
            ci.record_roots(case, output)

    def test_retention_never_follows_a_runtime_symlink(self):
        base = self.root()
        runtime = base / 'runtime'
        runtime.mkdir(mode=0o700)
        outside = base / 'outside'
        outside.write_text('outside bytes')
        (runtime / 'link').symlink_to(outside)
        (runtime / 'inside').write_text('fixture bytes')
        archive = base / 'retained.tar.gz'
        result = ci.retain([], runtime, archive)
        self.assertEqual(result['members'], 3)
        with tarfile.open(archive) as data:
            self.assertEqual(data.getmember('root-0/link').type, tarfile.SYMTYPE)
            self.assertEqual(data.extractfile('root-0/inside').read(), b'fixture bytes')
            self.assertNotIn(b'outside bytes', b''.join(data.extractfile(row).read() for row in data.getmembers() if row.isfile()))

    def test_retention_refuses_a_changed_root_and_special_file(self):
        base = self.root()
        runtime = base / 'runtime'
        runtime.mkdir(mode=0o700)
        custody = base / 'custody'
        custody.write_text(json.dumps(dict(path=str(runtime), device=runtime.stat().st_dev,
                                          inode=runtime.stat().st_ino + 1, uid=os.getuid())) + '\n')
        with self.assertRaisesRegex(ValueError, 'identity'):
            ci.retain([custody], runtime, base / 'wrong.tar.gz')
        os.mkfifo(runtime / 'fifo', mode=0o600)
        with self.assertRaisesRegex(ValueError, 'special'):
            ci.retain([], runtime, base / 'special.tar.gz')

    def test_workflow_is_readonly_optional_and_uses_standard_ephemeral_runners(self):
        workflow = (ROOT / '.github/workflows/kimi-native.yml').read_text()
        self.assertIn('  pull_request:', workflow)
        self.assertIn('  workflow_dispatch:', workflow)
        self.assertNotIn('pull_request_target', workflow)
        self.assertIn('permissions:\n  contents: read\ncache-mode: none', workflow)
        for forbidden in ('secrets.', 'secrets: inherit', 'environment:', 'actions/cache', 'self-hosted', 'contents: write'):
            self.assertNotIn(forbidden, workflow)
        self.assertIn('os: ["ubuntu-24.04", "macos-15"]', workflow)
        self.assertIn('CARGO_BUILD_JOBS: 2', workflow)
        for variable, directory in (('CARGO_TARGET_DIR', 'target'), ('CARGO_HOME', 'cargo'), ('RUSTUP_HOME', 'rustup')):
            self.assertIn('echo "' + variable + '=$RUNNER_TEMP/kimi-' + directory + '"', workflow)
        self.assertIn('} >> "$GITHUB_ENV"', workflow)
        self.assertIn('brew install coreutils libevent ncurses utf8proc jemalloc bison pkg-config', workflow)
        self.assertIn('$(brew --prefix utf8proc)/lib/pkgconfig', workflow)
        self.assertIn('$(brew --prefix jemalloc)/lib/pkgconfig', workflow)
        installer = (ROOT / 'scripts/ci/install-tmux.sh').read_text()
        self.assertIn('if [ "$(uname -s)" = Darwin ]; then', installer)
        self.assertIn('persist-credentials: false', workflow)
        exact_source = '${{ github.event.pull_request.head.sha || github.sha }}'
        self.assertIn('SOURCE_SHA: ' + exact_source, workflow)
        self.assertIn('ref: ' + exact_source, workflow)
        for family in ('results', 'retained'):
            self.assertIn('name: kimi-native-' + family + '-${{ matrix.os }}-${{ env.SOURCE_SHA }}', workflow)


if __name__ == '__main__':
    unittest.main()
