#!/usr/bin/env python3
"""One trusted-main stable Android candidate; never publish or accept a command/ref."""
import argparse
import base64
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import signal
import stat
import subprocess
import sys
import tarfile
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = 'ufna/zerus'
SECRETS = ('ANDROID_KEYSTORE_BASE64', 'ANDROID_STORE_PASSWORD', 'ANDROID_KEY_ALIAS',
           'ANDROID_KEY_PASSWORD', 'ANDROID_GOOGLE_SERVICES_JSON')
WHOLE_SECONDS = 1800
CLEANUP_SECONDS = 60
MAX_LOG = 64 * 1024**2
SEMVER = r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def before(end, clock=time.monotonic):
    require(clock() < end, 'Original candidate deadline expired.')


def context(env):
    require(env.get('GITHUB_REPOSITORY') == REPOSITORY and
            env.get('GITHUB_REF') == 'refs/heads/main' and
            env.get('GITHUB_EVENT_NAME') == 'workflow_dispatch', 'Trusted upstream main dispatch required.')
    sha = env.get('REQUESTED_SOURCE_COMMIT', '')
    require(re.fullmatch('[0-9a-f]{40}', sha) and sha == env.get('GITHUB_SHA'), 'Exact workflow source SHA required.')
    version = env.get('REQUESTED_VERSION', '')
    require(re.fullmatch(SEMVER, version), 'Stable semantic version required.')
    numbers = [env.get(name, '') for name in ('REQUESTED_VERSION_CODE', 'REQUESTED_CI_RUN_ID',
                                             'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT')]
    require(all(re.fullmatch('[1-9][0-9]{0,19}', n) for n in numbers), 'Positive exact IDs/code required.')
    code, ci_run, run, attempt = map(int, numbers)
    require(code <= 2_100_000_000, 'Android code exceeds platform limit.')
    signer = env.get('REQUESTED_SIGNING_CERT_SHA256', '')
    require(re.fullmatch('[0-9a-f]{64}', signer), 'Explicit reviewed existing signer SHA-256 required.')
    return dict(source_commit=sha, version=version, version_code=code, ci_run_id=ci_run,
                run_id=run, run_attempt=attempt, signing_cert_sha256=signer)


def stable_identity(text, code_text, request):
    versions = re.findall(r'^val stableVersionName = "([^"]+)"$', text, re.M)
    require(versions == [request['version']] and re.fullmatch(SEMVER, versions[0]),
            'Requested version must equal committed stableVersionName.')
    require(code_text.strip() == str(request['version_code']), 'Requested code must equal committed VERSION_CODE.')


def validate_ci(run, workflow, jobs, request):
    require(type(run.get('id')) is int and run['id'] == request['ci_run_id'], 'Wrong main CI run.')
    require(run.get('path') == '.github/workflows/ci.yml' and run.get('event') == 'push' and
            run.get('head_branch') == 'main' and run.get('head_sha') == request['source_commit'] and
            run.get('head_repository', {}).get('full_name') == REPOSITORY and
            run.get('repository', {}).get('full_name') == REPOSITORY,
            'Successful PR/manual/fork/other-source CI cannot admit signing.')
    require(run.get('status') == 'completed' and run.get('conclusion') == 'success', 'Full main CI must finish successfully.')
    require(workflow.get('path') == '.github/workflows/ci.yml' and
            type(workflow.get('id')) is int and workflow['id'] == run.get('workflow_id'), 'Wrong CI workflow identity.')
    attempt = run.get('run_attempt')
    require(type(attempt) is int and attempt > 0, 'Unknown main CI attempt.')
    # latest is deliberate: no guessing/reusing an absent earlier-attempt gate.
    for name in ('CI gate', 'Trusted main checks / Suite results'):
        found = [j for j in jobs if j.get('name') == name]
        require(len(found) == 1 and found[0].get('status') == 'completed' and
                found[0].get('conclusion') == 'success' and found[0].get('run_attempt') == attempt,
                'Both exact full-matrix main CI gates must succeed in the selected attempt.')
    return dict(run_id=run['id'], run_attempt=attempt, workflow_id=workflow['id'])


