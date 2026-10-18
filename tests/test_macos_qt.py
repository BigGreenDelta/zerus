"""The pinned Qt used for the macOS bundle must come from verified official archives."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
spec = importlib.util.spec_from_file_location("install_qt_macos", ROOT / "scripts/install-qt-macos.py")
qt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qt)

UPDATES = """<Updates>
 <PackageUpdate><Name>qt.qt6.6113.clang_64</Name><Version>6.11.3-0-202609240332</Version>
  <DownloadableArchives>qtbase-MacOS-X.7z, qtdeclarative-MacOS-X.7z, qtdoc-MacOS-X.7z</DownloadableArchives></PackageUpdate>
 <PackageUpdate><Name>qt.qt6.6113.clang_64.debug</Name><Version>6.11.3-0-1</Version>
  <DownloadableArchives>qtbase-debug.7z</DownloadableArchives></PackageUpdate>
</Updates>"""
ARM64, X86_64 = 0x0100000C, 0x01000007
LOAD, WEAK = 0xC, 0x80000018


def macho(cputype, commands):
    """A 64-bit Mach-O image with the given (command, name) load commands."""
    body = b""
    for command, name in commands:
        # A dylib command has a name offset, a timestamp and two versions; a run path only its offset.
        start = 12 if command == qt.LC_RPATH else 24
        size = (start + len(name) + 8) // 8 * 8
        body += struct.pack("<III", command, size, start) + bytes(start - 12) + name.encode().ljust(size - start, b"\0")
    return struct.pack("<IiiIIIII", 0xFEEDFACF, cputype, 0, 2, len(commands), len(body), 0, 0) + body


def universal(*images):
    """A universal file of thin images at 16 KiB boundaries, as the official Qt ships them."""
    entries, payload = b"", b""
    for image in images:
        cputype = struct.unpack_from("<i", image, 4)[0]
        entries += struct.pack(">iiIII", cputype, 0, 0x4000 + len(payload), len(image), 14)
        payload += image.ljust(-(-len(image) // 0x4000) * 0x4000, b"\0")
    return (struct.pack(">II", 0xCAFEBABE, len(images)) + entries).ljust(0x4000, b"\0") + payload


def framework(name):
    return f"lib/{name}.framework/Versions/A/{name}"


class PinnedQt(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="zerus-qt-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.pin = json.loads((ROOT / "packaging/macos/qt.json").read_text())
        retry = mock.patch.object(qt.qt_archives, "RETRY_DELAY", 0)
        retry.start()
        self.addCleanup(retry.stop)

    def test_repository_pin_is_valid_and_complete(self):
        qt.validate_pin(self.pin)
        self.assertEqual(self.pin["qt_version"], "6.11.2")
        names = " ".join(archive["path"] for archive in self.pin["archives"])
        # The build-time lrelease of qttools loads QtQml, which qtdeclarative provides.
        for module in ("qtbase", "qtsvg", "qtmultimedia", "qttools", "qtdeclarative"):
            self.assertIn(module + "-", names)
        # Qt WebEngine and the modules only it needs are not pinned.
        for module in ("qtwebchannel", "qtpositioning", "qtwebengine"):
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

    def image(self, prefix, name, run_path, *frameworks, weak=()):
        """A universal image of the official layout that loads the Qt frameworks through @rpath."""
        commands = [(LOAD, "@rpath/" + framework(item)[len("lib/"):]) for item in frameworks]
        commands += [(WEAK, "@rpath/" + framework(item)[len("lib/"):]) for item in weak]
        commands += [(LOAD, "/usr/lib/libSystem.B.dylib"), (qt.LC_RPATH, "@loader_path/" + run_path)]
        path = prefix / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(universal(macho(X86_64, commands), macho(ARM64, commands)))

    def test_build_tools_find_every_qt_framework_they_load(self):
        # The official Qt 6.11.2 lrelease loads QtQml, which only qtdeclarative
        # provides: without it dyld aborted lrelease at qrc_translations in CI.
        prefix = self.root / "qt"
        self.image(prefix, "bin/lrelease", "../lib", "QtCore", "QtNetwork", "QtQml")
        self.image(prefix, framework("QtCore"), "../../../")
        self.image(prefix, framework("QtNetwork"), "../../../", "QtCore")
        missing = r"bin/lrelease: lrelease loads @rpath/QtQml\.framework/Versions/A/QtQml,"
        with self.assertRaisesRegex(ValueError, missing):
            qt.check_tools(prefix, ["bin/lrelease"])
        self.image(prefix, framework("QtQml"), "../../../", "QtCore", "QtNetwork", weak=["QtQmlMeta"])
        qt.check_tools(prefix, ["bin/lrelease"])
        # Each loaded framework's own dependencies count too.
        self.image(prefix, framework("QtQml"), "../../../", "QtCore", "QtNetwork", "QtQmlCore")
        with self.assertRaisesRegex(ValueError, r"QtQml loads @rpath/QtQmlCore\."):
            qt.check_tools(prefix, ["bin/lrelease"])
        # A framework found only outside the prefix does not count.
        self.image(self.root, framework("QtQmlCore"), "../../../", "QtCore")
        self.image(prefix, framework("QtQml"), "../../../../../lib", "QtCore", "QtNetwork", "QtQmlCore")
        with self.assertRaisesRegex(ValueError, r"QtQml loads @rpath/QtQmlCore\."):
            qt.check_tools(prefix, ["bin/lrelease"])
        # Nor does a missing tool or a file that is not a 64-bit Mach-O image.
        (prefix / "bin/qmake").write_text("#!/bin/sh\n")
        for tool in ("bin/lupdate", "bin/qmake"):
            with self.assertRaises(ValueError, msg=tool):
                qt.check_tools(prefix, [tool])

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
        entry = self.source("desktop/qt.test.clang_64/6.11.2-0-1qtbase-MacOS.tar", bundle.read_bytes())
        pin = {"qt_version": "6.11.2", "repository": qt.REPOSITORY, "archives": [entry],
               "packages": [{"directory": "desktop", "name": "qt.test.clang_64", "archives": ["qtbase"]}]}
        output = self.root / "qt"
        with mock.patch("sys.stdout"):
            qt.install(pin, output, base=(self.root / "repository").as_uri(), check_version=False)
        self.assertEqual((output / "bin/marker").read_text(), "qt\n")
        self.assertEqual(json.loads((output / "zerus-qt.json").read_text())["qt_version"], "6.11.2")
        with self.assertRaises(ValueError):
            qt.install(pin, output, base=(self.root / "repository").as_uri(), check_version=False)
        tampered = self.root / "tampered"
        broken = dict(pin, archives=[dict(entry, sha256="0" * 64)])
        with mock.patch("sys.stderr"), self.assertRaises(ValueError):
            qt.install(broken, tampered, base=(self.root / "repository").as_uri(), check_version=False)
        self.assertFalse((tampered / "bin").exists())

    @unittest.skipUnless(shutil.which("bsdtar"), "bsdtar extracts the official 7-Zip archives")
    def test_install_rejects_a_build_tool_with_a_missing_framework_before_running_qt(self):
        payload = self.root / "payload"
        self.image(payload, "bin/lrelease", "../lib", "QtCore", "QtQml")
        self.image(payload, framework("QtCore"), "../../../")
        bundle = self.root / "bundle.tar"
        with tarfile.open(bundle, "w") as archive:
            for name in ("bin", "lib"):
                archive.add(payload / name, arcname=name)
        entry = self.source("desktop/qt.test.clang_64/6.11.2-0-1qttools-MacOS.tar", bundle.read_bytes())
        pin = {"qt_version": "6.11.2", "repository": qt.REPOSITORY, "archives": [entry],
               "packages": [{"directory": "desktop", "name": "qt.test.clang_64", "archives": ["qttools"]}]}
        with mock.patch.object(qt, "BUILD_TOOLS", ("bin/lrelease",)), mock.patch("sys.stdout"), \
                mock.patch.object(qt.subprocess, "check_output") as run, \
                self.assertRaisesRegex(ValueError, r"loads @rpath/QtQml\.framework/Versions/A/QtQml,"):
            qt.install(pin, self.root / "qt", base=(self.root / "repository").as_uri())
        run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
