"""Data-only admission for one sealed official Kimi Code release in hosted CI.

This record selects fixture bytes; it grants no installation or process authority.
Native fixtures still prove their private copies, environments and processes.
"""
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import stat

IMAGE_LIMIT = 256 * 1024**2
VERSION = re.compile(r'(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})')
SHA256 = re.compile(r'[0-9a-f]{64}')


def distinct(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError('duplicate release key')
        value[key] = item
    return value


def host_target():
    pair = (platform.system(), platform.machine().lower())
    target = {('Linux', 'x86_64'): 'linux-x64', ('Darwin', 'arm64'): 'darwin-arm64',
              ('Windows', 'amd64'): 'win32-x64'}.get(pair)
    if target is None:
        raise ValueError('unreviewed Kimi fixture architecture')
    return target


def identity(info):
    return (info.st_dev, info.st_ino, info.st_mode, info.st_size, info.st_nlink,
            info.st_mtime_ns, info.st_ctime_ns)


def comparable_identity(info):
    if os.name != 'nt':
        return identity(info)
    # CPython 3.14.8: path stat's ctime is CreationTime, descriptor
    # stat's ctime is ChangeTime. Only path stat adds filename-derived 0111.
    # Full same-route snapshots still compare those raw fields separately.
    values = tuple(getattr(info, key, None) for key in
                   ('st_dev', 'st_ino', 'st_mode', 'st_size', 'st_nlink', 'st_mtime_ns', 'st_birthtime_ns'))
    if any(type(value) is not int for value in values) or values[1] <= 0:
        raise ValueError('Windows release file identity evidence is unavailable')
    device, inode, mode, size, links, modified, created = values
    return (device, inode, stat.S_IFMT(mode), stat.S_IMODE(mode) & ~0o111,
            size, links, modified, created)


def readonly_file(info):
    if os.name == 'nt':
        attributes = getattr(info, 'st_file_attributes', None)
        readonly = getattr(stat, 'FILE_ATTRIBUTE_READONLY', None)
        if (type(attributes) is not int or type(readonly) is not int or readonly <= 0
                or attributes & readonly == 0):
            raise ValueError('release file Windows read-only attribute is unavailable or unset')
    elif info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) != 0o400:
        raise ValueError('release file must be private and sealed read-only')


def sealed(path, limit, expected_size=None):
    path = Path(path)
    before = path.lstat()
    if (not stat.S_ISREG(before.st_mode) or before.st_nlink != 1 or before.st_size > limit
            or expected_size is not None and before.st_size != expected_size):
        raise ValueError('release file kind, links or size changed')
    readonly_file(before)
    fd = os.open(path, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0) | getattr(os, 'O_NONBLOCK', 0))
    with os.fdopen(fd, 'rb') as stream:
        opened = os.fstat(stream.fileno())
        readonly_file(opened)
        if comparable_identity(opened) != comparable_identity(before):
            raise ValueError('release file changed before reading')
        checksum, data, count = hashlib.sha256(), [], 0
        while count < before.st_size:
            chunk = stream.read(min(65536, before.st_size - count))
            if not chunk:
                raise ValueError('short release file')
            count += len(chunk)
            checksum.update(chunk)
            if limit <= 65536:
                data.append(chunk)
        after = path.lstat()
        readonly_file(after)
        final = os.fstat(stream.fileno())
        readonly_file(final)
        if (identity(final) != identity(opened) or identity(after) != identity(before)
                or comparable_identity(final) != comparable_identity(opened)
                or comparable_identity(final) != comparable_identity(after)):
            raise ValueError('release file changed while reading')
    return checksum.hexdigest(), b''.join(data)


def admitted_version(image, expected_sha, record_path=None, target=None):
    """Verify the runner's immutable selection, exact manifest and original image."""
    record_path = record_path or os.environ.get('HGS_KIMI_TEST_RECORD')
    if not record_path:
        raise ValueError('a sealed selected Kimi release record is required')
    record_path = Path(record_path)
    parent = record_path.parent.lstat()
    if not stat.S_ISDIR(parent.st_mode):
        raise ValueError('release directory is not a directory')
    if os.name != 'nt' and (parent.st_uid != os.getuid() or stat.S_IMODE(parent.st_mode) != 0o700):
        raise ValueError('release directory must be private')
    _, raw = sealed(record_path, 65536)
    record = json.loads(raw, object_pairs_hook=distinct)
    if (not isinstance(record, dict)
            or set(record) != {'schema', 'version', 'target', 'manifest_sha256', 'image_sha256', 'image_size'}
            or type(record['schema']) is not int or record['schema'] != 1
            or not isinstance(record['version'], str) or VERSION.fullmatch(record['version']) is None
            or record['target'] != (target or host_target())
            or type(record['image_size']) is not int or not 0 < record['image_size'] <= IMAGE_LIMIT
            or any(not isinstance(record[key], str) or SHA256.fullmatch(record[key]) is None
                   for key in ('manifest_sha256', 'image_sha256'))):
        raise ValueError('invalid selected Kimi release record')
    manifest_sha, raw = sealed(record_path.parent / 'manifest.json', 65536)
    manifest = json.loads(raw, object_pairs_hook=distinct)
    filename = 'kimi-code-' + record['target'] + ('.exe' if record['target'].startswith('win32-') else '')
    entry = manifest['platforms'][record['target']]
    if (manifest_sha != record['manifest_sha256'] or manifest['version'] != record['version']
            or manifest['tag'] != '@moonshot-ai/kimi-code@' + record['version']
            or entry['filename'] != filename or entry['checksum'] != record['image_sha256']
            or expected_sha != record['image_sha256']
            or Path(image).absolute() != (record_path.parent / filename).absolute()):
        raise ValueError('selected Kimi manifest/image identity differs')
    observed, _ = sealed(image, IMAGE_LIMIT, record['image_size'])
    if observed != record['image_sha256']:
        raise ValueError('selected Kimi image hash differs')
    return record['version']