def json_api(endpoint, end):
    before(end)
    request = urllib.request.Request('https://api.github.com/repos/' + REPOSITORY + '/' + endpoint,
                                     headers={'Authorization': 'Bearer ' + os.environ['GH_TOKEN'],
                                              'Accept': 'application/vnd.github+json',
                                              'X-GitHub-Api-Version': '2022-11-28'})
    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, req, fp, code, msg, headers, newurl):
            raise ValueError('Main CI lookup redirects are not admitted.')
    with urllib.request.build_opener(NoRedirect).open(request, timeout=min(30, end - time.monotonic())) as response:
        raw = response.read(2 * 1024**2 + 1)
    before(end)
    require(len(raw) <= 2 * 1024**2, 'Main CI API lookup exceeded bounds.')
    return json.loads(raw)


def main_ci(request, end):
    run = json_api('actions/runs/' + str(request['ci_run_id']), end)
    workflow = json_api('actions/workflows/ci.yml', end)
    jobs = []
    for page in range(1, 11):
        data = json_api(f"actions/runs/{request['ci_run_id']}/jobs?filter=latest&per_page=100&page={page}", end)
        require(type(data.get('total_count')) is int and 0 < data['total_count'] <= 1000 and
                isinstance(data.get('jobs'), list), 'Invalid bounded main CI job inventory.')
        jobs += data['jobs']
        if len(jobs) >= data['total_count']:
            require(len(jobs) == data['total_count'], 'Main CI job inventory changed.')
            return validate_ci(run, workflow, jobs, request)
        require(len(data['jobs']) == 100, 'Incomplete main CI job inventory.')
    raise ValueError('Main CI inventory exceeded bounds.')


def write_new(path, data, mode=0o600):
    with os.fdopen(os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, mode), 'wb') as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def json_new(path, value):
    write_new(path, (json.dumps(value, indent=2, sort_keys=True) + '\n').encode())
    descriptor = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def run_root(env, request):
    parent = Path(env['RUNNER_TEMP'])
    require(parent.is_absolute() and parent.is_dir() and not parent.is_symlink(), 'Canonical runner temporary root required.')
    return parent / f"zerus-stable-android-{request['run_id']}-{request['run_attempt']}"


def source_check(request):
    sha = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    require(sha == request['source_commit'], 'Checked-out source differs from workflow source.')
    require(not subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=all'], cwd=ROOT),
            'Exact clean source required before signing.')
    stable_identity((ROOT / 'mobile/android/app/build.gradle.kts').read_text(),
                    (ROOT / 'mobile/android/VERSION_CODE').read_text(), request)


def admit(env):
    request = context(env)
    end = time.monotonic() + WHOLE_SECONDS
    source_check(request)
    ci = main_ci(request, min(end - CLEANUP_SECONDS, time.monotonic() + 120))
    before(end - CLEANUP_SECONDS)
    root = run_root(env, request)
    root.mkdir(mode=0o700)  # exclusive, after trust/full CI; no old admission reuse
    info = root.stat()
    # Every generation process starts after this boot-clock instant.
    admitted = time.clock_gettime(time.CLOCK_BOOTTIME)
    json_new(root / 'admission.json', dict(schema=1, request=request, ci=ci, end=end, admitted=admitted,
                                         root_dev=info.st_dev, root_ino=info.st_ino, uid=os.getuid()))
    print('Exact main CI and stable source admitted; no signing input created.')


