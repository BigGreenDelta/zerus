"""The pinned Qt of the Windows build must come from verified official archives."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import qt_archives  # noqa: E402

spec = importlib.util.spec_from_file_location("install_qt_windows", ROOT / "scripts/install-qt-windows.py")
qt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qt)

BASE = "qt.qt6.6103.win64_msvc2022_64"
MULTIMEDIA = "qt.qt6.6103.addons.qtmultimedia.win64_msvc2022_64"
# The archives aqtinstall 3.3.0 installed for `install-qt windows desktop 6.10.3
# win64_msvc2022_64 -m qtmultimedia`: every archive of the base package, and Qt
# Multimedia, each into the directory of its Extract operation.
AQT = {
    "qtbase": (BASE, ""), "qtsvg": (BASE, ""), "qtdeclarative": (BASE, ""), "qttools": (BASE, ""),
    "qttranslations": (BASE, ""), "d3dcompiler_47": (BASE, "/bin"), "opengl32sw": (BASE, "/bin"),
    "qtmultimedia": (MULTIMEDIA, ""),
}

UPDATES = """<Updates>
 <PackageUpdate><Name>qt.qt6.6112.win64_msvc2022_64</Name><Version>6.11.2-0-202608131017</Version>
  <DownloadableArchives>qtbase-Windows-X.7z, qtsvg-Windows-X.7z, d3dcompiler_47-x64.7z</DownloadableArchives>
  <Operations>
   <Operation name="Extract"><Argument>@TargetDir@/6.11.2/msvc2022_64</Argument>
    <Argument>qtbase-Windows-X.7z</Argument></Operation>
   <Operation name="Extract"><Argument>@TargetDir@/6.11.2/msvc2022_64</Argument>
    <Argument>qtsvg-Windows-X.7z</Argument></Operation>
   <Operation name="Extract"><Argument>@TargetDir@/6.11.2/msvc2022_64/bin</Argument>
    <Argument>d3dcompiler_47-x64.7z</Argument></Operation>
  </Operations></PackageUpdate>
</Updates>"""

LISTING = """
7-Zip 26.03 (x64) : Copyright (c) 1999-2026 Igor Pavlov : 2026-09-21

Listing archive: D:\\a\\_temp\\Qt.downloads\\00-qtbase.7z

--
Path = D:\\a\\_temp\\Qt.downloads\\00-qtbase.7z
Type = 7z
Physical Size = 42748757
Solid = +

----------
Path = bin
Size = 0
Attributes = D drwxr-xr-x

Path = bin\\Qt6Core.dll
Size = 10
Attributes = A -rw-r--r--
CRC = 4B247DFA

Path = lib\\cmake\\Qt6Core\\Qt6CoreConfig.cmake
Size = 0
Packed Size =
Attributes = A

