"""Sealed main CI candidates and anonymous dev catalogs (no GitHub Releases)."""
from __future__ import annotations

import datetime as dt
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import tempfile
import zipfile
import urllib.parse
import zlib

from update_feed import (GitHub, HEX40, HEX64, ORIGIN, REPOSITORY, directory,
                         check_android_sequence, file_hash, fsync_directory, require, safe_name, timestamp)

WORKFLOW = '.github/workflows/ci.yml'
MAX_BYTES = 2 * 1024**3
MAX_FILE_BYTES = 512 * 1024**2
PLATFORMS = {'desktop', 'android'}


def identity(run, *, workflow=WORKFLOW, event='push'):
    """Check API evidence before using a candidate; never trust archive metadata alone."""
    require(isinstance(run, dict) and run.get('path') == workflow
            and run.get('event') == event and run.get('head_branch') == 'main'
            and run.get('conclusion') == 'success' and run.get('status') == 'completed'
            and run.get('head_repository', {}).get('full_name') == REPOSITORY,
            'Candidate is not a successful upstream main build')
    for key in ('id', 'run_number', 'run_attempt'):
        require(type(run.get(key)) is int and 0 < run[key] < 2**53, 'Invalid workflow identity')
    require(HEX40.fullmatch(run.get('head_sha', '')), 'Invalid workflow source identity')
    return run


def validate(info, root, run):
    require(isinstance(info, dict) and info.get('schemaVersion') == 2
            and type(info['schemaVersion']) is int and info.get('product') == 'zerus'
            and info.get('channel') == 'dev' and info.get('platform') in PLATFORMS,
            'Invalid dev candidate scope')
    require(info.get('channelTag') == 'dev', 'Wrong channel tag')
    require('sourceDirty' not in info, 'Dirty dev provenance is forbidden')
    platform = info['platform']
    require(info.get('buildId') == run['id'] and type(info.get('buildId')) is int
            and info.get('buildNumber') == run['run_number']
            and type(info.get('buildNumber')) is int
            and info.get('sourceCommit') == run['head_sha'], 'Candidate/run identity mismatch')
    require(info.get('buildUrl') == f'https://github.com/{REPOSITORY}/actions/runs/{run["id"]}',
            'Unexpected build provenance URL')
    timestamp(info.get('publishedAt'))
    version = info.get('versionName')
    require(isinstance(version, str) and re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+(?:-dev\.[0-9]+)?', version),
            'Invalid dev version')
    if '-dev.' in version:
        require(version.rsplit('-dev.', 1)[1] == str(info['buildNumber']), 'Dev version/build sequence mismatch')
    artifacts = info.get('artifacts')
    require(isinstance(artifacts, list) and 1 <= len(artifacts) <= 16, 'Invalid dev artifacts')
    names = set()
    total = 0
    for item in artifacts:
        require(isinstance(item, dict), 'Invalid artifact')
        name = safe_name(item.get('name'))
        require(name not in names, 'Duplicate artifact')
        names.add(name)
        size = item.get('size')
        require(type(size) is int and 0 < size <= MAX_FILE_BYTES
                and HEX64.fullmatch(item.get('sha256', '')), 'Invalid artifact seal')
        require(item.get('url') == f'{ORIGIN}/downloads/dev-{platform}-{run["id"]}/{name}',
                'Unexpected artifact URL')
        kind = item.get('kind')
        if kind == 'source':
            require(item.get('os') == 'any' and item.get('arch') == 'source'
                    and name.endswith('-source.tar.gz'), 'Invalid source archive')
        elif platform == 'android' and kind == 'apk':
            require(size <= 128*1024**2 and name.endswith('.apk') and item.get('os') == 'android'
                    and item.get('arch') == 'universal'
                    and item.get('packageName') == 'app.zerus.mobile'
                    and type(item.get('minSdk')) is int and 26 <= item['minSdk'] <= 100
                    and HEX64.fullmatch(item.get('signingCertSha256', '')), 'Invalid APK identity')
        elif platform == 'desktop' and kind == 'arch-package':
            require(name.endswith('.pkg.tar.zst') and item.get('os') == 'linux'
                    and item.get('arch') == 'x86_64', 'Invalid desktop package')
        else:
            raise ValueError('Unsupported dev artifact kind')
        path = root / name
        require(path.is_file() and not path.is_symlink() and path.stat().st_size == size
                and file_hash(path) == item['sha256'], 'Candidate bytes do not match seal')
        total += size
    require(total <= MAX_BYTES and sum(a['kind'] == 'source' for a in artifacts) == 1,
            'Candidate needs one exact source archive within budget')
    require(sum(a['kind'] == ('apk' if platform == 'android' else 'arch-package')
                for a in artifacts) == 1, 'Candidate lacks its platform binary')
    if platform == 'android':
        require(type(info.get('versionCode')) is int and 0 < info['versionCode'] <= 2_100_000_000,
                'Invalid Android version code')
    require({p.name for p in root.iterdir()} == names | {'dev-info.json'},
            'Candidate has unexpected files')
    return info