def admission(env):
    request = context(env)
    root = run_root(env, request)
    info = root.lstat()
    require(stat.S_ISDIR(info.st_mode) and info.st_uid == os.getuid() and stat.S_IMODE(info.st_mode) == 0o700,
            'Owned private admission root required.')
    path = root / 'admission.json'
    metadata = path.lstat()
    require(stat.S_ISREG(metadata.st_mode) and metadata.st_nlink == 1 and metadata.st_uid == os.getuid() and
            stat.S_IMODE(metadata.st_mode) == 0o600 and metadata.st_size <= 8192, 'Private admission record required.')
    data = json.loads(path.read_text())
    require(data.get('schema') == 1 and data.get('request') == request and data.get('uid') == info.st_uid and
            data.get('root_dev') == info.st_dev and data.get('root_ino') == info.st_ino,
            'Admission or generation root changed.')
    require(type(data.get('end')) in (int, float) and math.isfinite(data['end']) and data['end'] > 0,
            'Original end missing.')
    require(type(data.get('admitted')) in (int, float) and math.isfinite(data['admitted']) and data['admitted'] > 0,
            'Generation admission time missing.')
    return root, data


def java_escape(value):
    encoded = value.encode('utf-16-be')
    return ''.join('\\u%04x' % int.from_bytes(encoded[i:i + 2], 'big') for i in range(0, len(encoded), 2))


def signing_inputs(private, source, env):
    require(all(env.get(name) for name in SECRETS), 'Provision the existing Android signer and Firebase configuration in release.')
    key = base64.b64decode(env['ANDROID_KEYSTORE_BASE64'], validate=True)
    require(0 < len(key) <= 1024**2, 'Signing input exceeds bounds.')
    config = json.loads(env['ANDROID_GOOGLE_SERVICES_JSON'])
    encoded = json.dumps(config).encode()
    require(isinstance(config, dict) and len(encoded) <= 1024**2, 'Private Firebase configuration exceeds bounds.')
    write_new(private / 'signer.jks', key)
    values = dict(storeFile=str(private / 'signer.jks'), storePassword=env['ANDROID_STORE_PASSWORD'],
                  keyAlias=env['ANDROID_KEY_ALIAS'], keyPassword=env['ANDROID_KEY_PASSWORD'])
    write_new(private / 'signing.properties', ''.join(k + '=' + java_escape(v) + '\n' for k, v in values.items()).encode())
    write_new(source / 'mobile/android/app/google-services.json', encoded)


def source_members(members, prefix):
    require(0 < len(members) <= 10000 and sum(m.size for m in members) <= 512 * 1024**2, 'Source archive exceeds bounds.')
    seen = set()
    for member in members:
        name = PurePosixPath(member.name)
        require(bool(name.parts) and not name.is_absolute() and '..' not in name.parts and name.parts[0] == prefix and
                name.as_posix() == member.name.rstrip('/') and name.parts not in seen and
                (member.isfile() or member.isdir()), 'Unsafe or nonregular source archive member.')
        seen.add(name.parts)


def extract_source(archive, directory, version):
    with tarfile.open(archive, 'r:gz') as source:
        members = source.getmembers()
        source_members(members, 'zerus-' + version)
        source.extractall(directory, members=members, filter='data')
    return directory / ('zerus-' + version)


