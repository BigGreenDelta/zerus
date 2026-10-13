"""Windows portable zip contracts: packaging, layout checks and size limits.

Packaging runs with a synthetic Qt installation, windeployqt and MSVC runtime,
so these checks need no Windows tools and never execute bundled programs.
"""
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import tomllib
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import windows_portable as portable

COMMIT = "a" * 40
VERSION = (ROOT / "VERSION").read_text().strip()
CLI_VERSION = tomllib.loads((ROOT / "Cargo.toml").read_text())["package"]["version"]
QT_VERSION = "6.10.3"
# What the fake windeployqt deploys next to hgs-tray.exe.
QT_RUNTIME = [name for name in portable.REQUIRED if name.startswith(("Qt", "platforms/", "imageformats/",
                                                                   "iconengines/", "resources/", "multimedia/",
                                                                   "avcodec-", "avformat-", "avutil-", "swresample-", "swscale-"))]


def manifest(**updates):
    value = {"product": "Zerus", "version": VERSION, "cli_version": CLI_VERSION, "platform": "Windows",
             "architecture": "x86_64", "qt_version": QT_VERSION, "native_agents_bundled": False,
             "source_commit": COMMIT}
    value.update(updates)
    return value


def bundle(path, files=None, info=None, entries=()):
    """A minimal zip with every required file; entries adds raw ZipInfo/data pairs."""
    files = {name: b"x" for name in portable.REQUIRED} if files is None else files
    files = {**files, "build-info.json": json.dumps(manifest() if info is None else info).encode()}
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(files.items()):
            archive.writestr(f"{portable.TOP}/{name}", data)
        for entry, data in entries:
            archive.writestr(entry, data)
    return path


class InspectTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="zerus-windows-contract-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)

    def test_complete_bundle_returns_its_manifest(self):
        self.assertEqual(portable.inspect(bundle(self.directory / "ok.zip")), manifest())

    def test_unsafe_names_and_entry_types_are_rejected(self):
        def raw(name, mode=stat.S_IFREG | 0o644):
            # Set the name after construction: ZipInfo would rewrite a Windows separator.
            entry = zipfile.ZipInfo("placeholder")
            entry.filename = name
            entry.create_system = 3
            entry.external_attr = mode << 16
            return entry
        unsafe = [raw(f"{portable.TOP}/../outside"), raw("/absolute"), raw(f"{portable.TOP}\\windows"),
                  raw(f"{portable.TOP}/C:drive"), raw(f"{portable.TOP}/stream:data"), raw("other/file"),
                  raw(f"{portable.TOP}/nul.txt"), raw(f"{portable.TOP}/trailing."), raw(f"{portable.TOP}//empty"),
                  raw(f"{portable.TOP}/HGS.EXE"), raw(f"{portable.TOP}/folder/", stat.S_IFDIR | 0o755),
                  raw(f"{portable.TOP}/link", stat.S_IFLNK | 0o777)]
        for index, entry in enumerate(unsafe):
            with self.subTest(entry=entry.filename), self.assertRaises(ValueError):
                portable.inspect(bundle(self.directory / f"unsafe-{index}.zip", entries=[(entry, b"x")]))

    def test_missing_runtime_or_notices_are_rejected(self):
        for name in ("hgs.exe", "Qt6Multimedia.dll", "multimedia/ffmpegmediaplugin.dll", "imageformats/qsvg.dll",
                     "vcruntime140_1.dll", "licenses/qt/LGPL-3.0-only.txt", "licenses/qt/IJG.txt",
                     "licenses/rust/inventory.json"):
            files = {item: b"x" for item in portable.REQUIRED if item != name}
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "lacks: " + name):
                portable.inspect(bundle(self.directory / "missing.zip", files=files))

    def test_system_libraries_and_software_opengl_are_rejected(self):
        # windeployqt copies System32's ICU forwarder, Mesa's opengl32sw.dll and
        # Microsoft's Direct3D shader compilers by default.
        # Qt WebEngine (Chromium) is never part of the Windows bundle.
        for name in ("icuuc.dll", "ICUIN.DLL", "icu.dll", "opengl32sw.dll", "plugins/icudt72.dll",
                     "D3Dcompiler_47.dll", "d3dcompiler_46.dll", "dxcompiler.dll", "DXIL.dll",
                     "Qt6WebEngineCore.dll", "Qt6WebEngineWidgets.dll", "QtWebEngineProcess.exe",
                     "resources/icudtl.dat", "resources/qtwebengine_resources.pak", "resources/v8_context_snapshot.bin"):
            files = {**{item: b"x" for item in portable.REQUIRED}, name: b"x"}
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "must not contain"):
                portable.inspect(bundle(self.directory / "forbidden.zip", files=files))
        self.assertFalse([name for name in portable.REQUIRED if "webengine" in name.lower()])

    def test_manifest_must_name_the_exact_windows_build(self):
        for updates in ({"platform": "Arch Linux"}, {"architecture": "arm64"}, {"native_agents_bundled": True},
                        {"source_commit": "a" * 7}, {"version": "0.39"}, {"qt_version": None}, {"product": "Other"}):
            with self.subTest(updates=updates), self.assertRaises(ValueError):
                portable.inspect(bundle(self.directory / "manifest.zip", info=manifest(**updates)))
        with self.assertRaisesRegex(ValueError, "not an object"):
            portable.inspect(bundle(self.directory / "list.zip", info=[]))
        files = {name: b"x" for name in portable.REQUIRED}
        path = bundle(self.directory / "large.zip", files=files, info={**manifest(), "padding": "x" * portable.MANIFEST_LIMIT})
        with self.assertRaisesRegex(ValueError, "too large"):
            portable.inspect(path)

    def test_size_and_entry_limits(self):
        path = bundle(self.directory / "limits.zip")
        with patch.object(portable, "ZIP_LIMIT", path.stat().st_size - 1), \
                self.assertRaisesRegex(ValueError, "release asset limit"):
            portable.inspect(path)
        with patch.object(portable, "UNPACKED_LIMIT", 10), self.assertRaisesRegex(ValueError, "unpacks beyond"):
            portable.inspect(path)
        with patch.object(portable, "ENTRY_LIMIT", 3), self.assertRaisesRegex(ValueError, "entry count"):
            portable.inspect(path)
        (self.directory / "broken.zip").write_bytes(b"not a zip")
        with self.assertRaisesRegex(ValueError, "not a valid zip"):
            portable.inspect(self.directory / "broken.zip")

    def test_every_shipped_license_text_is_required(self):
        texts = sorted(path.stem for path in (ROOT / "packaging/windows/licenses").glob("*.txt"))
        self.assertEqual(texts, sorted(portable.QT_LICENSE_TEXTS))
        for name in texts:
            self.assertGreater((ROOT / "packaging/windows/licenses" / f"{name}.txt").stat().st_size, 200, name)

    def test_limits_match_the_release_mirror_and_candidate_bounds(self):
        import update_feed
        self.assertEqual(portable.ZIP_LIMIT, update_feed.ASSET_LIMIT)
        self.assertEqual(portable.UNPACKED_LIMIT, 500_000_000)