"""


class PinnedWindowsQt(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="zerus-qt-windows-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.pin = json.loads((ROOT / "packaging/windows/qt.json").read_text())
        retry = mock.patch.object(qt_archives, "RETRY_DELAY", 0)
        retry.start()
        self.addCleanup(retry.stop)

    def test_repository_pin_installs_what_aqtinstall_did(self):
        qt.validate_pin(self.pin)
        self.assertEqual((self.pin["qt_version"], self.pin["prefix"]), ("6.10.3", "6.10.3/msvc2022_64"))
        self.assertEqual(self.pin["repository"], "https://download.qt.io/online/qtsdkrepository/windows_x86")
        self.assertEqual([(package["directory"], package["name"]) for package in self.pin["packages"]],
                         [("desktop/qt6_6103/qt6_6103", BASE), ("desktop/qt6_6103/qt6_6103", MULTIMEDIA)])
        installed = {}
        for archive in self.pin["archives"]:
            folder, file = archive["path"].rsplit("/", 1)
            module = file.removeprefix("6.10.3-0-202603310407").split("-", 1)[0]
            installed[module] = (folder.rsplit("/", 1)[1], archive["target"].removeprefix("6.10.3/msvc2022_64"))
        self.assertEqual(installed, AQT)
        # Qt WebEngine is an optional Linux plugin and needs modules the build never uses.
        self.assertNotIn("webengine", json.dumps(self.pin))

    def test_pin_rejects_foreign_sources_targets_and_stray_archives(self):
        changes = [
            lambda pin: pin.update(repository=qt_archives.MACOS_REPOSITORY),
            lambda pin: pin.update(repository="https://mirror.example/qt/windows_x86"),
            lambda pin: pin.update(prefix="6.11.2/msvc2022_64"),
            lambda pin: pin.update(prefix="../msvc2022_64"),
            lambda pin: pin.pop("prefix"),
            lambda pin: pin["archives"][0].pop("target"),
            lambda pin: pin["archives"][0].update(target="6.10.3/msvc2022_arm64"),
            lambda pin: pin["archives"][0].update(target="6.10.3/msvc2022_64/../../outside"),
            lambda pin: pin["archives"][0].update(target="/6.10.3/msvc2022_64"),
            lambda pin: pin["archives"][0].update(path=pin["archives"][0]["path"].replace("6.10.3-", "6.10.2-")),
            lambda pin: pin["archives"][0].update(path="desktop/qt6_6103/qt6_6103/qt.other/6.10.3-0-1qtbase-x.7z"),
            lambda pin: pin["archives"][0].update(sha256="A" * 64),
            lambda pin: pin["archives"][0].update(size=True),
            lambda pin: pin["archives"].pop(),
            lambda pin: pin["packages"][1]["archives"].append("qtwebengine"),
            lambda pin: pin["packages"][0].update(name="qt.qt6.6103.win64_msvc2022_64/../x"),
            lambda pin: pin["packages"].pop(),
        ]
        for index, change in enumerate(changes):
            pin = copy.deepcopy(self.pin)
            change(pin)
            with self.subTest(change=index), self.assertRaises(ValueError):
                qt.validate_pin(pin)

    def test_updates_metadata_names_each_archive_and_its_directory(self):
        self.assertEqual(qt_archives.package_archives(UPDATES, "qt.qt6.6112.win64_msvc2022_64",
                                                      ["qtbase", "d3dcompiler_47"]),
                         [("6.11.2-0-202608131017qtbase-Windows-X.7z", "6.11.2/msvc2022_64"),
                          ("6.11.2-0-202608131017d3dcompiler_47-x64.7z", "6.11.2/msvc2022_64/bin")])
        for modules in (["qtmultimedia"], ["qt"]):
            with self.assertRaises(ValueError):
                qt_archives.package_archives(UPDATES, "qt.qt6.6112.win64_msvc2022_64", modules)
        broken = UPDATES.replace("@TargetDir@/6.11.2/msvc2022_64/bin", "C:/Qt/6.11.2/msvc2022_64/bin")
        with self.assertRaises(ValueError):
            qt_archives.package_archives(broken, "qt.qt6.6112.win64_msvc2022_64", ["qtbase"])

    def test_split_repository_layout_of_qt_6_11_is_expressible(self):
        # From Qt 6.11 each Windows architecture has a repository directory of its
        # own; the pin names it per package, and validation stays the same.
        folder = "desktop/qt6_6112/qt6_6112_msvc2022_64"
        archive = {"path": f"{folder}/qt.qt6.6112.win64_msvc2022_64/6.11.2-0-202608131017qtbase-Windows-X.7z",
                   "size": 1, "sha256": "0" * 64, "target": "6.11.2/msvc2022_64"}
        pin = {"qt_version": "6.11.2", "repository": qt.REPOSITORY, "prefix": "6.11.2/msvc2022_64",
               "packages": [{"directory": folder, "name": "qt.qt6.6112.win64_msvc2022_64", "archives": ["qtbase"]}],
               "archives": [archive]}
        self.assertIs(qt.validate_pin(pin), pin)

    def test_member_names_cannot_leave_or_alias_the_target(self):
        qt_archives.check_members(["bin/Qt6Core.dll", "bin\\Qt6Core.dll", "lib/cmake/Qt6/", "./bin/qmake.exe",
                                   "plugins/platforms/qwindows.dll", "translations/qt_zh_TW.qm"])
        for name in ("", "/", "/etc/passwd", "\\Windows\\win.ini", "\\\\server\\share\\x", "C:\\Windows\\x",
                     "C:x", "bin/Qt6Core.dll:stream", "../outside", "bin\\..\\..\\outside", "lib/../../outside",
                     "bin//../../outside", "con", "bin/NUL.txt", "lib/COM1", "bin/name.", "bin/name ", "bin/a*b",
                     "bin/\x01", "bin/a|b"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                qt_archives.check_member(name)

    def test_seven_zip_listing_reports_members_and_links(self):
        self.assertEqual(qt_archives.seven_zip_members(LISTING.replace("\n", "\r\n")),
                         [("bin", False), ("bin\\Qt6Core.dll", False),
                          ("lib\\cmake\\Qt6Core\\Qt6CoreConfig.cmake", False)])
        links = ["Attributes = A lrwxrwxrwx", "Attributes = AL", "Attributes = DL drwxr-xr-x",
                 "Attributes = A\nSymbolic Link = ..\\..\\outside", "Attributes = A\nHard Link = bin\\x",
                 "Attributes = A\nAnti = +", "Attributes = A prw-r--r--"]
        for attributes in links:
            listing = LISTING.replace("Attributes = A -rw-r--r--", attributes)
            with self.subTest(attributes=attributes):
                self.assertIn(("bin\\Qt6Core.dll", True), qt_archives.seven_zip_members(listing))
        for broken in (LISTING.replace("Type = 7z", "Type = zip"), LISTING.replace("----------", "-------"),
                       LISTING.replace("CRC = 4B247DFA", "unparsed line"),
                       LISTING.replace("Size = 10", "Path = bin\\other.dll")):
            with self.assertRaises(ValueError):
                qt_archives.seven_zip_members(broken)

    def source(self, name, payload, target="6.10.3/msvc2022_64"):
        path = self.root / "repository" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
        return {"path": name, "size": len(payload), "sha256": hashlib.sha256(payload).hexdigest(), "target": target}

    def test_download_stops_at_the_pinned_size_and_keeps_nothing_unverified(self):
        archive = self.source("pkg/archive.7z", b"verified bytes")
        base = (self.root / "repository").as_uri()
        target = self.root / "downloads/archive.7z"
        self.assertEqual(qt_archives.download(base, archive, target), f"{base}/pkg/archive.7z")
        self.assertEqual(target.read_bytes(), b"verified bytes")
        for broken in (dict(archive, sha256="0" * 64), dict(archive, size=archive["size"] - 1),
                       dict(archive, size=archive["size"] + 1)):
            target.unlink(missing_ok=True)
            with self.subTest(broken=broken), self.assertRaises(ValueError):
                qt_archives.download(base, broken, target)
            self.assertEqual(list(target.parent.iterdir()), [])
        # Every failed attempt is reported; the last one fails the installation.
        with mock.patch("sys.stderr") as stderr, self.assertRaisesRegex(ValueError, "in 3 attempts"):
            qt_archives.fetch(base, dict(archive, sha256="0" * 64), target)
        self.assertEqual(sum("failed" in str(call) for call in stderr.write.call_args_list), 2)
        with mock.patch("sys.stderr"), self.assertRaisesRegex(ValueError, "in 3 attempts"):
            qt_archives.fetch(base, dict(archive, path="pkg/missing.7z"), target)
        self.assertFalse(target.exists())

    def test_links_must_stay_inside_the_installation(self):
        tree = self.root / "tree"
        (tree / "lib/QtCore.framework/Versions/A").mkdir(parents=True)
        os.symlink("A", tree / "lib/QtCore.framework/Versions/Current")
        qt_archives.check_tree(tree)
        os.symlink("../../../outside", tree / "lib/escape")
        with self.assertRaisesRegex(ValueError, "leaves the installation"):
            qt_archives.check_tree(tree)

    def test_lock_records_digests_and_extract_directories(self):
        folder = "desktop/qt6_6103/qt6_6103"
        repository = self.root / "repository"
        (repository / folder).mkdir(parents=True)
        (repository / folder / "Updates.xml").write_text(UPDATES.replace("6.11.2", "6.10.3").replace("6112", "6103"))
        files = {}
        for name, payload in (("qtbase-Windows-X.7z", b"base"), ("d3dcompiler_47-x64.7z", b"d3d")):
            path = repository / folder / BASE / f"6.10.3-0-202608131017{name}"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(payload)
            digest = hashlib.sha256(payload).hexdigest()
            path.with_name(path.name + ".sha256").write_text(f"{digest}  {path.name}\n")
            files[name] = (len(payload), digest)
        pin = {"qt_version": "6.10.3", "repository": repository.as_uri(), "prefix": "6.10.3/msvc2022_64",
               "packages": [{"directory": folder, "name": BASE, "archives": ["qtbase", "d3dcompiler_47"]}],
               "archives": []}
        with mock.patch("sys.stdout"):
            locked = qt_archives.lock(pin, repository.as_uri(), targets=True)
        self.assertEqual([(entry["size"], entry["sha256"], entry["target"]) for entry in locked["archives"]],
                         [(*files["qtbase-Windows-X.7z"], "6.10.3/msvc2022_64"),
                          (*files["d3dcompiler_47-x64.7z"], "6.10.3/msvc2022_64/bin")])
        # A published digest that the served archive does not match stops the lock.
        (repository / folder / BASE / "6.10.3-0-202608131017qtbase-Windows-X.7z.sha256").write_text("0" * 64 + "\n")
        with mock.patch("sys.stdout"), self.assertRaisesRegex(ValueError, "published SHA-256"):
            qt_archives.lock(pin, repository.as_uri(), targets=True)

    def test_missing_seven_zip_is_reported(self):
        with mock.patch("shutil.which", return_value=None), self.assertRaisesRegex(ValueError, "7-Zip is required"):
            qt.seven_zip(None)
        self.assertEqual(qt.seven_zip("C:/Tools/7z.exe").program, "C:/Tools/7z.exe")


@unittest.skipUnless(shutil.which("7z"), "7-Zip extracts the official Windows archives")
class WindowsQtInstallation(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="zerus-qt-windows-install-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.repository = self.root / "repository"
        self.tool = qt_archives.SevenZip(shutil.which("7z"))
        for patcher in (mock.patch.object(qt_archives, "RETRY_DELAY", 0), mock.patch("sys.stdout")):
            patcher.start()
            self.addCleanup(patcher.stop)

    def archive(self, package, module, files, target="6.10.3/msvc2022_64", links=()):
        """A 7z archive of the files and links, served from the local repository."""
        content = self.root / "content" / f"{package}-{module}"
        for name, text in files.items():
            (content / name).parent.mkdir(parents=True, exist_ok=True)
            (content / name).write_text(text)
        for name, link in links:
            (content / name).parent.mkdir(parents=True, exist_ok=True)
            os.symlink(link, content / name)
        path = f"desktop/qt6_6103/qt6_6103/{package}/6.10.3-0-202603310407{module}-Windows-X86_64.7z"
        output = self.repository / path
        output.parent.mkdir(parents=True, exist_ok=True)
        # 7-Zip's own wildcard, not file names: p7zip (the 7z of the macOS runners)
        # follows a link named on its command line, failing on a dangling one, and
        # stores a link only when a wildcard finds it.
        subprocess.run([self.tool.program, "a", "-bd", "-snl", str(output), "*"],
                       cwd=content, check=True, capture_output=True)
        payload = output.read_bytes()
        return {"path": path, "size": len(payload), "sha256": hashlib.sha256(payload).hexdigest(), "target": target}

    def pin(self, archives, packages):
        return {"qt_version": "6.10.3", "repository": qt.REPOSITORY, "prefix": "6.10.3/msvc2022_64",
                "packages": [{"directory": "desktop/qt6_6103/qt6_6103", "name": name, "archives": modules}
                             for name, modules in packages], "archives": archives}

    def qt_archives(self):
        version = 'set(PACKAGE_VERSION "6.10.3")\n'
        base = self.archive(BASE, "qtbase", {
            "bin/windeployqt.exe": "deploy", "bin/qtpaths.exe": "paths", "bin/Qt6Core.dll": "core",
            "lib/cmake/Qt6Core/Qt6CoreConfigVersionImpl.cmake": version, "sbom/qtbase-6.10.3.spdx": "spdx"})
        d3d = self.archive(BASE, "d3dcompiler_47", {"d3dcompiler_47.dll": "d3d"}, "6.10.3/msvc2022_64/bin")
        multimedia = self.archive(MULTIMEDIA, "qtmultimedia", {"bin/Qt6Multimedia.dll": "multimedia"})
        return [base, d3d, multimedia], [(BASE, ["qtbase", "d3dcompiler_47"]), (MULTIMEDIA, ["qtmultimedia"])]

    def test_install_writes_the_aqtinstall_layout_from_verified_archives(self):
        pin = self.pin(*self.qt_archives())
        output = self.root / "Qt"
        prefix = qt.install(pin, output, self.tool, base=self.repository.as_uri())
        self.assertEqual(prefix, output / "6.10.3/msvc2022_64")
        self.assertEqual((prefix / "bin/d3dcompiler_47.dll").read_text(), "d3d")
        self.assertEqual((prefix / "bin/Qt6Multimedia.dll").read_text(), "multimedia")
        self.assertEqual((prefix / "bin/qt.conf").read_text(), "[Paths]\nPrefix=..\n")
        self.assertEqual(sorted(path.name for path in output.iterdir()), ["6.10.3"])
        self.assertFalse((self.root / "Qt.downloads").exists())
        self.assertEqual(qt.check(output, pin), prefix)
        # A cache restored for another pin is refused.
        other = copy.deepcopy(pin)
        other["archives"][0]["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "another Qt installation"):
            qt.check(output, other)
        with self.assertRaisesRegex(ValueError, "fresh Qt directory"):
            qt.install(pin, output, self.tool, base=self.repository.as_uri())

    def test_nothing_is_extracted_unless_every_archive_verifies(self):
        archives, packages = self.qt_archives()
        archives[-1] = dict(archives[-1], sha256="0" * 64)
        output = self.root / "Qt"
        with mock.patch("sys.stderr"), self.assertRaisesRegex(ValueError, "checksum"):
            qt.install(self.pin(archives, packages), output, self.tool, base=self.repository.as_uri())
        self.assertFalse(output.exists())

    def test_archives_with_links_are_refused_before_extraction(self):
        archives, packages = self.qt_archives()
        archives[1] = self.archive(BASE, "d3dcompiler_47", {"d3dcompiler_47.dll": "d3d"}, "6.10.3/msvc2022_64/bin",
                                   links=[("escape", "../../../../outside")])
        output = self.root / "Qt"
        with self.assertRaisesRegex(ValueError, "contains links: escape"):
            qt.install(self.pin(archives, packages), output, self.tool, base=self.repository.as_uri())
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
