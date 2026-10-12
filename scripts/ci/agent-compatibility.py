#!/usr/bin/env python3
"""Keyless released Codex contracts and read-only upstream capability reports."""
import argparse
import base64
import datetime
import difflib
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = Path(__file__).with_suffix('.json')
NATIVE_MEMBER = 'package/vendor/x86_64-unknown-linux-musl/bin/codex'
MAX_PACKAGE_BYTES = 512 * 1024 * 1024
NATIVE_RUNNER = '''
import json, sys, unittest
suite = unittest.defaultTestLoader.loadTestsFromNames(['test_clear_context_native.NativeCodexClear',
                                                       'test_codex_update.NativeCodexUpdate',
                                                       'test_external_agents_native.NativeExternalAgents',
                                                       'test_overmind_mcp_native.NativeCodexMcp'])
result = unittest.TextTestRunner(verbosity=2).run(suite)
print('ZERUS_NATIVE_RESULT=' + json.dumps({'tests': result.testsRun, 'skipped': len(result.skipped)}))
sys.exit(not result.wasSuccessful() or bool(result.skipped) or result.testsRun == 0)
'''


def fetch_json(url):
    request = urllib.request.Request(url, headers={'User-Agent': 'Zerus-agent-compatibility'})
    with urllib.request.urlopen(request, timeout=30) as response:
        data = response.read(8 * 1024 * 1024 + 1)
    if len(data) > 8 * 1024 * 1024:
        raise ValueError('Upstream metadata exceeds its size limit')
    return json.loads(data)


def version_text(value):
    if not isinstance(value, str) or not re.fullmatch(r'[A-Za-z0-9._+-]{1,64}', value):
        raise ValueError('Invalid published version')
    return value


def discover_upstream(manifest):
    rows = []
    for source in manifest['upstream']:
        row = {key: source[key] for key in ('name', 'url', 'notes', 'reviewed_versions')}
        try:
            value = fetch_json(source['url'])
            for key in source['version_path']:
                value = value[key]
            row['version'] = version_text(value)
            row['review'] = 'reviewed pin' if value in source['reviewed_versions'] else 'needs review'
        except Exception as error:
            row['error'] = str(error)
        rows.append(row)
    return rows


def install_native(package, destination):
    """Verify published bytes, then extract exactly one regular native executable."""
    url = package['url']
    if not url.startswith('https://registry.npmjs.org/@openai/codex/-/codex-'):
        raise ValueError('Expected the official Codex package distribution')
    algorithm, encoded = package['integrity'].split('-', 1)
    if algorithm != 'sha512':
        raise ValueError('Expected a SHA-512 package pin')
    expected = base64.b64decode(encoded, validate=True)
    if len(expected) != 64:
        raise ValueError('Invalid package integrity digest')
    digest = hashlib.sha512()
    destination.mkdir(parents=True, exist_ok=True)
    archive = destination / 'native.tgz'
    size = 0
    try:
        with urllib.request.urlopen(url, timeout=60) as response, archive.open('wb') as output:
            while chunk := response.read(1024 * 1024):
                size += len(chunk)
                if size > MAX_PACKAGE_BYTES:
                    raise ValueError('Native package exceeds its size limit')
                digest.update(chunk)
                output.write(chunk)
        if digest.digest() != expected:
            raise ValueError('Native package integrity mismatch')
        with tarfile.open(archive, 'r:gz') as tar:
            member = tar.getmember(NATIVE_MEMBER)
            if not member.isfile() or not 0 < member.size <= MAX_PACKAGE_BYTES:
                raise ValueError('Expected one regular bounded native executable')
            native = destination / 'codex'
            with tar.extractfile(member) as source, native.open('wb') as output:
                shutil.copyfileobj(source, output)
        native.chmod(0o755)
        return native
    finally:
        archive.unlink(missing_ok=True)


def fixture_environment(home):
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(('HGS_', 'OPENAI_', 'ANTHROPIC_', 'CODEX_', 'DEEPSEEK_', 'KIMI_'))}
    home.mkdir(parents=True, exist_ok=True)
    (home / '.codex').mkdir(exist_ok=True)
    env.update(HOME=str(home), CODEX_HOME=str(home / '.codex'),
               PYTHONPATH=str(ROOT / 'tests'), TERM='xterm-256color')
    return env