class SbomLicenseTests(unittest.TestCase):
    DOCUMENT = "\n".join([
        "SPDXVersion: SPDX-2.3",
        "PackageName: BundledLibjpeg",
        "PackageLicenseConcluded: IJG AND BSD-3-Clause",
        "PackageLicenseDeclared: (FTL OR GPL-2.0-only) AND LicenseRef-Local",
        "PackageCopyrightText: <text>Copyright (C) 1991 Someone",
        "LicenseConcluded: Not-A-Tag-Inside-Text</text>",
        "FileName: ./qtcore.cpp",
        "LicenseConcluded: LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only WITH Qt-GPL-exception-1.0",
        "LicenseInfoInFile: NOASSERTION",
        "PackageLicenseConcluded: DocumentRef-qtbase-1:LicenseRef-Shared",
        "LicenseID: LicenseRef-Local",
        "ExtractedText: <text>Permission is granted.",
        "met: conditions apply</text>",
        "LicenseID: LicenseRef-Empty",
        "ExtractedText: <text></text>",
        "",
    ])

    def test_identifiers_and_extracted_texts_are_read(self):
        named, defined = portable.sbom_licenses(self.DOCUMENT)
        self.assertEqual(named, {"IJG", "BSD-3-Clause", "FTL", "GPL-2.0-only", "LicenseRef-Local", "LGPL-3.0-only",
                                 "GPL-3.0-only", "Qt-GPL-exception-1.0", "DocumentRef-qtbase-1:LicenseRef-Shared"})
        self.assertEqual(defined, {"LicenseRef-Local"})

    def test_every_named_license_needs_a_text(self):
        texts = set(portable.QT_LICENSE_TEXTS)
        other = "LicenseID: LicenseRef-Shared\nExtractedText: <text>Shared terms</text>\n"
        self.assertEqual(portable.missing_license_texts({"a.spdx": self.DOCUMENT, "b.spdx": other}, texts), [])
        self.assertEqual(portable.missing_license_texts({"a.spdx": self.DOCUMENT}, texts),
                         ["a.spdx: DocumentRef-qtbase-1:LicenseRef-Shared"])
        self.assertEqual(portable.missing_license_texts({"a.spdx": self.DOCUMENT, "b.spdx": other}, texts - {"IJG"}),
                         ["a.spdx: IJG"])
        unknown = "PackageLicenseConcluded: Made-Up-1.0 AND LicenseRef-Empty AND LicenseRef-Elsewhere\n"
        self.assertEqual(portable.missing_license_texts({"c.spdx": unknown, "b.spdx": other}, texts),
                         ["c.spdx: LicenseRef-Elsewhere", "c.spdx: LicenseRef-Empty", "c.spdx: Made-Up-1.0"])
        with self.assertRaisesRegex(ValueError, "Unterminated"):
            portable.sbom_licenses("PackageCopyrightText: <text>open\n")


