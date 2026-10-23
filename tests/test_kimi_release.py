"""Selected-release admission controls: synthetic sealed bytes, no native process."""
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import stat
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

import kimi_release as release


class SelectedRelease(unittest.TestCase):
    def fixture(self):
        root = Path(self.enterContext(tempfile.TemporaryDirectory(prefix='kimi-release-control-')))
        root.chmod(0o700)
        image = root / 'kimi-code-linux-x64'
        image.write_bytes(b'synthetic image')
        sha = hashlib.sha256(image.read_bytes()).hexdigest()
        manifest = dict(version='2.3.1', tag='@moonshot-ai/kimi-code@2.3.1',
                        platforms={'linux-x64': dict(filename=image.name, checksum=sha)})
        (root / 'manifest.json').write_text(json.dumps(manifest))
        record = dict(schema=1, version='2.3.1', target='linux-x64', image_size=image.stat().st_size,
                      image_sha256=sha, manifest_sha256=hashlib.sha256((root / 'manifest.json').read_bytes()).hexdigest())
        self.save(root / 'selected-release.json', record)
        for path in root.iterdir():
            path.chmod(0o400)
        return root, image, sha, record

    def save(self, path, value):
        if path.exists():
            path.chmod(0o600)
        path.write_text(json.dumps(value))
        path.chmod(0o400)

    def admit(self, root, image, sha):
        return release.admitted_version(image, sha, root / 'selected-release.json', target='linux-x64')

    def test_exact_sealed_manifest_and_image_admit_the_selected_version(self):
        root, image, sha, _ = self.fixture()
        self.assertEqual(self.admit(root, image, sha), '2.3.1')

    def test_unknown_or_mismatched_selection_never_uses_historical_pins(self):
        for key, value in (('schema', True), ('schema', 2), ('version', '../2.2.0'),
                           ('version', '2.3.1-beta'), ('target', 'win32-x64'), ('image_size', True),
                           ('image_size', release.IMAGE_LIMIT + 1), ('image_size', 1),
                           ('image_sha256', '0' * 64), ('manifest_sha256', '0' * 64), ('extra', 1)):
            with self.subTest(key=key, value=value):
                root, image, sha, record = self.fixture()
                self.save(root / 'selected-release.json', dict(record, **{key: value}))
                with self.assertRaises(ValueError):
                    self.admit(root, image, sha)
        root, image, sha, _ = self.fixture()
        with mock.patch.dict(os.environ, {}, clear=True), self.assertRaises(ValueError):
            release.admitted_version(image, sha)
        with self.assertRaises(ValueError):
            self.admit(root, image, '0' * 64)

    def test_manifest_version_target_filename_and_checksum_are_bound(self):
        for key, value in (('version', '2.2.0'), ('tag', '@other/kimi@2.3.1'),
                           ('filename', '../image'), ('checksum', '0' * 64)):
            with self.subTest(key=key):
                root, image, sha, record = self.fixture()
                path = root / 'manifest.json'
                manifest = json.loads(path.read_text())
                if key in ('version', 'tag'):
                    manifest[key] = value
                else:
                    manifest['platforms']['linux-x64'][key] = value
                self.save(path, manifest)
                record['manifest_sha256'] = hashlib.sha256(path.read_bytes()).hexdigest()
                self.save(root / 'selected-release.json', record)
                with self.assertRaises(ValueError):
                    self.admit(root, image, sha)

    def test_changed_image_or_different_path_never_admits(self):
        root, image, sha, _ = self.fixture()
        other = root / 'other'
        other.write_bytes(image.read_bytes()); other.chmod(0o400)
        with self.assertRaises(ValueError):
            self.admit(root, other, sha)
        image.chmod(0o600); image.write_bytes(b'changed bytes!!'); image.chmod(0o400)
        with self.assertRaises(ValueError):
            self.admit(root, image, sha)

    @contextmanager
    def windows(self, overrides=None, after=False, path_change=None, fd_change=None, same_time=False):
        original = Path.lstat
        counts, fd_names = {}, {}
        def project(info, name, descriptor):
            values = {key: getattr(info, key) for key in dir(info) if key.startswith('st_')}
            route = (name, descriptor)
            counts[route] = counts.get(route, 0) + 1
            values['st_birthtime_ns'] = 100
            values['st_ctime_ns'] = 200 if descriptor and not same_time else 100
            if stat.S_ISREG(info.st_mode):
                values['st_mode'] = stat.S_IFREG | 0o444
                if not descriptor and Path(name).suffix.lower() in ('.exe', '.bat', '.cmd', '.com'):
                    values['st_mode'] |= 0o111
            attributes = stat.FILE_ATTRIBUTE_READONLY
            if not descriptor and overrides and name in overrides and (not after or counts[route] > 1):
                attributes = overrides[name]
            if attributes is not None:
                values['st_file_attributes'] = attributes
            else:
                values.pop('st_file_attributes', None)
            change = fd_change if descriptor else path_change
            if change and counts[route] > 1:
                values.update(change)
            return SimpleNamespace(**values)
        def observed(path, *args, **kwargs):
            return project(original(path, *args, **kwargs), path.name, False)
        def opened(path, *args):
            fd = os.open(path, *args)
            fd_names[fd] = Path(path).name
            return fd
        def descriptor(fd):
            return project(os.fstat(fd), fd_names[fd], True)
        # Only the subject's OS surface is NT; pathlib and the host remain Unix.
        with mock.patch.object(release, 'os', SimpleNamespace(name='nt', open=opened,
                                           fdopen=os.fdopen, fstat=descriptor, O_RDONLY=os.O_RDONLY)), \
                mock.patch.object(Path, 'lstat', observed):
            yield

    def test_windows_exact_readonly_attributes_admit_the_sealed_selection(self):
        root, image, sha, _ = self.fixture()
        with self.windows():
            self.assertEqual(self.admit(root, image, sha), '2.3.1')

    def test_windows_writable_or_missing_attribute_refuses_each_sealed_file(self):
        for name in ('selected-release.json', 'manifest.json', 'kimi-code-linux-x64'):
            for value in (0, None, True):
                with self.subTest(name=name, value=value):
                    root, image, sha, _ = self.fixture()
                    with self.windows({name: value}), self.assertRaisesRegex(ValueError, 'Windows read-only'):
                        self.admit(root, image, sha)

    def test_windows_readonly_removed_during_read_is_refused(self):
        root, image, sha, _ = self.fixture()
        with self.windows({image.name: 0}, after=True), self.assertRaisesRegex(ValueError, 'Windows read-only'):
            self.admit(root, image, sha)

    def test_windows_cross_route_creation_change_times_and_executable_bits_are_comparable(self):
        for name, same_time in (('fixture.data', False), ('fixture.exe', True)):
            with self.subTest(name=name, same_creation_change_time=same_time):
                root, _, _, _ = self.fixture()
                image = root / name
                image.write_bytes(b'not executable; synthetic data'); image.chmod(0o400)
                with self.windows(same_time=same_time):
                    try:
                        digest, content = release.sealed(image, 65536)
                    except ValueError as error:
                        self.fail('Unchanged Windows file must retain comparable identity: ' + str(error))
                self.assertEqual((digest, content), (hashlib.sha256(image.read_bytes()).hexdigest(), image.read_bytes()))

    def test_windows_changes_to_either_full_route_are_still_refused(self):
        for route in ('path_change', 'fd_change'):
            for field, value in (('st_ctime_ns', 999), ('st_ino', 0), ('st_dev', -1),
                                 ('st_mode', stat.S_IFDIR | 0o444), ('st_mode', stat.S_IFREG | 0o666),
                                 ('st_mode', stat.S_IFREG | 0o555),
                                 ('st_size', -1), ('st_nlink', 99), ('st_mtime_ns', -1),
                                 ('st_birthtime_ns', 999)):
                with self.subTest(route=route, field=field, value=value):
                    root, image, sha, _ = self.fixture()
                    with self.windows(**{route: {field: value}}), self.assertRaises(ValueError):
                        self.admit(root, image, sha)

    def test_windows_unknown_cross_route_identity_is_not_equal_proof(self):
        for field in ('st_dev', 'st_ino', 'st_mode', 'st_size', 'st_nlink', 'st_mtime_ns', 'st_birthtime_ns'):
            values = dict(st_dev=1, st_ino=2, st_mode=stat.S_IFREG | 0o444, st_size=1,
                          st_nlink=1, st_mtime_ns=20, st_birthtime_ns=10)
            values[field] = None
            with self.subTest(field=field), self.windows(), self.assertRaises(ValueError):
                release.comparable_identity(SimpleNamespace(**values))

    def test_windows_paired_birthtime_changes_cannot_keep_a_false_cross_route_match(self):
        root, image, sha, _ = self.fixture()
        with self.windows(path_change={'st_birthtime_ns': 999}, fd_change={'st_birthtime_ns': 999}), \
                self.assertRaises(ValueError):
            self.admit(root, image, sha)

    @unittest.skipIf(os.name == 'nt', 'Unix file modes and symbolic links')
    def test_private_modes_links_and_owner_are_required(self):
        for name in ('manifest.json', 'selected-release.json', 'kimi-code-linux-x64'):
            with self.subTest(name=name):
                root, image, sha, _ = self.fixture()
                path = root / name
                path.chmod(0o600)
                with self.assertRaises(ValueError):
                    self.admit(root, image, sha)
                path.chmod(0o400)
                os.link(path, root / 'hardlink')
                with self.assertRaises(ValueError):
                    self.admit(root, image, sha)
        root, image, sha, _ = self.fixture()
        original = root / 'manifest.json'
        original.rename(root / 'real-manifest')
        original.symlink_to(root / 'real-manifest')
        with self.assertRaises(ValueError):
            self.admit(root, image, sha)
        with mock.patch.object(release.os, 'getuid', return_value=os.getuid() + 1), self.assertRaises(ValueError):
            self.admit(root, image, sha)
