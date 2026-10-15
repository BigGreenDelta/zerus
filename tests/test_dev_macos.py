"""Mac Dev candidates stay sealed through the mirror and the Homebrew publisher."""
import copy
import importlib.util
import json
import os
from pathlib import Path
import shutil
import tempfile
import unittest

from test_dev_updates import COMMIT, NOW, FakeGitHub, candidate, dev, publisher, run
from homebrew_tap_fixture import make_tap_remote, remote_file
import macos_release as mac

spec = importlib.util.spec_from_file_location('dev_cask', Path(dev.__file__).with_name('publish-dev-cask.py'))
cask = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cask)


def signed(base, version='0.39.1-dev.1'):
    root = base / 'signed'; root.mkdir()
    info = {'asset': mac.macos_asset(version), 'qt_version': '6.11.2', 'team_id': 'ABCDE12345',
            'notarization_id': '0123abcd-4567-89ef-0123-456789abcdef', 'minimum_system_version': '14.0',
            'dmg': {'asset': mac.macos_dmg(version), 'notarization_id': '1123abcd-4567-89ef-0123-456789abcdef'}}
    for name in mac.macos_assets(info): (root / name).write_bytes(('Notarized fixture ' + name).encode())
    (root / 'macos-info.json').write_text(json.dumps(info))
    return root


class Public:
    def __init__(self, info, root):
        self.info, self.root, self.corrupt = info, root, False
    def stream(self, url, limit, **kwargs):
        if url.endswith('/dev.json'): yield json.dumps(self.info).encode()
        else: yield b'changed' if self.corrupt else (self.root / url.rsplit('/', 1)[1]).read_bytes()


class MacDevTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.client = FakeGitHub(self.root)
        self.client.api = lambda path: self.client.run
        self.signed = signed(self.root)
        self.sealed = self.root / 'mac-candidate'
        self.info = self.seal(self.sealed)
        self.client.info, self.client.path = self.info, self.sealed
    def seal(self, output):
        return dev.seal(self.root / 'input-desktop-1', output, 'desktop', 101, 1, COMMIT, NOW.isoformat(), macos=self.signed)
    def verify(self, public=None):
        with tempfile.TemporaryDirectory(dir=self.root) as destination:
            return cask.verified_candidate(self.client, public or Public(self.info, self.sealed), 101, Path(destination))
    def test_additive_mac_downloads_preserve_legacy_artifact_kinds_and_all_bytes(self):
        self.assertEqual({a['kind'] for a in self.info['artifacts']}, {'source', 'arch-package'})
        mirror = self.root / 'mirror'; mirror.mkdir()
        publisher.synchronize(self.client, mirror, ['desktop'], True)
        stored = mirror / 'downloads/dev-desktop-101'
        self.assertEqual(json.loads((mirror / 'updates/v2/desktop/dev.json').read_text()), self.info)
        for row in self.info['macos']['downloads']:
            self.assertEqual((stored / row['name']).read_bytes(), (self.signed / row['name']).read_bytes())
        publisher.synchronize(self.client, mirror, ['desktop'], True)
        (stored / self.info['macos']['asset']).write_bytes(b'changed')
        with self.assertRaises(ValueError): publisher.synchronize(self.client, mirror, ['desktop'], True)
    def test_sealing_rejects_missing_images_symlinks_and_foreign_versions_atomically(self):
        image = self.signed / self.info['macos']['dmg']['asset']; data = image.read_bytes(); image.unlink()
        with self.assertRaises(ValueError): self.seal(self.root / 'bad')
        self.assertFalse((self.root / 'bad').exists())
        target = self.root / 'outside'; target.write_bytes(data); image.symlink_to(target)
        with self.assertRaises(ValueError): self.seal(self.root / 'bad')
        image.unlink(); image.write_bytes(data)
        meta = json.loads((self.signed / 'macos-info.json').read_text()); meta['asset'] = mac.macos_asset('0.39.1-dev.2')
        (self.signed / 'macos-info.json').write_text(json.dumps(meta))
        with self.assertRaises(ValueError): self.seal(self.root / 'bad')
    def test_mac_seals_reject_other_origins_unsealed_bytes_and_extra_metadata(self):
        for key, wrong in [('url', 'https://example.invalid/app.zip'), ('size', 0), ('sha256', '0'*64), ('name', '../app.zip')]:
            info = copy.deepcopy(self.info); info['macos']['downloads'][0][key] = wrong
            with self.subTest(key=key), self.assertRaises(ValueError): dev.validate(info, self.sealed, run())
        for key, wrong in [('team_id', 'foreign'), ('extra', 'not reviewed'), ('notarization_id', '')]:
            info = copy.deepcopy(self.info); info['macos'][key] = wrong
            with self.subTest(key=key), self.assertRaises(ValueError): dev.validate(info, self.sealed, run())
    def test_tap_publication_requires_the_exact_public_seal_and_bytes(self):
        self.assertEqual(self.verify(), self.info)
        public = Public(copy.deepcopy(self.info), self.sealed); public.info['sourceCommit'] = 'b'*40
        with self.assertRaises(ValueError): self.verify(public)
        public = Public(self.info, self.sealed); public.corrupt = True
        with self.assertRaises(ValueError): self.verify(public)
        for key, value in [('event','pull_request'), ('conclusion','failure'), ('head_branch','feature')]:
            original = self.client.run; self.client.run = {**original,key:value}
            with self.subTest(key=key), self.assertRaises(ValueError): self.verify()
            self.client.run = original
    def test_superseded_and_legacy_candidates_leave_the_tap_unchanged(self):
        public = Public({**self.info,'buildNumber':2}, self.sealed)
        self.assertIsNone(self.verify(public))
        legacy = self.root / 'candidate-desktop-1'
        self.client.info = json.loads((legacy / 'dev-info.json').read_text()); self.client.path = legacy
        self.assertIsNone(self.verify())
    def test_dev_template_uses_exact_mirror_build_and_never_executes_install_hooks(self):
        text = mac.render_cask('0.39.1-dev.1', 'a'*64, 'dev', 'dev-desktop-101')
        self.assertIn('https://zerus.dev/downloads/dev-desktop-101/zerus-#{version}-macos-arm64.zip', text)
        self.assertIn('conflicts_with cask: ["zerus", "zerus@nightly"]', text)
        for forbidden in ['postflight', 'preflight', 'system_command', 'uninstall quit:', 'uninstall launchctl:']: self.assertNotIn(forbidden, text)
        for tag in ['dev-desktop-0', '../dev', 'dev-desktop-101?replace']:
            with self.assertRaises(ValueError): mac.render_cask('0.39.1-dev.1', 'a'*64, 'dev', tag)
        self.assertGreater(mac.cask_version_key('0.39.1-dev.10','dev'), mac.cask_version_key('9.9.9-dev.9','dev'))
    def test_tap_updates_only_dev_and_rejects_a_changed_same_version(self):
        stable = mac.render_cask('0.39.0','c'*64)
        nightly = mac.render_cask('0.39.0.r1.gaaaaaaa.n1','d'*64,'nightly','nightly-99')
        remote = make_tap_remote(self.root, {'Casks/zerus.rb':stable,'Casks/zerus@nightly.rb':nightly})
        (self.sealed / 'homebrew').mkdir()
        recipe = self.sealed / 'homebrew/zerus@dev.rb'
        recipe.write_text(mac.render_cask('0.39.1-dev.1','a'*64,'dev','dev-desktop-101'))
        def prepare():
            workspace = Path(tempfile.mkdtemp(dir=self.root))
            return cask.publisher.prepare_tap(self.sealed, workspace, os.environ.copy(), '0.39.1-dev.1', 'dev', str(remote))
        plan = prepare(); cask.publisher.git(plan,'push','origin','HEAD:main')
        self.assertEqual(remote_file(remote,'Casks/zerus@dev.rb'), recipe.read_text())
        self.assertEqual(remote_file(remote,'Casks/zerus.rb'),stable)
        self.assertEqual(remote_file(remote,'Casks/zerus@nightly.rb'),nightly)
        self.assertIsNone(prepare())
        recipe.write_text(mac.render_cask('0.39.1-dev.1','b'*64,'dev','dev-desktop-101'))
        with self.assertRaises(ValueError): prepare()


if __name__ == '__main__': unittest.main()