def method_names(value):
    methods = set()
    if isinstance(value, dict):
        method = value.get('properties', {}).get('method', {})
        if isinstance(method, dict):
            methods.update(item for item in method.get('enum', []) if isinstance(item, str))
            if isinstance(method.get('const'), str):
                methods.add(method['const'])
        for child in value.values():
            methods.update(method_names(child))
    elif isinstance(value, list):
        for child in value:
            methods.update(method_names(child))
    return methods


def schema_snapshot(directory):
    methods, files = set(), {}
    for path in sorted(directory.rglob('*.json')):
        data = json.loads(path.read_text())
        methods.update(method_names(data))
        normalized = json.dumps(data, sort_keys=True, separators=(',', ':')).encode()
        files[str(path.relative_to(directory))] = hashlib.sha256(normalized).hexdigest()
    if not files or not methods:
        raise ValueError('Native protocol schema did not expose any methods')
    return {'methods': sorted(methods), 'files': files}


def schema_changes(before, after):
    first, second = before['files'], after['files']
    return {
        'added_methods': sorted(set(after['methods']) - set(before['methods'])),
        'removed_methods': sorted(set(before['methods']) - set(after['methods'])),
        'added_schemas': sorted(set(second) - set(first)),
        'removed_schemas': sorted(set(first) - set(second)),
        'changed_schemas': sorted(name for name in first.keys() & second.keys() if first[name] != second[name]),
    }


def qualify(package, work, results, hgs):
    version = version_text(package['version'])
    print('Checking real Codex', version, flush=True)
    row = {'version': version, 'package_integrity': package['integrity'], 'status': 'failed'}
    directory = results / version
    directory.mkdir(parents=True, exist_ok=True)
    try:
        native = install_native(package, work / version)
        env = fixture_environment(work / version / 'home')
        actual = subprocess.check_output([str(native), '--version'], env=env, text=True, timeout=20).strip()
        if actual != 'codex-cli ' + version:
            raise ValueError('Native executable version mismatch: ' + actual)
        help_text = subprocess.check_output([str(native), '--help'], env=env, text=True, timeout=20)
        (directory / 'cli-help.txt').write_text(help_text)
        row['cli_help_sha256'] = hashlib.sha256(help_text.encode()).hexdigest()
        for label, flags in [('stable', []), ('experimental', ['--experimental'])]:
            schemas = directory / ('schemas-' + label)
            subprocess.run([str(native), 'app-server', 'generate-json-schema', '--out', str(schemas), *flags],
                           env=env, check=True, capture_output=True, text=True, timeout=60)
            row['schema_' + label] = schema_snapshot(schemas)
        row['geometries'] = {}
        for geometry in ('95x47', '110x35'):
            for mode, animations in (('animated', 'true'), ('reduced', 'false')):
                case = geometry + '-' + mode
                test_env = dict(env, HGS_TEST_BIN=str(hgs), HGS_CODEX_TEST_BIN=str(native),
                                HGS_CODEX_TEST_SIZE=geometry, HGS_CODEX_TEST_ANIMATIONS=animations)
                run = subprocess.run([sys.executable, '-c', NATIVE_RUNNER], cwd=ROOT, env=test_env,
                                     capture_output=True, text=True, timeout=180)
                output = run.stdout + run.stderr
                (directory / (case + '.log')).write_text(output)
                print(output, flush=True)
                marker = next((line.removeprefix('ZERUS_NATIVE_RESULT=') for line in run.stdout.splitlines()
                               if line.startswith('ZERUS_NATIVE_RESULT=')), None)
                if marker is not None:
                    row['geometries'][case] = json.loads(marker)
                if run.returncode or marker is None:
                    raise ValueError('Native contract failed at ' + case)
        row['status'] = 'passed'
    except Exception as error:
        row['error'] = str(error)
        print('Codex', version, 'failed:', error, flush=True)
    return row