def seal(root, output, platform, run_id, number, commit, published, apk_identity=None):
    require(platform in PLATFORMS and HEX40.fullmatch(commit), 'Invalid dev source/platform')
    require(type(run_id) is int and run_id > 0 and type(number) is int and number > 0,
            'Invalid dev run')
    timestamp(published)
    require(not output.exists(), 'Candidate output already exists')
    artifacts = []
    if apk_identity:
        version = apk_identity['version_name']
        require(version.endswith(f'-dev.{number}'), 'APK Dev version/build sequence mismatch')
    else:
        base = (root / 'DEV_VERSION').read_text().strip()
        stable = (root / 'VERSION').read_text().strip()
        require(re.fullmatch(r'(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)', base)
                and tuple(map(int, base.split('.'))) > tuple(map(int, stable.split('.'))), 'Dev base must follow the stable release')
        version = f'{base}-dev.{number}'
        package = json.loads((root / 'desktop-build-info.json').read_text())
        embedded = package.get('desktop_build', {})
        require(package.get('source_commit') == commit and package.get('version') == stable
                and embedded.get('sourceCommit') == commit and embedded.get('sourceDirty') is False
                and embedded.get('devBuildNumber') == number and embedded.get('versionName') == version
                and embedded.get('version') == stable, 'Checked package Dev identity differs from the candidate')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.dev-candidate-', dir=output.parent) as temp:
        staging = Path(temp)
        for path in sorted(root.iterdir()):
            if path.name in {'VERSION', 'DEV_VERSION', 'desktop-build-info.json'}:
                continue
            name = safe_name(path.name)
            require(path.is_file() and not path.is_symlink(), 'Nonregular candidate file')
            kind = 'source' if name.endswith('-source.tar.gz') else 'apk' if name.endswith('.apk') else 'arch-package'
            item = {'kind': kind, 'name': name, 'size': path.stat().st_size, 'sha256': file_hash(path),
                    'url': f'{ORIGIN}/downloads/dev-{platform}-{run_id}/{name}',
                    'os': 'any' if kind == 'source' else 'android' if kind == 'apk' else 'linux',
                    'arch': 'source' if kind == 'source' else 'universal' if kind == 'apk' else 'x86_64'}
            if kind == 'apk':
                require(apk_identity is not None, 'APK identity must be verified before sealing')
                item.update(packageName=apk_identity['package_name'], minSdk=apk_identity['min_sdk'],
                            signingCertSha256=apk_identity['signing_cert_sha256'])
            shutil.copyfile(path, staging / name)
            artifacts.append(item)
        info = {'schemaVersion': 2, 'product': 'zerus', 'platform': platform, 'channel': 'dev',
                'versionName': version, 'buildId': run_id, 'buildNumber': number, 'channelTag': 'dev',
                'sourceCommit': commit, 'publishedAt': published,
                'buildUrl': f'https://github.com/{REPOSITORY}/actions/runs/{run_id}', 'artifacts': artifacts}
        if apk_identity:
            info['versionCode'] = apk_identity['version_code']
        (staging / 'dev-info.json').write_text(json.dumps(info, indent=2, sort_keys=True) + '\n')
        validate(info, staging, {'id': run_id, 'run_number': number, 'head_sha': commit})
        staging.rename(output)
    return info


