"""Selected-release admission controls: synthetic sealed bytes, no native process."""
import hashlib
import json
import os
from pathlib import Path
import tempfile
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