def write_report(report, results):
    (results / 'report.json').write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
    lines = ['# Native agent compatibility', '', 'Synthetic fixtures only; no model credentials or live sessions.', '']
    for row in report.get('upstream', []):
        if 'error' in row:
            lines.append('- ' + row['name'] + ': discovery failed (see report.json).')
        else:
            lines.append(f"- {row['name']}: `{row['version']}` ({row['review']}). [Official changes]({row['notes']})")
    lines.extend(['', '## Real Codex contracts', ''])
    for row in report['contracts']:
        lines.append(f"- `{row['version']}`: {row['status']}; clear, draft preservation, identity, the Overmind MCP server, two terminal sizes and animation modes.")
    for label, changes in report.get('capability_changes', {}).items():
        lines.extend(['', '## ' + label + ' protocol changes', '', 'Experimental methods remain separate from supported APIs.'])
        for kind in ('added_methods', 'removed_methods', 'added_schemas', 'removed_schemas', 'changed_schemas'):
            values = changes[kind]
            lines.append(f'- {kind.replace("_", " ")}: {len(values)}')
            if kind.endswith('methods'):
                lines.extend('  - `' + value.replace('`', '') + '`' for value in values)
    if 'cli_help_changed' in report:
        lines.extend(['', 'CLI help changed: ' + ('yes; review cli-help.diff' if report['cli_help_changed'] else 'no') + '.'])
    lines.extend(['', 'Review release notes and schema diffs before changing pins or implementing new features.', ''])
    summary = '\n'.join(lines)
    (results / 'summary.md').write_text(summary)
    if path := os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(path, 'a') as file:
            file.write(summary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--latest', action='store_true', help='Discover agent releases and qualify latest Codex as a canary')
    parser.add_argument('--results', type=Path, default=ROOT / 'artifacts/test-results/agent-compatibility')
    parser.add_argument('--hgs', type=Path, default=ROOT / 'target/debug/hgs')
    args = parser.parse_args()
    manifest = json.loads(MANIFEST.read_text())
    results = args.results.resolve()
    results.mkdir(parents=True, exist_ok=True)
    report = {'checked_at': datetime.datetime.now(datetime.timezone.utc).isoformat(), 'contracts': [], 'errors': []}
    packages = manifest['codex'][:]
    if args.latest:
        report['upstream'] = discover_upstream(manifest)
        report['errors'].extend(row['name'] + ': ' + row['error'] for row in report['upstream'] if 'error' in row)
        try:
            latest = next(row['version'] for row in report['upstream'] if row['name'] == 'Codex')
            if latest not in {package['version'] for package in packages}:
                data = fetch_json('https://registry.npmjs.org/@openai/codex/' + latest + '-linux-x64')
                if data['version'] != latest + '-linux-x64':
                    raise ValueError('Latest native package version mismatch')
                packages.append({'version': latest, 'url': data['dist']['tarball'], 'integrity': data['dist']['integrity']})
            report['canary_version'] = latest
        except Exception as error:
            report['errors'].append('Codex canary discovery: ' + str(error))
    with tempfile.TemporaryDirectory(prefix='zerus-native-compat-') as temporary:
        for package in packages:
            report['contracts'].append(qualify(package, Path(temporary), results, args.hgs.resolve()))
    current = next((row for row in report['contracts'] if row['version'] == manifest['codex'][-1]['version']), None)
    latest = next((row for row in report['contracts'] if row['version'] == report.get('canary_version')), None)
    if current and latest:
        report['capability_changes'] = {label: schema_changes(current['schema_' + label], latest['schema_' + label])
            for label in ('stable', 'experimental') if 'schema_' + label in current and 'schema_' + label in latest}
        if 'cli_help_sha256' in current and 'cli_help_sha256' in latest:
            report['cli_help_changed'] = current['cli_help_sha256'] != latest['cli_help_sha256']
            before = (results / current['version'] / 'cli-help.txt').read_text().splitlines(keepends=True)
            after = (results / latest['version'] / 'cli-help.txt').read_text().splitlines(keepends=True)
            (results / 'cli-help.diff').write_text(''.join(difflib.unified_diff(
                before, after, fromfile=current['version'], tofile=latest['version'])))
    write_report(report, results)
    return bool(report['errors'] or any(row['status'] != 'passed' for row in report['contracts']))


if __name__ == '__main__':
    sys.exit(main())
