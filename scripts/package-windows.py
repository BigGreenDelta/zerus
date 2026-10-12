#!/usr/bin/env python3
"""Bundle the tested hgs.exe and hgs-tray.exe into the Windows portable zip.

windeployqt adds the Qt and Qt WebEngine runtime, vswhere locates the app-local
MSVC runtime, and the notices that make the bundle redistributable are copied
next to them. Nothing is compiled here: the executables are the exact files
the Windows CI job has just tested.
"""
import argparse
import datetime
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import tomllib
import zipfile

from release_common import require
from windows_portable import TOP, inspect, missing_license_texts

ROOT = Path(__file__).resolve().parents[1]
CRT_REQUIRED = ("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll")


def qt_version(qt_root):
    """The exact Qt release, read from its CMake package version file."""
    for name in ("Qt6CoreConfigVersionImpl.cmake", "Qt6CoreConfigVersion.cmake"):
        path = qt_root / "lib" / "cmake" / "Qt6Core" / name
        if path.is_file():
            match = re.search(r'set\(PACKAGE_VERSION "(\d+\.\d+\.\d+)"\)', path.read_text(encoding="utf-8"))
            if match:
                return match[1]
    raise SystemExit(f"Cannot determine the Qt version under {qt_root}.")


def visual_studio():
    """The newest Visual Studio with the x64 C++ tools, as vswhere reports it."""
    program_files = os.environ.get("ProgramFiles(x86)") or os.environ.get("ProgramFiles") or r"C:\Program Files (x86)"
    vswhere = Path(program_files) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    require(vswhere.is_file(), f"vswhere is missing: {vswhere}")
    output = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
                                      "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                      "-property", "installationPath", "-utf8"])
    install = output.decode("utf-8").strip()
    require(install and Path(install).is_dir(), "vswhere found no Visual Studio with the x64 C++ tools.")
    return Path(install)


def crt_directory(install):
    """App-local x64 CRT: the default redistributable version, never a hard-coded one."""
    redist = install / "VC" / "Redist" / "MSVC"
    default = install / "VC" / "Auxiliary" / "Build" / "Microsoft.VCRedistVersion.default.txt"
    if default.is_file():
        versions = [default.read_text(encoding="utf-8").strip()]
    else:
        versions = sorted((path.name for path in redist.iterdir() if re.fullmatch(r"\d+(?:\.\d+)+", path.name)),
                          key=lambda value: tuple(map(int, value.split("."))), reverse=True)[:1]
    require(versions, f"No MSVC redistributable under {redist}.")
    # Microsoft.VC143.CRT for Visual Studio 2022, Microsoft.VC145.CRT for 2026.
    folders = sorted((redist / versions[0] / "x64").glob("Microsoft.VC14*.CRT"))
    require(len(folders) == 1, f"Expected one x64 CRT folder under {redist / versions[0]}.")
    return folders[0]


