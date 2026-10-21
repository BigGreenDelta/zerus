#!/usr/bin/env python3
"""Prepare public, source-bound inputs for the opt-in hosted hint contract.

No provider or hgs executable runs here. Fetch and build share one original
2400-second preparation end, separate from every native row's 1200 seconds.
"""
import argparse
import base64
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import time
import tomllib
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
PRE_FEATURE = 'ad3285c19013cf2ff78ec8eaf6ab2fbdd5253dce'
PREPARATION_SECONDS = 2400
FETCH_SECONDS = 600
BUILD_SECONDS = 900
HASH_PROFILE = 'profile.dev.package.sha2.opt-level=3'


def build_arguments(cargo, *targets):
    """Optimize the qualification images' hashing without changing defaults."""
    return [cargo, 'build', '--locked', '--offline', '-j2', '-vv',
            '--config', HASH_PROFILE, '--bin', 'hgs', *targets]


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, msg, headers, newurl):
        return None


def open_package(request, timeout):
    return urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect()).open(request, timeout=timeout)


def build_environment(environment):
    forbidden = re.compile(r'TOKEN|SECRET|PASSWORD|COOKIE|CREDENTIAL|ASKPASS', re.IGNORECASE)
    return {key: value for key, value in environment.items()
            if not forbidden.search(key) and key.upper() != 'SSH_AUTH_SOCK'
            and not key.upper().startswith(('HGS_', 'TMUX', 'ACTIONS_', 'GITHUB_', 'GH_',
                                            'ANTHROPIC_', 'CLAUDE', 'OPENAI_', 'CODEX_', 'KIMI_'))}