def unpack(raw, destination):
    """Bounded data-only extraction. Never execute candidate code on the mirror."""
    if isinstance(raw, bytes):
        require(len(raw) <= MAX_BYTES, 'Artifact archive exceeds limit')
        raw = io.BytesIO(raw)
    try:
        with zipfile.ZipFile(raw) as archive:
            members = archive.infolist()
            require(1 <= len(members) <= 17, 'Unexpected artifact member count')
            names = set()
            total = 0
            for item in members:
                name = safe_name(item.filename)
                mode = (item.external_attr >> 16) & 0o170000
                # Actions archives store or deflate; never ask for passwords or other codecs.
                require(name not in names and mode in (0, 0o100000) and not item.is_dir()
                        and item.compress_type in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED)
                        and not item.flag_bits & 0x1
                        and 0 < item.file_size <= MAX_FILE_BYTES, 'Unsafe artifact archive member')
                names.add(name)
                total += item.file_size
                require(total <= MAX_BYTES, 'Expanded artifact archive exceeds limit')
                with archive.open(item) as source, (destination / name).open('xb') as output:
                    copied = 0
                    while block := source.read(65536):
                        copied += len(block)
                        require(copied <= item.file_size, 'Archive member exceeds seal')
                        output.write(block)
                    require(copied == item.file_size, 'Truncated artifact member')
    except (zipfile.BadZipFile, zlib.error, EOFError, NotImplementedError):
        # Truncated or corrupt compressed data, or zipfile's NotImplementedError
        # for a newer format version or flag, fails like any other unsafe archive.
        raise ValueError('Invalid artifact archive') from None


class DevGitHub(GitHub):
    def asset_origin(self, url):
        host = urllib.parse.urlsplit(url).hostname or ''
        if re.fullmatch(r'productionresultssa[0-9]+\.blob\.core\.windows\.net', host):
            from update_feed import https
            return https(url, {host})
        return super().asset_origin(url)

    def runs(self):
        return self.api(f'/repos/{REPOSITORY}/actions/workflows/ci.yml/runs?branch=main&event=push&status=success&per_page=20')['workflow_runs']

    def candidate(self, run, platform, destination):
        rows = self.api(f'/repos/{REPOSITORY}/actions/runs/{run["id"]}/artifacts?per_page=100')
        require(type(rows.get('total_count')) is int and rows['total_count'] <= 100,
                'Unexpected run artifact catalog')
        # Failed-job retries can retain an earlier successful platform job.
        # Names include the attempt so upload-artifact never replaces a seal.
        attempts = []
        pattern = re.compile(r'zerus-dev-' + re.escape(platform) + r'-a([1-9][0-9]*)')
        for artifact in rows['artifacts']:
            match = pattern.fullmatch(artifact.get('name', ''))
            if match:
                attempt = int(match[1])
                require(attempt <= run['run_attempt'], 'Artifact belongs to a future attempt')
                attempts.append((attempt, artifact))
        require(attempts, 'Missing dev artifact')
        latest = max(attempt for attempt, _ in attempts)
        selected = [artifact for attempt, artifact in attempts if attempt == latest]
        require(len(selected) == 1, 'Missing/ambiguous dev artifact')
        artifact = selected[0]
        require(artifact.get('expired') is False and type(artifact.get('id')) is int
                and 0 < artifact['id'] < 2**53 and type(artifact.get('size_in_bytes')) is int
                and 0 < artifact['size_in_bytes'] <= MAX_BYTES, 'Invalid/expired dev artifact')
        evidence = artifact.get('workflow_run', {})
        require(evidence.get('id') == run['id'] and evidence.get('head_sha') == run['head_sha']
                and evidence.get('head_branch') == 'main', 'Artifact belongs to another run')
        url = f'/repos/{REPOSITORY}/actions/artifacts/{artifact["id"]}/zip'
        digest = hashlib.sha256()
        with tempfile.TemporaryFile() as archive:
            for block in self.stream('https://api.github.com' + url, MAX_BYTES,
                                     api=True, download=True):
                digest.update(block)
                archive.write(block)
            require(artifact.get('digest') == 'sha256:' + digest.hexdigest(),
                    'GitHub artifact digest mismatch')
            archive.seek(0)
            unpack(archive, destination)


def check_previous(path, info):
    require(not path.is_symlink(), 'Symlinked dev catalog')
    if not path.exists():
        return
    require(path.stat().st_size <= 256 * 1024, 'Existing catalog exceeds limit')
    old = json.loads(path.read_text())
    require(old['schemaVersion'] == 2 and old['platform'] == info['platform']
            and old['channel'] == 'dev', 'Previous catalog scope mismatch')
    if old['buildId'] == info['buildId']:
        require(old == info, 'Immutable candidate changed')
    else:
        require(info['buildNumber'] > old['buildNumber'], 'Refusing dev build rollback')
        if info['platform'] == 'android':
            require(info['versionCode'] > old['versionCode'], 'Refusing APK downgrade/same-code replacement')


