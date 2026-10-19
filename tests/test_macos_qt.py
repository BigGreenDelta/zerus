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
        # A framework found first outside the prefix is refused.
        self.image(self.root, framework("QtQmlCore"), "../../../", "QtCore")
        self.image(prefix, framework("QtQml"), "../../../../../lib", "QtCore", "QtNetwork", "QtQmlCore")
        with self.assertRaisesRegex(ValueError, r"QtQml loads @rpath/QtQmlCore\."):
            qt.check_tools(prefix, ["bin/lrelease"])
        # Nor does a missing tool or a file that is not a 64-bit Mach-O image.
        (prefix / "bin/qmake").write_text("#!/bin/sh\n")
        for tool in ("bin/lupdate", "bin/qmake"):
            with self.assertRaises(ValueError, msg=tool):
                qt.check_tools(prefix, [tool])

    def write(self, path, *images):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(universal(*images) if len(images) > 1 else images[0])

    def test_each_architecture_needs_framework_code_of_its_own(self):
        prefix = self.root / "qt"
        commands = [(qt.LC_RPATH, "@loader_path/../lib"), (LOAD, "@rpath/" + framework("QtQml")[len("lib/"):])]
        self.write(prefix / framework("QtQml"), macho(X86_64, []))
        for tool in ([macho(ARM64, commands)], [macho(X86_64, commands), macho(ARM64, commands)]):
            self.write(prefix / "bin/lrelease", *tool)
            with self.assertRaisesRegex(ValueError, r"bin/lrelease: QtQml has no arm64 code\."):
                qt.check_tools(prefix, ["bin/lrelease"])
        self.write(prefix / framework("QtQml"), macho(X86_64, []), macho(ARM64, []))
        qt.check_tools(prefix, ["bin/lrelease"])

    def test_absolute_names_outside_the_system_must_be_inside_the_prefix(self):
        prefix = self.root / "qt"
        outside = self.root / "outside" / framework("QtQml")
        self.write(outside, macho(ARM64, []))
        refused = {str(outside): "outside the Qt prefix", "/usr/lib/../.." + str(outside): "outside the Qt prefix",
                   str(self.root / "absent" / framework("QtQml")): "does not provide",
                   framework("QtQml"): "not supported", "@executable_path/../" + framework("QtQml"): "not supported"}
        for name, error in refused.items():
            self.write(prefix / "bin/lrelease", macho(ARM64, [(LOAD, name)]))
            with self.assertRaisesRegex(ValueError, error, msg=name):
                qt.check_tools(prefix, ["bin/lrelease"])
        # System libraries are accepted; an absolute name inside the prefix is checked in turn.
        inner = prefix / framework("QtQml")
        self.write(inner, macho(ARM64, [(LOAD, "@rpath/" + framework("QtCore")[len("lib/"):])]))
        self.write(prefix / "bin/lrelease", macho(ARM64, [
            (qt.LC_RPATH, "@loader_path/../lib"), (LOAD, "/usr/lib/libSystem.B.dylib"),
            (LOAD, "/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit"), (LOAD, str(inner))]))
        with self.assertRaisesRegex(ValueError, r"QtQml loads @rpath/QtCore\."):
            qt.check_tools(prefix, ["bin/lrelease"])
        self.write(prefix / framework("QtCore"), macho(ARM64, []))
        qt.check_tools(prefix, ["bin/lrelease"])

    def test_the_first_existing_run_path_candidate_is_the_one_checked(self):
        prefix = self.root / "qt"
        qml = (LOAD, "@rpath/" + framework("QtQml")[len("lib/"):])
        self.write(self.root / "outside" / framework("QtQml")[len("lib/"):], macho(ARM64, []))
        self.write(prefix / framework("QtQml"), macho(ARM64, []))
        self.write(prefix / "bin/lrelease", macho(ARM64, [
            (qt.LC_RPATH, "@loader_path/../../outside"), (qt.LC_RPATH, "@loader_path/../lib"), qml]))
        with self.assertRaisesRegex(ValueError, "outside the Qt prefix"):
            qt.check_tools(prefix, ["bin/lrelease"])
        # A run path without the image is passed over; an unsupported run path is refused.
        self.write(prefix / "bin/lrelease", macho(ARM64, [
            (qt.LC_RPATH, str(self.root / "absent")), (qt.LC_RPATH, "@loader_path/../lib"), qml]))
        qt.check_tools(prefix, ["bin/lrelease"])
        self.write(prefix / "bin/lrelease", macho(ARM64, [(qt.LC_RPATH, "@executable_path/../lib"), qml]))
        with self.assertRaisesRegex(ValueError, "has the run path @executable_path/../lib"):
            qt.check_tools(prefix, ["bin/lrelease"])

    def test_dependency_cycles_end_and_the_graph_is_bounded(self):
        prefix = self.root / "qt"
        own = (qt.LC_RPATH, "@loader_path/../../../")
        self.write(prefix / "bin/lrelease", macho(ARM64, [(qt.LC_RPATH, "@loader_path/../lib"),
                                                          (LOAD, "@rpath/A.framework/Versions/A/A")]))
        self.write(prefix / framework("A"), macho(ARM64, [own, (LOAD, "@rpath/B.framework/Versions/A/B")]))
        self.write(prefix / framework("B"), macho(ARM64, [own, (LOAD, "@rpath/A.framework/Versions/A/A")]))
        read = Path.read_bytes
        with mock.patch.object(Path, "read_bytes", autospec=True, side_effect=read) as reads:
            qt.check_tools(prefix, ["bin/lrelease"])
        self.assertEqual(reads.call_count, 3)
        with mock.patch.object(qt, "VISIT_LIMIT", 2), self.assertRaisesRegex(ValueError, "more than 2 images"):
            qt.check_tools(prefix, ["bin/lrelease"])

    def test_a_present_weak_image_is_checked(self):
        prefix = self.root / "qt"
        self.write(prefix / "bin/lrelease", macho(ARM64, [(qt.LC_RPATH, "@loader_path/../lib"),
                                                          (WEAK, "@rpath/A.framework/Versions/A/A")]))
        qt.check_tools(prefix, ["bin/lrelease"])
        self.write(prefix / framework("A"), macho(ARM64, [(LOAD, "@rpath/Missing.framework/Versions/A/Missing")]))
        with self.assertRaisesRegex(ValueError, r"A loads @rpath/Missing\."):
            qt.check_tools(prefix, ["bin/lrelease"])

    def test_malformed_images_are_refused(self):
        good = macho(ARM64, [(qt.LC_RPATH, "@loader_path/../lib"), (WEAK, "/usr/lib/libSystem.B.dylib")])
        fat = universal(macho(X86_64, []), good)
        self.assertEqual(qt.load_commands(good), {(ARM64, 0): ([("/usr/lib/libSystem.B.dylib", True)],
                                                               ["@loader_path/../lib"])})
        self.assertEqual(set(qt.load_commands(fat)), {(ARM64, 0), (X86_64, 0)})

        def patched(data, offset, value, form="<I"):
            return data[:offset] + struct.pack(form, value) + data[offset + struct.calcsize(form):]

        def thin(command):
            return struct.pack("<IiiIIIII", 0xFEEDFACF, ARM64, 0, 2, 1, len(command), 0, 0) + command

        table = struct.unpack_from("<I", good, 20)[0]
        broken = {
            "universal header without slices": struct.pack(">II", 0xCAFEBABE, 0),
            "universal table beyond the file": struct.pack(">II", 0xCAFEBABE, 2),
            "slice beyond the file": patched(fat, 20, len(fat), ">I"),
            "slice of another architecture": patched(fat, 8, ARM64, ">i"),
            "two slices of one architecture": universal(good, good),
            "64-bit universal header": struct.pack(">II", 0xCAFEBABF, 1) + bytes(64),
            "truncated header": good[:31],
            "script": b"#!/bin/sh\n",
            "command table beyond the image": patched(good, 20, len(good)),
            "more commands than the table holds": patched(good, 16, 3),
            "commands not filling the table": patched(good, 20, table + 8) + bytes(8),
            "command shorter than its header": patched(good, 36, 4),
            "unaligned command size": patched(good, 36, 36),
            "name beyond its command": patched(good, 40, 64),
            "name inside the command header": patched(good, 40, 8),
            "unterminated name": thin(struct.pack("<III", qt.LC_RPATH, 16, 12) + b"/lib"),
            "undecodable name": thin(struct.pack("<III", qt.LC_RPATH, 16, 12) + b"\xff\0\0\0"),
        }
        for case, data in broken.items():
            with self.subTest(case), self.assertRaises(ValueError):
                qt.load_commands(data)
        # A tool without any slice is not a tool without dependencies.
        self.write(self.root / "qt/bin/lrelease", broken["universal header without slices"])
        with self.assertRaisesRegex(ValueError, "Malformed universal Mach-O header"):
            qt.check_tools(self.root / "qt", ["bin/lrelease"])

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
