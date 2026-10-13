"""The pinned Qt used for the macOS bundle must come from verified official archives."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("install_qt_macos", ROOT / "scripts/install-qt-macos.py")
qt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qt)

UPDATES = """<Updates>
 <PackageUpdate><Name>qt.qt6.6113.clang_64</Name><Version>6.11.3-0-202609240332</Version>
  <DownloadableArchives>qtbase-MacOS-X.7z, qtdeclarative-MacOS-X.7z, qtdoc-MacOS-X.7z</DownloadableArchives></PackageUpdate>
 <PackageUpdate><Name>qt.qt6.6113.clang_64.debug</Name><Version>6.11.3-0-1</Version>
  <DownloadableArchives>qtbase-debug.7z</DownloadableArchives></PackageUpdate>
</Updates>"""


class PinnedQt(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="zerus-qt-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.pin = json.loads((ROOT / "packaging/macos/qt.json").read_text())

    def test_repository_pin_is_valid_and_complete(self):
        qt.validate_pin(self.pin)
        self.assertEqual(self.pin["qt_version"], "6.11.2")
        names = " ".join(archive["path"] for archive in self.pin["archives"])
        for module in ("qtbase", "qtsvg", "qtmultimedia"):
            self.assertIn(module + "-", names)
        # Qt WebEngine and the modules only it needs are not part of the bundle.
        for module in ("qtdeclarative", "qtwebchannel", "qtpositioning", "qtwebengine"):
            self.assertNotIn(module + "-", names)

    def test_pin_rejects_foreign_sources_and_malformed_entries(self):
        changes = [
            lambda pin: pin.update(repository="https://mirror.example/qt"),
            lambda pin: pin["archives"][0].update(sha256="0" * 63),
            lambda pin: pin["archives"][0].update(path="../outside.7z"),
            lambda pin: pin["archives"][0].update(path="/absolute.7z"),
            lambda pin: pin["archives"][0].update(size=0),
            lambda pin: pin["archives"].append(dict(pin["archives"][0])),
            lambda pin: pin.update(unexpected=True),
        ]
        for change in changes:
            pin = copy.deepcopy(self.pin)
            change(pin)
            with self.assertRaises(ValueError):
                qt.validate_pin(pin)

    def test_updates_metadata_selects_exact_archives(self):
        self.assertEqual(qt.archives_from_updates(UPDATES, "qt.qt6.6113.clang_64", ["qtbase", "qtdeclarative"]),
                         ["6.11.3-0-202609240332qtbase-MacOS-X.7z", "6.11.3-0-202609240332qtdeclarative-MacOS-X.7z"])
        with self.assertRaises(ValueError):
            qt.archives_from_updates(UPDATES, "qt.qt6.6113.clang_64", ["qtwebengine"])
        with self.assertRaises(ValueError):
            qt.archives_from_updates(UPDATES, "qt.qt6.6113.missing", ["qtbase"])

    def test_unsafe_archive_members_are_rejected(self):
        qt.check_members(["bin/qmake", "lib/QtCore.framework/Versions/A/QtCore"])
        for name in ("/etc/passwd", "../outside", "lib/../../outside", ""):
            with self.assertRaises(ValueError, msg=name):
                qt.check_members([name])

    def source(self, name, payload):
        path = self.root / "repository" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
        return {"path": name, "size": len(payload), "sha256": hashlib.sha256(payload).hexdigest()}

    def test_download_rejects_digest_and_size_mismatch_without_partial_file(self):
        archive = self.source("pkg/archive.7z", b"verified bytes")
        base = (self.root / "repository").as_uri()
        target = self.root / "downloads/archive.7z"
        qt.download(base, archive, target)
        self.assertEqual(target.read_bytes(), b"verified bytes")
        for broken in (dict(archive, sha256="0" * 64), dict(archive, size=archive["size"] - 1)):
            target.unlink(missing_ok=True)
            with self.assertRaises(ValueError):
                qt.download(base, broken, target)
            self.assertFalse(target.exists())

    @unittest.skipUnless(shutil.which("bsdtar"), "bsdtar extracts the official 7-Zip archives")
    def test_install_extracts_only_verified_archives_into_a_fresh_prefix(self):
        payload = self.root / "payload"
        (payload / "bin").mkdir(parents=True)
        (payload / "bin/marker").write_text("qt\n")
        bundle = self.root / "bundle.tar"
        with tarfile.open(bundle, "w") as archive:
            archive.add(payload / "bin", arcname="bin")
        entry = self.source("pkg/qtbase.tar", bundle.read_bytes())
        pin = {"qt_version": "6.11.2", "repository": qt.REPOSITORY, "packages": [], "archives": [entry]}
        output = self.root / "qt"
        qt.install(pin, output, base=(self.root / "repository").as_uri(), check_version=False)
        self.assertEqual((output / "bin/marker").read_text(), "qt\n")
        self.assertEqual(json.loads((output / "zerus-qt.json").read_text())["qt_version"], "6.11.2")
        with self.assertRaises(ValueError):
            qt.install(pin, output, base=(self.root / "repository").as_uri(), check_version=False)
        tampered = self.root / "tampered"
        broken = dict(pin, archives=[dict(entry, sha256="0" * 64)])
        with self.assertRaises(ValueError):
            qt.install(broken, tampered, base=(self.root / "repository").as_uri(), check_version=False)
        self.assertFalse((tampered / "bin").exists())


if __name__ == "__main__":
    unittest.main()