@unittest.skipIf(os.name == "nt", "the synthetic windeployqt is a POSIX script; Windows CI packages the real bundle")
class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="zerus-windows-package-")
        self.addCleanup(self.temporary.cleanup)
        base = self.directory = Path(self.temporary.name)
        self.qt = base / "Qt" / QT_VERSION / "msvc2022_64"
        version_file = self.qt / "lib/cmake/Qt6Core/Qt6CoreConfigVersionImpl.cmake"
        version_file.parent.mkdir(parents=True)
        version_file.write_text(f'set(PACKAGE_VERSION "{QT_VERSION}")\n')
        (self.qt / "sbom").mkdir()
        for name in ("qtbase", "qtsvg", "qtmultimedia"):
            (self.qt / "sbom" / f"{name}-{QT_VERSION}.spdx").write_text(f"SPDXVersion: SPDX-2.3\nName: {name}\n")
        (self.qt / "sbom" / f"qtbase-{QT_VERSION}.spdx").write_text("\n".join([
            "SPDXVersion: SPDX-2.3", "PackageName: BundledLibjpeg", "PackageLicenseConcluded: IJG AND BSD-3-Clause",
            "PackageLicenseConcluded: LGPL-3.0-only OR GPL-3.0-only WITH Qt-GPL-exception-1.0",
            "PackageLicenseConcluded: LicenseRef-Qt-Commercial", "LicenseID: LicenseRef-Qt-Commercial",
            "ExtractedText: <text>Licensees holding valid commercial Qt licenses",
            "may use this file in accordance with the commercial license agreement.</text>", ""]))
        (self.qt / "sbom" / f"qtmultimedia-{QT_VERSION}.spdx").write_text("\n".join([
            "SPDXVersion: SPDX-2.3", "PackageName: QtMultimedia",
            "PackageLicenseConcluded: Unlicense OR MIT-0",
            "PackageName: FFmpeg",
            "PackageLicenseConcluded: LGPL-2.1-or-later AND BSD-Source-Code AND ISC", ""]))
        (self.qt / "sbom" / f"qtbase-{QT_VERSION}.source.spdx").write_text("source\n")
        self.windeployqt = base / "windeployqt"
        self.windeployqt.write_text(f"#!{sys.executable}\n" + "\n".join([
            "import json, pathlib, sys",
            "target = pathlib.Path(sys.argv[-1])",
            "pathlib.Path(__file__).with_name('arguments.json').write_text(json.dumps(sys.argv[1:]))",
            # It also copies the System32 ICU forwarder that Qt6Core imports and,
            # unless told not to, Microsoft's Direct3D shader compilers.
            f"names = {QT_RUNTIME + ['icuuc.dll']!r}",
            "if '--no-system-d3d-compiler' not in sys.argv: names.append('D3Dcompiler_47.dll')",
            "if '--no-system-dxc-compiler' not in sys.argv: names += ['dxcompiler.dll', 'dxil.dll']",
            "for name in names:",
            "    path = target.parent / name",
            "    path.parent.mkdir(parents=True, exist_ok=True)",
            "    path.write_bytes(b'qt ' + name.encode())",
        ]) + "\n")
        self.windeployqt.chmod(0o755)
        self.crt = base / "Microsoft.VC143.CRT"
        self.crt.mkdir()
        for name in ("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll", "concrt140.dll"):
            (self.crt / name).write_bytes(name.encode())
        self.rust = base / "rust-licenses"
        (self.rust / "serde-1.0.0").mkdir(parents=True)
        (self.rust / "serde-1.0.0" / "LICENSE-MIT").write_text("MIT\n")
        (self.rust / "inventory.json").write_text(json.dumps({"target": "x86_64-pc-windows-msvc", "packages": [{}]}))
        (base / "hgs.exe").write_bytes(b"tested cli")
        (base / "hgs-tray.exe").write_bytes(b"tested gui")

    def package(self, output, **overrides):
        arguments = {"--cli": self.directory / "hgs.exe", "--gui": self.directory / "hgs-tray.exe",
                     "--output": output, "--qt-root": self.qt, "--windeployqt": self.windeployqt,
                     "--crt-dir": self.crt, "--rust-licenses": self.rust, **overrides}
        command = [sys.executable, str(ROOT / "scripts/package-windows.py")]
        for key, value in arguments.items():
            command += [key, str(value)]
        environment = {key: value for key, value in os.environ.items() if not key.startswith(("QT_", "HGS_"))}
        environment["SOURCE_DATE_EPOCH"] = "1760000000"
        return subprocess.run(command, cwd=self.directory, env=environment, capture_output=True, text=True)

    def check(self, path, *arguments):
        return subprocess.run([sys.executable, str(ROOT / "scripts/check-windows-portable.py"), str(path), *arguments],
                              capture_output=True, text=True)

    def test_multimedia_notices_are_required(self):
        (self.qt / "sbom" / f"qtmultimedia-{QT_VERSION}.spdx").unlink()
        output = self.directory / "missing-multimedia.zip"
        result = self.package(output)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("lacks its SBOM notices", result.stderr)
        self.assertFalse(output.exists())

    def test_bundle_holds_tested_binaries_runtime_notices_and_provenance(self):
        output = self.directory / "out" / "zerus-windows-x86_64.zip"
        result = self.package(output)
        self.assertEqual(result.returncode, 0, result.stderr)
        head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
        info = portable.inspect(output)
        self.assertEqual(info, {**manifest(), "source_commit": head})
        arguments = json.loads((self.directory / "arguments.json").read_text())
        self.assertEqual(arguments[:-1], ["--release", "--no-compiler-runtime", "--no-opengl-sw",
                                          "--no-system-d3d-compiler", "--no-system-dxc-compiler",
                                          "--include-plugins", "qsvg,qsvgicon",
                                          "--skip-plugin-types", "qmltooling,generic",
                                          "--exclude-plugins", "qtposition_nmea"])
        self.assertTrue(arguments[-1].endswith("hgs-tray.exe"))
        with zipfile.ZipFile(output) as archive:
            names = [entry.filename for entry in archive.infolist()]
            self.assertEqual(names, sorted(names))
            self.assertTrue(all(name.startswith(portable.TOP + "/") for name in names))
            self.assertEqual(archive.read(f"{portable.TOP}/hgs.exe"), b"tested cli")
            self.assertEqual(archive.read(f"{portable.TOP}/hgs-tray.exe"), b"tested gui")
            self.assertEqual(archive.read(f"{portable.TOP}/concrt140.dll"), b"concrt140.dll")
            self.assertEqual(archive.read(f"{portable.TOP}/LICENSE"), (ROOT / "LICENSE").read_bytes())
            self.assertEqual(archive.read(f"{portable.TOP}/licenses/qt/LGPL-3.0-only.txt"),
                             (ROOT / "packaging/windows/licenses/LGPL-3.0-only.txt").read_bytes())
            for name in portable.QT_LICENSE_TEXTS:
                self.assertIn(f"{portable.TOP}/licenses/qt/{name}.txt", names)
            self.assertFalse([name for name in names if "icu" in name.lower() and name.endswith(".dll")])
            self.assertFalse([name for name in names if portable.FORBIDDEN.search(name)])
            self.assertNotIn(f"{portable.TOP}/licenses/qt/README.md", names)
            self.assertIn(f"{portable.TOP}/licenses/qt/sbom/qtmultimedia-{QT_VERSION}.spdx", names)
            self.assertNotIn(f"{portable.TOP}/licenses/qt/sbom/qtbase-{QT_VERSION}.source.spdx", names)
            self.assertIn(f"{portable.TOP}/licenses/rust/serde-1.0.0/LICENSE-MIT", names)
            readme = archive.read(f"{portable.TOP}/README.txt").decode()
            self.assertIn(f"Zerus {VERSION} for Windows", readme)
            self.assertIn(head, readme)
            self.assertNotRegex(readme, r"@[A-Z_]+@")
            self.assertNotRegex(readme, r"[^\r]\n")
        # Equal inputs give equal bytes, so a rebuilt zip can be compared.
        again = self.directory / "again.zip"
        self.assertEqual(self.package(again).returncode, 0)
        self.assertEqual(again.read_bytes(), output.read_bytes())
        report = self.check(output)
        self.assertEqual(report.returncode, 0, report.stderr)
        self.assertIn("files", report.stdout)
        self.assertNotEqual(self.check(output, "--commit", "b" * 40).returncode, 0)

    def test_a_license_without_its_text_leaves_no_bundle(self):
        sbom = self.qt / "sbom" / f"qtsvg-{QT_VERSION}.spdx"
        sbom.write_text("SPDXVersion: SPDX-2.3\nPackageName: Bundled\nPackageLicenseConcluded: MIT AND Made-Up-1.0\n"
                        "PackageLicenseDeclared: LicenseRef-Undefined\n")
        output = self.directory / "zerus-windows-x86_64.zip"
        result = self.package(output)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f"qtsvg-{QT_VERSION}.spdx: LicenseRef-Undefined, qtsvg-{QT_VERSION}.spdx: Made-Up-1.0",
                      result.stderr)
        self.assertEqual(sorted(path.name for path in self.directory.glob("*.zip*")), [])

    def test_a_deployed_direct3d_compiler_leaves_no_bundle(self):
        # A windeployqt that ignores --no-system-d3d-compiler fails packaging.
        script = self.windeployqt.read_text()
        self.windeployqt.write_text(script.replace("if '--no-system-d3d-compiler' not in sys.argv:", "if True:"))
        output = self.directory / "zerus-windows-x86_64.zip"
        result = self.package(output)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(f"must not contain {portable.TOP}/D3Dcompiler_47.dll", result.stderr)
        self.assertEqual(sorted(path.name for path in self.directory.glob("*.zip*")), [])

    def test_missing_runtime_or_qt_notices_leave_no_bundle(self):
        (self.crt / "vcruntime140_1.dll").unlink()
        output = self.directory / "zerus-windows-x86_64.zip"
        result = self.package(output)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("vcruntime140_1.dll", result.stderr)
        (self.crt / "vcruntime140_1.dll").write_bytes(b"restored")
        (self.qt / "sbom" / f"qtbase-{QT_VERSION}.spdx").unlink()
        result = self.package(output)
        self.assertIn("SBOM", result.stderr)
        self.assertEqual(sorted(path.name for path in self.directory.glob("*.zip*")), [])


if __name__ == "__main__":
    unittest.main()