def copy(source, destination):
    require(source.is_file() and not source.is_symlink(), f"Missing bundle input: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)


def source_commit():
    return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()


def source_time():
    """Entry time: SOURCE_DATE_EPOCH or the commit time, so rebuilt zips stay comparable."""
    value = os.environ.get("SOURCE_DATE_EPOCH") or subprocess.check_output(
        ["git", "log", "-1", "--format=%ct", "HEAD"], cwd=ROOT, text=True).strip()
    moment = datetime.datetime.fromtimestamp(int(value), datetime.timezone.utc)
    return max(moment.timetuple()[:6], (1980, 1, 1, 0, 0, 0))


def write_zip(stage, output, date_time):
    """Sorted, '/'-separated regular files under one top-level folder."""
    files = []
    for path in sorted(stage.rglob("*")):
        require(not path.is_symlink(), f"Symlinks cannot be bundled: {path}")
        if path.is_file():
            files.append(path)
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in files:
            entry = zipfile.ZipInfo(f"{TOP}/{path.relative_to(stage).as_posix()}", date_time=date_time)
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.create_system = 3
            entry.external_attr = (stat.S_IFREG | 0o644) << 16
            entry.file_size = path.stat().st_size
            with path.open("rb") as source, archive.open(entry, "w") as target:
                shutil.copyfileobj(source, target, 1024 * 1024)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cli", type=Path, required=True, help="tested hgs.exe")
    parser.add_argument("--gui", type=Path, required=True, help="tested hgs-tray.exe")
    parser.add_argument("--output", type=Path, required=True, help="zip to write")
    parser.add_argument("--qt-root", type=Path, default=os.environ.get("QT_ROOT_DIR"),
                        help="Qt installation, e.g. .../6.10.3/msvc2022_64 [default: $QT_ROOT_DIR]")
    parser.add_argument("--windeployqt", type=Path, help="[default: <qt-root>/bin/windeployqt.exe]")
    parser.add_argument("--crt-dir", type=Path, help="Microsoft.VC14*.CRT folder [default: found with vswhere]")
    parser.add_argument("--rust-licenses", type=Path,
                        help="notices from scripts/collect-licenses.py [default: collected for this host target]")
    args = parser.parse_args()
    require(args.qt_root is not None and args.qt_root.is_dir(), "Pass --qt-root or set QT_ROOT_DIR.")
    require(args.output.suffix == ".zip", "The output must be a .zip file.")
    qt_root = args.qt_root.resolve()
    windeployqt = args.windeployqt or qt_root / "bin" / "windeployqt.exe"
    crt = args.crt_dir or crt_directory(visual_studio())
    version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    cli_version = tomllib.loads((ROOT / "Cargo.toml").read_text(encoding="utf-8"))["package"]["version"]
    manifest = {"product": "Zerus", "version": version, "cli_version": cli_version, "platform": "Windows",
                "architecture": "x86_64", "qt_version": qt_version(qt_root), "native_agents_bundled": False,
                "source_commit": source_commit()}
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="zerus-windows-") as temporary:
        stage = Path(temporary) / TOP
        copy(args.cli, stage / "hgs.exe")
        copy(args.gui, stage / "hgs-tray.exe")
        # Qt libraries, plugins, translations and the Qt WebEngine process with its
        # resources. The SVG image and icon plugins are loaded at run time only,
        # so windeployqt cannot infer them from imports. QML debugging and TUIO
        # touch plugins are not used; the NMEA position plugin needs Qt Serial
        # Port, which is not shipped. The compiler runtime is copied as DLLs
        # below instead of windeployqt's vc_redist installer. Qt Quick and Qt
        # WebEngine render through Direct3D, so the Mesa software OpenGL
        # fallback, which the bundled notices do not cover, is left out.
        # Microsoft's Direct3D shader compilers stay out as well: Windows 10 1903
        # and later include D3DCompiler_47.dll, and the notices do not cover the
        # Windows SDK's DXC (dxcompiler.dll, dxil.dll) either.
        subprocess.run([str(windeployqt), "--release", "--no-compiler-runtime", "--no-opengl-sw",
                        "--no-system-d3d-compiler", "--no-system-dxc-compiler",
                        "--include-plugins", "qsvg,qsvgicon", "--skip-plugin-types", "qmltooling,generic",
                        "--exclude-plugins", "qtposition_nmea", str(stage / "hgs-tray.exe")], check=True)
        # windeployqt also copies the ICU that Qt6Core imports from PATH: the build
        # machine's System32 copy, which is a forwarder to icu.dll on current
        # Windows. Qt6Core uses the system ICU instead; never redistribute it.
        for path in sorted(stage.glob("*.dll")):
            if path.name.lower().startswith("icu"):
                print(f"Leaving out {path.name}, a Windows system library.")
                path.unlink()
        for dll in sorted(crt.glob("*.dll")):
            copy(dll, stage / dll.name)
        missing = [name for name in CRT_REQUIRED if not (stage / name).is_file()]
        require(not missing, f"The MSVC runtime under {crt} lacks: {', '.join(missing)}")
        for name in ("LICENSE", "THIRD_PARTY_NOTICES.md"):
            copy(ROOT / name, stage / name)
        licenses = stage / "licenses"
        copy(ROOT / "tray/vendor/libvterm/LICENSE", licenses / "libvterm-LICENSE")
        copy(ROOT / "tray/vendor/md4c/LICENSE.md", licenses / "md4c-LICENSE")
        copy(ROOT / "docs/licenses/codex-LICENSE.txt", licenses / "codex-LICENSE.txt")
        if args.rust_licenses:
            require((args.rust_licenses / "inventory.json").is_file(), "Collect dependency license texts first.")
            shutil.copytree(args.rust_licenses, licenses / "rust")
        else:
            subprocess.run([sys.executable, str(ROOT / "scripts/collect-licenses.py"), str(licenses / "rust")],
                           cwd=ROOT, check=True)
        for text in sorted((ROOT / "packaging/windows/licenses").glob("*.txt")):
            copy(text, licenses / "qt" / text.name)
        # Qt ships SPDX documents per module; Qt WebEngine's includes the notices
        # of the Chromium components it contains. Source SBOMs are not needed.
        sboms = sorted(path for path in (qt_root / "sbom").glob("*.spdx") if not path.name.endswith(".source.spdx"))
        names = {path.name for path in sboms}
        require(f"qtbase-{manifest['qt_version']}.spdx" in names and
                f"qtmultimedia-{manifest['qt_version']}.spdx" in names and
                f"qtwebengine-{manifest['qt_version']}.spdx" in names and
                any(name.startswith("qtwebengine-chromium-") for name in names),
                "The Qt installation lacks its SBOM notices; review Qt licensing before distribution.")
        for sbom in sboms:
            copy(sbom, licenses / "qt" / "sbom" / sbom.name)
        # The documents name licenses by identifier; each needs its text here.
        texts = {path.stem for path in (licenses / "qt").glob("*.txt")}
        missing = missing_license_texts({sbom.name: sbom.read_text(encoding="utf-8") for sbom in sboms}, texts)
        require(not missing, "Qt SPDX documents name licenses without a text in packaging/windows/licenses: "
                + ", ".join(missing))
        readme = (ROOT / "packaging/windows/README.txt").read_text(encoding="utf-8")
        for key in ("version", "cli_version", "qt_version", "source_commit"):
            readme = readme.replace(f"@{key.upper()}@", manifest[key])
        require(not re.search(r"@[A-Z_]+@", readme), "Unreplaced placeholder in the Windows README.")
        # Notepad and other Windows tools expect CRLF in a plain-text README.
        (stage / "README.txt").write_bytes(readme.replace("\n", "\r\n").encode("utf-8"))
        (stage / "build-info.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        partial = output.with_name(output.name + ".partial")
        partial.unlink(missing_ok=True)
        try:
            write_zip(stage, partial, source_time())
            # The same layout and manifest checks the nightly publisher repeats.
            inspect(partial)
        except BaseException:
            partial.unlink(missing_ok=True)
            raise
    os.replace(partial, output)
    print(f"Wrote {output} ({output.stat().st_size} bytes) for Zerus {version}, hgs {cli_version}, "
          f"Qt {manifest['qt_version']}, commit {manifest['source_commit']}.")


if __name__ == "__main__":
    try:
        main()
    except ValueError as error:
        raise SystemExit(f"package-windows: {error}") from None