def digest(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def source_receipt(path, expected):
    def git(*args):
        return subprocess.check_output(['git', '-C', str(path), *args], timeout=15).decode().strip()
    if not re.fullmatch(r'[0-9a-f]{40}', expected) or git('rev-parse', 'HEAD') != expected:
        raise ValueError('Checkout does not match the exact reviewed source')
    if git('status', '--porcelain', '--untracked-files=normal'):
        raise ValueError('Build input checkout is dirty')
    for package in tomllib.loads((path / 'Cargo.lock').read_text())['package']:
        if 'source' in package and (package['source'] != 'registry+https://github.com/rust-lang/crates.io-index' or
                                   not re.fullmatch(r'[0-9a-f]{64}', package.get('checksum', ''))):
            raise ValueError('Only checksum-pinned public crates.io dependencies are admitted')
    return dict(commit=expected, tree=git('rev-parse', 'HEAD^{tree}'),
                cargo_lock_sha256=digest(path / 'Cargo.lock'))


def tool_receipts(cargo, rustc):
    tools = dict(cargo=cargo, rustc=rustc, python=Path(sys.executable))
    if os.name == 'nt':
        # Run this preparation under the existing msvc-env.py. Pin the actual
        # compiler, librarian, linker and SDK tools rather than only a VS label.
        for name in ('cl', 'link', 'lib', 'rc', 'mt'):
            found = shutil.which(name)
            if not found:
                raise ValueError('Missing MSVC/SDK tool: ' + name)
            tools[name] = Path(found)
    return {name: dict(path=str(Path(path).resolve()), sha256=digest(path)) for name, path in tools.items()}


def fetch_package(package, output, end):
    """One exact official URL, no discovery, redirect, retry or fallback."""
    destination = output / package['url'].rsplit('/', 1)[-1]
    partial = destination.with_suffix('.partial')
    expected = base64.b64decode(package['integrity'].removeprefix('sha512-'), validate=True)
    if not package['integrity'].startswith('sha512-') or len(expected) != 64:
        raise ValueError('Expected the reviewed SHA-512 package pin')
    remaining = end - time.monotonic()
    if remaining <= 0:
        raise TimeoutError('Original preparation fetch end exhausted')
    value, size = hashlib.sha512(), 0
    request = urllib.request.Request(package['url'], headers={'User-Agent': 'Zerus-hint-contract'})
    with open_package(request, timeout=min(60, remaining)) as response, partial.open('xb') as target:
        if response.geturl() != package['url']:
            raise ValueError('Pinned package redirected')
        while block := response.read(1024 * 1024):
            if time.monotonic() >= end:
                raise TimeoutError('Original preparation fetch end exhausted')
            size += len(block)
            if size > 512 * 1024 * 1024:
                raise ValueError('Pinned package exceeds 512 MiB')
            value.update(block)
            target.write(block)
    if value.digest() != expected:
        raise ValueError('Pinned package integrity mismatch')
    partial.rename(destination)
    return dict(package, filename=destination.name, bytes=size, sha256=digest(destination))


def prepare(args):
    start = time.monotonic()
    end = start + PREPARATION_SECONDS
    output = args.output.resolve()
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    report = dict(status='incomplete', os=platform.system(), arch=platform.machine(),
                  original_end_monotonic=end, preparation_seconds=PREPARATION_SECONDS,
                  compiler_jobs=2, steps=[])
    def save():
        (output / 'build-receipt.json').write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
    def command(label, argv, cwd, limit, env, deadline=end):
        remaining = min(limit, deadline - time.monotonic())
        if remaining <= 0:
            raise TimeoutError('Original preparation end exhausted before ' + label)
        row = dict(label=label, argv=list(map(str, argv)), source_directory=str(cwd),
                   timeout_seconds=remaining, started_monotonic=time.monotonic(), status='unknown')
        report['steps'].append(row)
        log = output / (label + '.log')
        row['raw_log'] = log.name
        save()
        try:
            with log.open('wb') as stream:
                result = subprocess.run(argv, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                        stdout=stream, stderr=subprocess.STDOUT, timeout=remaining)
            row.update(exit_code=result.returncode, status='passed' if result.returncode == 0 else 'failed')
            if result.returncode:
                raise ValueError(label + ' failed; see ' + log.name)
        finally:
            row['finished_monotonic'] = time.monotonic()
            row['raw_log_sha256'] = digest(log)
            save()
    save()
    try:
        report['candidate'] = source_receipt(ROOT, args.source)
        report['pre_feature'] = source_receipt(args.pre_feature, PRE_FEATURE)
        report['tools_before'] = tool_receipts(args.cargo, args.rustc)
        if args.cargo.parent != args.rustc.parent:
            raise ValueError('Direct cargo and rustc must belong to the same reviewed toolchain')
        env = dict(build_environment(os.environ), RUSTC=str(args.rustc), CARGO_BUILD_JOBS='2',
                   CARGO_INCREMENTAL='0', CARGO_NET_RETRY='0', CARGO_HTTP_TIMEOUT='60')
        # The private rustup bin directory also makes rustdoc/tool subprocess
        # selection independent of the runner's default Rust toolchain.
        env['PATH'] = str(args.rustc.parent) + os.pathsep + env['PATH']
        command('rustc-version', [args.rustc, '-vV'], ROOT, 15, env)
        if not (output / 'rustc-version.log').read_text().startswith('rustc 1.99.1 '):
            raise ValueError('The direct reviewed Rust 1.99.1 toolchain is required')
        if os.name == 'nt':
            env.update(CC=report['tools_before']['cl']['path'], AR=report['tools_before']['lib']['path'],
                       CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_LINKER=report['tools_before']['link']['path'])
            names = ('VCTOOLSINSTALLDIR', 'VCTOOLSVERSION', 'WINDOWSSDKDIR', 'WINDOWSSDKVERSION',
                     'UCRTVERSION', 'INCLUDE', 'LIB', 'LIBPATH')
            report['msvc_inputs'] = {name: env[name] for name in names if name in env}
        spec = importlib.util.spec_from_file_location('hint_compatibility', ROOT / 'scripts/ci/agent-compatibility.py')
        compatibility = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(compatibility)
        manifest = json.loads(compatibility.MANIFEST.read_text())
        compatibility.check_manifest(manifest)
        target = compatibility.host_target()
        packages = [row for row in manifest[args.agent] if row['target'] == target]
        expected_versions = (['2.1.296'] if args.agent == 'claude' and target in
                             ('linux-x64', 'darwin-arm64', 'win32-x64') else
                             ['0.160.1', '0.162.0'] if (args.agent, target) == ('codex', 'linux-x64') else
                             ['0.162.0'] if (args.agent, target) == ('codex', 'win32-x64') else [])
        if not expected_versions or [row['version'] for row in packages] != expected_versions:
            raise ValueError('Hosted hint allocation differs from its six reviewed pin/OS rows')
        for package in packages:
            expected_url = compatibility.AGENTS[args.agent]['tarball'].format(version=package['version'], target=target)
            if package['url'] != expected_url:
                raise ValueError('Expected the exact official pinned npm URL')
        report['packages'] = []
        package_cache = output / 'packages'
        package_cache.mkdir(mode=0o700)
        fetch_end = min(end, time.monotonic() + FETCH_SECONDS)
        host = next(line.split(': ', 1)[1] for line in (output / 'rustc-version.log').read_text().splitlines()
                    if line.startswith('host: '))
        for label, path in (('candidate-fetch', ROOT), ('pre-feature-fetch', args.pre_feature)):
            command(label, [args.cargo, 'fetch', '--locked', '--target', host], path, FETCH_SECONDS, env, fetch_end)
        for index, package in enumerate(packages):
            # One dedicated fetch child lets the parent enforce the original
            # shared fetch end even if a socket read is still blocked. This
            # worker starts no subprocesses or native programs.
            command('package-fetch-' + str(index), [sys.executable, str(Path(__file__).resolve()),
                    '--fetch-package', json.dumps(package), '--fetch-end', str(fetch_end),
                    '--output', str(package_cache)], ROOT, FETCH_SECONDS, env, fetch_end)
            receipt = package_cache / (package['url'].rsplit('/', 1)[-1] + '.json')
            report['packages'].append(json.loads(receipt.read_text()))
            save()
        for label, path, extra in (('candidate', ROOT, ['--example', 'session_start_observer']),
                                   ('pre-feature', args.pre_feature, [])):
            target_dir = output / (label + '-target')
            build_env = dict(env, CARGO_TARGET_DIR=str(target_dir))
            command(label + '-build', build_arguments(args.cargo, *extra), path, BUILD_SECONDS, build_env)
            report[label.replace('-', '_')]['build_target'] = str(target_dir)
        suffix = '.exe' if os.name == 'nt' else ''
        paths = dict(candidate=output / 'candidate-target/debug' / ('hgs' + suffix),
                     pre_feature=output / 'pre-feature-target/debug' / ('hgs' + suffix),
                     observer=output / 'candidate-target/debug/examples' / ('session_start_observer' + suffix))
        report['binaries'] = {name: dict(path=str(path), sha256=digest(path),
                            source=PRE_FEATURE if name == 'pre_feature' else args.source) for name, path in paths.items()}
        report['tools_after'] = tool_receipts(args.cargo, args.rustc)
        if report['tools_before'] != report['tools_after']:
            raise ValueError('Build tool bytes changed')
        if source_receipt(ROOT, args.source) != {k: v for k, v in report['candidate'].items() if k != 'build_target'} or \
                source_receipt(args.pre_feature, PRE_FEATURE) != {k: v for k, v in report['pre_feature'].items() if k != 'build_target'}:
            raise ValueError('Build source changed')
        if time.monotonic() >= end:
            raise TimeoutError('Original preparation end expired')
        report['status'] = 'prepared'
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        report['finished_monotonic'] = time.monotonic()
        save()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source')
    parser.add_argument('--pre-feature', type=lambda value: Path(value).resolve())
    parser.add_argument('--cargo', type=lambda value: Path(value).resolve())
    parser.add_argument('--rustc', type=lambda value: Path(value).resolve())
    parser.add_argument('--agent', choices=('claude', 'codex'))
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--fetch-package', help=argparse.SUPPRESS)
    parser.add_argument('--fetch-end', type=float, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.fetch_package is not None:
        if args.fetch_end is None:
            parser.error('Package worker needs the original fetch end')
        package = json.loads(args.fetch_package)
        record = fetch_package(package, args.output, args.fetch_end)
        (args.output / (record['filename'] + '.json')).write_text(json.dumps(record) + '\n')
    else:
        if not all((args.source, args.pre_feature, args.cargo, args.rustc, args.agent)):
            parser.error('Preparation needs source, pre-feature, direct cargo/rustc and agent inputs')
        prepare(args)


if __name__ == '__main__':
    main()