def command(argv, cwd, env, log, end):
    """Fixed Linux runner commands, one owned unreaped session, original end."""
    before(end)
    pending = log.parent / ('command-' + log.name + '.pending.json')
    complete = log.parent / ('command-' + log.name + '.complete.json')
    json_new(pending, dict(schema=1, state='pending', log=log.name))
    with log.open('xb') as output:
        child = subprocess.Popen(argv, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                 stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        failure, unknown_owner = None, False
        try:
            while os.waitid(os.P_PID, child.pid, os.WEXITED | os.WNOHANG | os.WNOWAIT) is None:
                before(end)
                require(output.tell() <= MAX_LOG, 'Private command log exceeded bounds.')
                time.sleep(0.1)
            before(end)
            require(output.tell() <= MAX_LOG, 'Private command log exceeded bounds.')
        except BaseException as error:
            failure = error
            unknown_owner = isinstance(error, ChildProcessError)
        # Keep the direct child unreaped through both group actions. An ECHILD
        # observation loses that protection: refuse signals, never guess a PID.
        cleanup_errors = ['unreaped-child-identity'] if unknown_owner else []
        if not unknown_owner:
            for name, sig in [('terminate', signal.SIGTERM), ('kill', signal.SIGKILL)]:
                try:
                    os.killpg(child.pid, sig)
                except ProcessLookupError:
                    pass
                except BaseException:
                    cleanup_errors.append(name)
                if name == 'terminate':
                    try:
                        time.sleep(0.1)
                    except BaseException:
                        cleanup_errors.append('termination-grace')
        try:
            child.wait(timeout=10)
        except BaseException:
            cleanup_errors.append('wait')
        if cleanup_errors:
            # Pending intent survives even if the UNKNOWN record cannot fsync.
            # No complete record means cleanup_private must retain this root.
            json_new(log.parent / 'PROCESS_CUSTODY_UNKNOWN', dict(schema=1, errors=cleanup_errors))
            raise ValueError('Owned command cleanup unknown; private root retained.') from failure
        json_new(complete, dict(schema=1, state='complete', pid=child.pid,
                                pending_sha256=hashlib.sha256(pending.read_bytes()).hexdigest()))
        if failure is not None:
            raise failure
        require(child.returncode == 0, 'Fixed candidate command failed; private output is not published.')
        before(end)


def file_identity(info):
    return (info.st_dev, info.st_ino, info.st_uid, info.st_nlink, info.st_mode,
            info.st_size, info.st_mtime_ns, info.st_ctime_ns)


def frozen_mapping(sealer, sealed_apk, mapping, snapshot, end):
    """Copy once privately; validate and hash the same unchanged snapshot."""
    before(end)
    with os.fdopen(os.open(mapping, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW), 'rb') as source:
        initial = os.fstat(source.fileno())
        require(stat.S_ISREG(initial.st_mode) and initial.st_uid == os.getuid() and initial.st_nlink == 1 and
                0 < initial.st_size <= 128 * 1024**2, 'Invalid build mapping source.')
        with os.fdopen(os.open(snapshot, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600), 'wb') as output:
            copied = 0
            for part in iter(lambda: source.read(1024**2), b''):
                before(end)
                copied += len(part)
                require(copied <= initial.st_size, 'Build mapping changed while freezing.')
                output.write(part)
            output.flush()
            os.fsync(output.fileno())
            os.fchmod(output.fileno(), 0o400)
        require(copied == initial.st_size and file_identity(initial) == file_identity(os.fstat(source.fileno())) and
                file_identity(initial) == file_identity(mapping.lstat()), 'Build mapping changed while freezing.')
    before(end)
    with os.fdopen(os.open(snapshot, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW), 'rb') as source:
        initial = os.fstat(source.fileno())
        require(stat.S_ISREG(initial.st_mode) and stat.S_IMODE(initial.st_mode) == 0o400 and
                initial.st_uid == os.getuid() and initial.st_nlink == 1, 'Frozen private mapping required.')
        def digest():
            source.seek(0)
            value = hashlib.sha256()
            for part in iter(lambda: source.read(1024**2), b''):
                before(end)
                value.update(part)
            before(end)
            return value.hexdigest()
        before_hash = digest()
        map_id = sealer.mapping_identity(sealed_apk, snapshot)
        after_hash = digest()
        require(before_hash == after_hash and file_identity(initial) == file_identity(os.fstat(source.fileno())) and
                file_identity(initial) == file_identity(snapshot.lstat()), 'Frozen mapping changed during APK identity observation.')
    before(end)
    return dict(mapping_sha256=before_hash, r8_map_id=map_id)


def completed_commands(private):
    for pending in private.glob('command-*.pending.json'):
        complete = pending.with_name(pending.name.replace('.pending.json', '.complete.json'))
        if not complete.is_file() or complete.is_symlink() or complete.stat().st_size > 8192:
            return False
        try:
            record = json.loads(complete.read_text())
            if (record.get('schema') != 1 or record.get('state') != 'complete' or
                    record.get('pending_sha256') != hashlib.sha256(pending.read_bytes()).hexdigest()):
                return False
        except (ValueError, OSError):
            return False
    return True


def mapping_upload(text):
    require('\x1b' not in text, 'Plain bounded Gradle task evidence required.')
    ordered = []
    for name in ('injectCrashlyticsMappingFileIdRelease', 'uploadCrashlyticsMappingFileRelease', 'assembleRelease'):
        lines = list(re.finditer(r'^> Task :app:' + name + r'([^\r\n]*)\r?$', text, re.M))
        require(len(lines) == 1 and lines[0][1] == '', 'Exact same-build mapping task must execute, without SKIPPED/UP-TO-DATE/cache suffix.')
        ordered.append(lines[0].start())
    success = list(re.finditer(r'^BUILD SUCCESSFUL in .+\r?$', text, re.M))
    require(len(success) == 1 and ordered == sorted(ordered) and ordered[-1] < success[0].start() and
            'BUILD FAILED' not in text, 'Successful exact-build Crashlytics upload evidence required.')
    return dict(inject='executed', upload='executed', assemble='executed', build='success')


def validate_apk(info, request):
    require(info.get('package_name') == 'app.zerus.mobile' and info.get('version_name') == request['version'] and
            type(info.get('version_code')) is int and info['version_code'] == request['version_code'] and
            info.get('signing_cert_sha256') == request['signing_cert_sha256'],
            'Sealed APK differs from committed stable identity or reviewed existing signer.')


def public_family(names, version):
    expected = {f'release/zerus-{version}.apk', f'release/zerus-{version}-source.tar.gz',
                'release/release-info.json', 'release/SHA256SUMS', 'candidate-provenance.json'}
    require(set(names) == expected and len(names) == len(expected), 'Only exact public Android candidate files may leave the job.')


def sha256(path, end):
    digest = hashlib.sha256()
    before(end)
    with path.open('rb') as stream:
        for part in iter(lambda: stream.read(1024**2), b''):
            before(end)
            digest.update(part)
    before(end)
    return digest.hexdigest()


def process_started(directory):
    """Boot-clock start seconds from the always readable stat record."""
    with (directory / 'stat').open('rb') as source:
        record = source.read(4097)
    require(len(record) <= 4096, 'Unknown over-bound process identity during cleanup.')
    fields = record.rsplit(b')', 1)[1].split()
    require(len(fields) > 19 and fields[19].isdigit(), 'Unknown process start during cleanup.')
    return int(fields[19]) / os.sysconf('SC_CLK_TCK')


def generation_survivors(private, end, proc=Path('/proc'), admitted=None):
    """Observe escaped same-UID generation processes; never signal by discovery.

    A same-UID process may refuse identity reads (a capability-holding user
    manager, for example). It cannot carry this generation's paths if it started
    before admission; one started later stays unknown and refuses cleanup.
    """
    needle = os.fsencode(private)
    survivors, count = [], 0
    with os.scandir(proc) as entries:
        for item in entries:
            before(end)
            count += 1
            require(count <= 8192, 'Process inventory exceeded finite cleanup bound.')
            if not item.name.isdecimal() or int(item.name) == os.getpid():
                continue
            directory = proc / item.name
            try:
                if directory.stat().st_uid != os.getuid():
                    continue
                values = []
                try:
                    for name in ('cmdline', 'environ'):
                        with (directory / name).open('rb') as source:
                            value = source.read(65537)
                        require(len(value) <= 65536, 'Unknown over-bound process identity during cleanup.')
                        values.append(value)
                    values.append(os.fsencode(os.readlink(directory / 'cwd')))
                except PermissionError:
                    require(admitted is not None and process_started(directory) < admitted,
                            'Uninspectable same-user process started during this generation; retain private root.')
                    continue
                if any(needle in value for value in values):
                    survivors.append(int(item.name))
            except (FileNotFoundError, ProcessLookupError):
                continue  # exited while being observed; no signal or identity inferred
            before(end)
    before(end)
    return survivors


def cleanup_private(root, data):
    before(data['end'])
    info = root.lstat()
    require(stat.S_ISDIR(info.st_mode) and info.st_dev == data['root_dev'] and info.st_ino == data['root_ino'] and
            info.st_uid == data['uid'] and stat.S_IMODE(info.st_mode) == 0o700, 'Changed private generation; cleanup refused.')
    private = root / 'private'
    if private.is_symlink():
        raise ValueError('Private cleanup root changed.')
    if private.exists():
        require(private.is_dir() and private.stat().st_uid == os.getuid(), 'Unknown private cleanup root.')
        require(not (private / 'PROCESS_CUSTODY_UNKNOWN').exists(), 'Unknown command cleanup; private root retained.')
        require(completed_commands(private), 'Unfinished command custody; private root retained.')
        require(not generation_survivors(private, data['end'], admitted=data['admitted']),
                'Generation process survives outside reaped commands; retain private root.')
        require(shutil.rmtree.avoids_symlink_attacks, 'Descriptor-based private cleanup required.')
        shutil.rmtree(private)  # fd-based no-follow rmtree on the fixed Ubuntu runner
    require(not private.exists() and not private.is_symlink(), 'Private cleanup incomplete; no artifact admitted.')
    before(data['end'])


def build(env):
    root, data = admission(env)
    request, end = data['request'], data['end']
    before(end - CLEANUP_SECONDS)
    source_check(request)
    json_new(root / 'build-started.json', dict(schema=1, request=request, original_end=end))
    private = root / 'private'
    private.mkdir(mode=0o700)
    candidate = root / 'staged-candidate'
    try:
        archive = private / f"zerus-{request['version']}-source.tar.gz"
        base_env = {name: env[name] for name in ('PATH', 'LANG', 'ANDROID_HOME', 'JAVA_HOME_17_X64') if name in env}
        base_env.update(HOME=str(private / 'home'), JAVA_HOME=env['JAVA_HOME_17_X64'],
                        TMPDIR=str(private / 'tmp'), GRADLE_USER_HOME=str(private / 'gradle'),
                        ZERUS_ANDROID_SIGNING_PROPERTIES=str(private / 'signing.properties'),
                        PYTHONDONTWRITEBYTECODE='1')
        base_env['PATH'] = str(Path(base_env['JAVA_HOME']) / 'bin') + os.pathsep + base_env['PATH']
        for name in ('home', 'tmp', 'gradle'):
            (private / name).mkdir(mode=0o700)
        command(['git', 'archive', '--format=tar.gz', f"--prefix=zerus-{request['version']}/",
                 '--output=' + str(archive), request['source_commit']], ROOT, base_env, private / 'archive.log', end - CLEANUP_SECONDS)
        source = extract_source(archive, private, request['version'])
        before(end - CLEANUP_SECONDS)
        signing_inputs(private, source, env)
        sdk = Path(env['ANDROID_HOME'])
        command([str(sdk / 'cmdline-tools/latest/bin/sdkmanager'), 'platforms;android-36', 'build-tools;36.0.0'],
                source, base_env, private / 'sdk.log', end - CLEANUP_SECONDS)
        command(['./gradlew', '--no-daemon', '--console=plain', '--max-workers=2', '--no-parallel',
                 '--no-build-cache', 'assembleRelease', 'lintRelease', '-PzerusFirebase=true',
                 '-Pkotlin.compiler.execution.strategy=in-process'],
                source / 'mobile/android', base_env, private / 'gradle.log', end - CLEANUP_SECONDS)
        tasks = mapping_upload((private / 'gradle.log').read_text())
        before(end - CLEANUP_SECONDS)
        spec = importlib.util.spec_from_file_location('android_stable_sealer', ROOT / 'scripts/prepare-mobile-release.py')
        sealer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(sealer)
        apk = private / f"zerus-{request['version']}.apk"
        shutil.copyfile(source / 'mobile/android/app/build/outputs/apk/release/app-release.apk', apk)
        mapping = source / 'mobile/android/app/build/outputs/mapping/release/mapping.txt'
        tools = sdk / 'build-tools/36.0.0'
        metadata_calls = []
        def apk_tool(argv):
            require(argv[0] in (str(tools / 'aapt2'), str(tools / 'apksigner')), 'Fixed APK tools required.')
            log = private / f'apk-tool-{len(metadata_calls)}.log'
            metadata_calls.append(log)
            command(argv, source, base_env, log, end - CLEANUP_SECONDS)
            require(log.stat().st_size <= 256 * 1024, 'APK metadata exceeds bounds.')
            return log.read_text()
        info = sealer.seal(apk, archive, candidate, request['source_commit'], 'stable', False,
                           tools / 'aapt2', tools / 'apksigner', run=apk_tool)
        validate_apk(info, request)
        frozen = frozen_mapping(sealer, candidate / apk.name, mapping, private / 'frozen-mapping.txt', end - CLEANUP_SECONDS)
        provenance = dict(schema=1, source_commit=request['source_commit'], channel='stable',
                          ci=data['ci'], candidate_run_id=request['run_id'], candidate_attempt=request['run_attempt'],
                          version=request['version'], version_code=request['version_code'],
                          signing_cert_sha256=request['signing_cert_sha256'], artifacts=info['artifacts'],
                          **frozen,
                          private_mapping_route='existing Firebase Crashlytics', mapping_upload=tasks,
                          gradle_log_sha256=sha256(private / 'gradle.log', end - CLEANUP_SECONDS),
                          release_info_sha256=sha256(candidate / 'release-info.json', end - CLEANUP_SECONDS),
                          release_sums_sha256=sha256(candidate / 'SHA256SUMS', end - CLEANUP_SECONDS))
        json_new(root / 'mapping-receipt.json', provenance)
        before(end - CLEANUP_SECONDS)
    finally:
        cleanup_private(root, data)
    before(end)
    provenance['private_cleanup'] = 'complete'
    # Preserve the v1 release's exact four-file family and SHA256SUMS. The
    # Actions-only provenance is beside it, never an Android release asset.
    public_family(['release/' + p.name for p in candidate.iterdir()] + ['candidate-provenance.json'], request['version'])
    require(all(p.is_file() and not p.is_symlink() for p in candidate.iterdir()), 'Public candidate must contain regular files only.')
    before(end)
    public = root / 'candidate'
    public.mkdir(mode=0o700)
    candidate.rename(public / 'release')
    json_new(public / 'candidate-provenance.json', provenance)
    before(end)
    print('Stable APK, exact source and nonsecret provenance sealed after private cleanup; nothing published.')


def cleanup(env):
    request = context(env)
    root = run_root(env, request)
    if not root.exists() and not root.is_symlink():
        return  # admission failed before allocation
    root, data = admission(env)
    cleanup_private(root, data)
    print('Private stable candidate inputs absent; no publication action.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase', choices=('admit', 'build', 'cleanup'))
    args = parser.parse_args()
    signal.signal(signal.SIGTERM, lambda signum, frame: sys.exit('Candidate interrupted.'))
    try:
        {'admit': admit, 'build': build, 'cleanup': cleanup}[args.phase](os.environ)
    except (ValueError, OSError, KeyError, json.JSONDecodeError, subprocess.SubprocessError, tarfile.TarError):
        print('Stable Android candidate refused or failed; no artifact/publication admitted.', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
