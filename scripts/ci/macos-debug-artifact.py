#!/usr/bin/env python3
"""Transfer only the completed macOS unit/debug producer's two executables."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
MAX_BINARY = 512 * 1024 * 1024
FILES = {'hgs': 'target/debug/hgs', 'fake_agent': 'target/debug/examples/fake_agent'}
HELPERS = ('scripts/ci/cli.sh', 'scripts/ci/macos-debug-artifact.py',
           'scripts/ci/agent-compatibility.py', 'scripts/ci/agent-compatibility.json')
ARCHES = {'arm64': ('aarch64-apple-darwin', 0x0100000C),
          'x86_64': ('x86_64-apple-darwin', 0x01000007)}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def number(value):
    require(isinstance(value, str) and re.fullmatch(r'[1-9][0-9]{0,15}', value), 'Unknown positive identity')
    require(int(value) < 2**53, 'Identity is outside its exact wire range')
    return int(value)


def digest(value):
    require(isinstance(value, str) and re.fullmatch(r'[0-9a-f]{64}', value), 'Unknown SHA-256')
    return value


def release(value):
    require(isinstance(value, str) and re.fullmatch(r'[1-9][0-9]*\.[0-9]+\.[0-9]+', value), 'Unknown stable Rust release')
    return value


def admit(artifact_id, manifest_sha, producer_attempt, rust_release, current_attempt):
    number(artifact_id)  # One immutable artifact ID, never an empty/name/all-artifacts selector.
    digest(manifest_sha)
    require(number(producer_attempt) <= number(current_attempt), 'Producer attempt is in the future')
    release(rust_release)


def host():
    require(platform.system() == 'Darwin', 'Debug transfer requires an actual Darwin host')
    machine = platform.machine().lower()
    machine = {'aarch64': 'arm64', 'amd64': 'x86_64'}.get(machine, machine)
    require(machine in ARCHES, 'Unknown runtime architecture')
    return {'system': 'Darwin', 'machine': machine}


def context():
    require(os.environ.get('RUNNER_OS') == 'macOS', 'Unknown runner OS')
    observed = host()
    expected_arch = {'ARM64': 'arm64', 'X64': 'x86_64'}.get(os.environ.get('RUNNER_ARCH'))
    require(observed['machine'] == expected_arch, 'Runner label and runtime architecture disagree')
    source = os.environ.get('SOURCE_COMMIT', '')
    require(re.fullmatch(r'[0-9a-f]{40}', source), 'Unknown source object')
    actual = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True, timeout=10).strip()
    require(actual == source, 'Checkout differs from the selected source')
    changed = subprocess.check_output(['git', 'diff', '--no-ext-diff', '--name-only', 'HEAD', '--'],
                                      cwd=ROOT, text=True, timeout=10).strip()
    require(not changed, 'Tracked producer or consumer source was modified')
    repository = os.environ.get('GITHUB_REPOSITORY', '')
    require(re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository), 'Unknown repository')
    return dict(source=source, repository=repository, run_id=number(os.environ.get('GITHUB_RUN_ID', '')),
                attempt=number(os.environ.get('GITHUB_RUN_ATTEMPT', '')), platform=observed)


def identity(path, limit=MAX_BINARY, allow_linked=False):
    """Hash one regular no-follow leaf; no binary/version/help execution."""
    with os.fdopen(os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK), 'rb') as stream:
        before = os.fstat(stream.fileno())
        require(stat.S_ISREG(before.st_mode) and (allow_linked or before.st_nlink == 1) and 0 < before.st_size <= limit,
                'Unknown, linked or oversize artifact leaf')
        hashed = hashlib.sha256()
        remaining = before.st_size
        while remaining:
            block = stream.read(min(1024 * 1024, remaining))
            require(block, 'Artifact read was short')
            hashed.update(block)
            remaining -= len(block)
        after = os.fstat(stream.fileno())
        require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) ==
                (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns),
                'Artifact changed during hashing')
    return dict(bytes=before.st_size, sha256=hashed.hexdigest())


def macho(path, machine):
    with os.fdopen(os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK), 'rb') as stream:
        header = stream.read(8)
    require(len(header) == 8 and header[:4] == b'\xcf\xfa\xed\xfe' and
            struct.unpack('<I', header[4:])[0] == ARCHES[machine][1],
            'Executable is not the expected native 64-bit Mach-O')


def helpers(root):
    return {name: identity(root / name, 1024 * 1024)['sha256'] for name in HELPERS}


def toolchain(machine):
    verbose = subprocess.check_output(['rustc', '-Vv'], text=True, timeout=10)
    require(0 < len(verbose) <= 16384, 'Unknown Rust identity')
    version = re.search(r'(?m)^release: (.+)$', verbose)
    require(version is not None, 'Rust release is absent')
    version = release(version[1])
    require('\nhost: ' + ARCHES[machine][0] + '\n' in '\n' + verbose, 'Rust host differs from runtime architecture')
    compiler = Path(subprocess.check_output(['rustup', 'which', 'rustc'], text=True, timeout=10).strip())
    return dict(release=version, verbose=verbose, compiler=identity(compiler, allow_linked=True))


def prerequisites():
    versions = {'python': sys.version.splitlines()[0]}
    for name, arguments in (('tmux', ['tmux', '-V']), ('node', ['node', '--version'])):
        versions[name] = subprocess.check_output(arguments, text=True, timeout=10).strip()
    require(all(0 < len(value) <= 256 and not any(ord(c) < 32 for c in value)
                for value in versions.values()), 'Unknown prerequisite identity')
    return versions


def seal(root, directory, bound, compiler):
    require(not directory.exists() and not directory.is_symlink(), 'Producer directory already exists')
    require(set(bound) == {'source', 'repository', 'run_id', 'attempt', 'platform'}, 'Unknown producer binding')
    require(compiler['release'] == release(compiler['release']), 'Unknown compiler release')
    data = dict(schema=1, source=bound['source'], repository=bound['repository'], run_id=bound['run_id'],
                producer_attempt=bound['attempt'], platform=bound['platform'], toolchain=compiler,
                helpers=helpers(root), prerequisites=prerequisites(), files={})
    directory.mkdir(mode=0o700)
    for name, relative in FILES.items():
        origin = root / relative
        # Cargo may hard-link its finished deps/examples executable into this
        # public debug path. Read it without mutation; the exclusive handoff
        # copy and every downloaded artifact must still have one link.
        expected = identity(origin, allow_linked=True)
        macho(origin, bound['platform']['machine'])
        # Exclusive output files; only these public compiled executables leave the job.
        with os.fdopen(os.open(origin, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK), 'rb') as incoming, (directory / name).open('xb') as output:
            shutil.copyfileobj(incoming, output, 1024 * 1024)
        require(identity(directory / name) == expected, 'Producer copy changed')
        data['files'][name] = expected
    with (directory / 'manifest.json').open('x', encoding='utf-8') as output:
        json.dump(data, output, sort_keys=True)
        output.write('\n')
    return identity(directory / 'manifest.json', 65536)['sha256']


def verify(root, directory, bound, expected_sha, producer_attempt, rust_release):
    digest(expected_sha)
    require(not directory.is_symlink() and directory.is_dir(), 'Unknown artifact directory')
    require(set(p.name for p in directory.iterdir()) == set(FILES) | {'manifest.json'}, 'Unexpected artifact file set')
    with os.fdopen(os.open(directory / 'manifest.json', os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK), 'rb') as stream:
        metadata = os.fstat(stream.fileno())
        require(stat.S_ISREG(metadata.st_mode) and metadata.st_nlink == 1 and 0 < metadata.st_size <= 65536,
                'Unknown manifest leaf')
        raw = stream.read(65537)
    require(len(raw) == metadata.st_size and hashlib.sha256(raw).hexdigest() == expected_sha,
            'Producer manifest was changed')
    data = json.loads(raw)
    require(set(data) == {'schema', 'source', 'repository', 'run_id', 'producer_attempt', 'platform',
                         'toolchain', 'helpers', 'prerequisites', 'files'} and type(data['schema']) is int and data['schema'] == 1,
            'Unknown debug manifest schema')
    require(data['source'] == bound['source'] and data['repository'] == bound['repository'] and
            type(data['run_id']) is int and data['run_id'] == bound['run_id'], 'Artifact source or run differs')
    require(type(data['producer_attempt']) is int and data['producer_attempt'] == number(producer_attempt) and
            data['producer_attempt'] <= bound['attempt'], 'Artifact producer attempt differs')
    require(data['platform'] == bound['platform'], 'Artifact runtime platform differs')
    require(data['helpers'] == helpers(root), 'Source helpers differ from producer')
    require(set(data['prerequisites']) == {'python', 'tmux', 'node'} and all(
        isinstance(value, str) and 0 < len(value) <= 256 and not any(ord(c) < 32 for c in value)
        for value in data['prerequisites'].values()), 'Unknown producer prerequisites')
    compiler = data['toolchain']
    require(set(compiler) == {'release', 'verbose', 'compiler'} and compiler['release'] == release(rust_release),
            'Artifact Rust toolchain differs')
    require(isinstance(compiler['verbose'], str) and len(compiler['verbose']) <= 16384 and
            '\nhost: ' + ARCHES[bound['platform']['machine']][0] + '\n' in '\n' + compiler['verbose'] and
            '\nrelease: ' + rust_release + '\n' in '\n' + compiler['verbose'], 'Invalid producing Rust identity')
    require(set(compiler['compiler']) == {'bytes', 'sha256'} and type(compiler['compiler']['bytes']) is int and
            0 < compiler['compiler']['bytes'] <= MAX_BINARY, 'Unknown producing compiler')
    digest(compiler['compiler']['sha256'])
    require(set(data['files']) == set(FILES), 'Unknown executable set')
    for name in FILES:
        require(identity(directory / name) == data['files'][name], 'Executable digest or size differs')
        macho(directory / name, bound['platform']['machine'])
    return data


def install(root, directory, data, check_rust=False):
    if check_rust:
        require(toolchain(data['platform']['machine']) == data['toolchain'], 'Consumer Rust toolchain differs')
    for relative in FILES.values():
        destination = root / relative
        # Refuse cached symlink ancestors and leaves before publishing anything.
        for part in [destination, *destination.parents]:
            if part == root:
                break
            require(not part.is_symlink(), 'Cached executable path is linked')
    for name, relative in FILES.items():
        destination = root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(dir=destination.parent, prefix='.verified-debug-', delete=False) as output:
            temporary = Path(output.name)
            try:
                with os.fdopen(os.open(directory / name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK), 'rb') as incoming:
                    shutil.copyfileobj(incoming, output, 1024 * 1024)
                output.flush()
                require(identity(temporary) == data['files'][name], 'Executable changed before publication')
                os.fchmod(output.fileno(), 0o755)
                os.replace(temporary, destination)
            finally:
                temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('admit', 'seal', 'install'))
    parser.add_argument('--directory', type=Path)
    parser.add_argument('--artifact-id', default='')
    parser.add_argument('--manifest-sha256', default='')
    parser.add_argument('--producer-attempt', default='')
    parser.add_argument('--rust-release', default='')
    parser.add_argument('--check-rust', action='store_true')
    args = parser.parse_args()
    if args.mode == 'admit':
        admit(args.artifact_id, args.manifest_sha256, args.producer_attempt, args.rust_release,
              os.environ.get('GITHUB_RUN_ATTEMPT', ''))
        return
    bound = context()
    require(args.directory is not None, 'Artifact directory is absent')
    if args.mode == 'seal':
        compiler = toolchain(bound['platform']['machine'])
        sha = seal(ROOT, args.directory, bound, compiler)
        with open(os.environ['GITHUB_OUTPUT'], 'a', encoding='utf-8') as output:
            output.write(f"manifest_sha256={sha}\nproducer_attempt={bound['attempt']}\nrust_release={compiler['release']}\n")
    else:
        admit(args.artifact_id, args.manifest_sha256, args.producer_attempt, args.rust_release,
              os.environ.get('GITHUB_RUN_ATTEMPT', ''))
        data = verify(ROOT, args.directory, bound, args.manifest_sha256, args.producer_attempt, args.rust_release)
        install(ROOT, args.directory, data, args.check_rust)
        receipt = dict(artifact_id=args.artifact_id, manifest_sha256=args.manifest_sha256,
                       source=bound['source'], repository=bound['repository'], run_id=bound['run_id'],
                       producer_attempt=data['producer_attempt'], consumer_attempt=bound['attempt'],
                       platform=bound['platform'], toolchain=data['toolchain'], files=data['files'],
                       helpers=data['helpers'], producer_prerequisites=data['prerequisites'],
                       consumer_prerequisites=prerequisites())
        output = ROOT / 'artifacts/test-results/debug-input.json'
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open('x', encoding='utf-8') as stream:
            json.dump(receipt, stream, sort_keys=True)
            stream.write('\n')
        print('Verified debug input: ' + args.artifact_id + ' / ' + args.manifest_sha256)


if __name__ == '__main__':
    main()