def scoped_path(root, *parts):
    """Check every owned ancestor before reading, retiring or publishing files."""
    path = root
    require(not path.is_symlink() and (not path.exists() or path.is_dir()), 'Unsafe dev mirror root')
    for index, part in enumerate(parts):
        path = path / part
        require(not path.is_symlink(), 'Symlinked dev mirror path')
        if index < len(parts) - 1:
            require(not path.exists() or path.is_dir(), 'Non-directory dev mirror ancestor')
    return path


def atomic_json(path, value):
    require(not path.is_symlink(), 'Symlinked dev state')
    data = (json.dumps(value, indent=2, sort_keys=True) + '\n').encode()
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as output:
            temporary = Path(output.name)
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        temporary.chmod(0o644)
        os.replace(temporary, path)
        fsync_directory(path.parent)
    finally:
        if temporary:
            temporary.unlink(missing_ok=True)


def publish(root, info, staging):
    feed = directory(root, 'updates', 'v2', info['platform']) / 'dev.json'
    check_previous(feed, info)
    check_android_sequence(root, info, feed)
    downloads = directory(root, 'downloads')
    target = downloads / f'dev-{info["platform"]}-{info["buildId"]}'
    require(not target.is_symlink(), 'Symlinked dev build')
    if target.exists():
        for source in staging.iterdir():
            require(file_hash(target / source.name) == file_hash(source), 'Immutable dev asset collision')
    else:
        with tempfile.TemporaryDirectory(prefix='.dev-publish-', dir=downloads) as temp:
            prepared = Path(temp)
            prepared.chmod(0o755)
            for source in staging.iterdir():
                destination = prepared / source.name
                with source.open('rb') as content, destination.open('xb') as output:
                    shutil.copyfileobj(content, output)
                    output.flush()
                    os.fsync(output.fileno())
                destination.chmod(0o644)
            fsync_directory(prepared)
            prepared.rename(target)
        fsync_directory(downloads)
    atomic_json(feed, info)


def collect(root, *, keep=3, grace_seconds=72*3600, now=None):
    """Retire only dev-owned directories; grace starts after they leave the feed."""
    require(keep >= 1 and grace_seconds >= 0, 'Invalid dev retention')
    now = now or dt.datetime.now(dt.timezone.utc)
    downloads = scoped_path(root, 'downloads')
    require(not downloads.exists() or downloads.is_dir(), 'Unsafe dev downloads directory')
    for platform in sorted(PLATFORMS):
        feed = scoped_path(root, 'updates', 'v2', platform, 'dev.json')
        protected = json.loads(feed.read_text())['buildId'] if feed.exists() else None
        entries = []
        for path in downloads.glob(f'dev-{platform}-*'):
            require(not path.is_symlink() and path.is_dir(), 'Unsafe dev retention directory')
            marker = path / 'dev-info.json'
            require(marker.is_file() and not marker.is_symlink() and marker.stat().st_size <= 256*1024,
                    'Missing dev ownership marker')
            info = json.loads(marker.read_text())
            require(info.get('platform') == platform and info.get('channel') == 'dev'
                    and info.get('schemaVersion') == 2 and type(info.get('buildNumber')) is int
                    and type(info.get('buildId')) is int
                    and path.name == f'dev-{platform}-{info["buildId"]}', 'Invalid dev retention identity')
            require(not any(p.is_symlink() or not p.is_file() for p in path.iterdir()),
                    'Unsafe retained dev content')
            entries.append((info['buildNumber'], info['buildId'], path))
        entries.sort(reverse=True)
        for index, (_, build_id, path) in enumerate(entries):
            retired = path / '.retired.json'
            if index < keep or build_id == protected:
                retired.unlink(missing_ok=True)
                continue
            if not retired.exists():
                atomic_json(retired, {'retiredAt': now.isoformat()})
            else:
                require(retired.stat().st_size <= 1024, 'Invalid retirement marker')
                since = timestamp(json.loads(retired.read_text())['retiredAt'])
                if (now - since).total_seconds() >= grace_seconds:
                    shutil.rmtree(path)
