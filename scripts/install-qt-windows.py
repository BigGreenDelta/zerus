#!/usr/bin/env python3
"""Install the pinned official Qt for the Windows build from verified archives.

packaging/windows/qt.json pins the archives of the Qt online repository that
aqtinstall installed for this build before: the base package of the MSVC
architecture with all its archives and Qt Multimedia, each by path, size,
SHA-256 and the directory its Extract operation names. Every archive is
downloaded (mirrors may serve the bytes), verified and listed with 7-Zip before
any is extracted (scripts/qt_archives.py), into the layout aqtinstall made:
<output>/<version>/<arch>. Like aqtinstall, the installer then writes bin/qt.conf;
the official Qt is relocatable, so nothing else needs patching for CMake and
windeployqt.

`--check` confirms that an installation, such as a restored cache, came from this
pin. `--lock` regenerates the archive list from the official Updates.xml, its
.sha256 files and the archives themselves for a deliberate Qt update.
"""
import argparse
import json
from pathlib import Path
import re
import shutil
import sys

import qt_archives
from qt_archives import require

ROOT = Path(__file__).resolve().parents[1]
PIN = ROOT / "packaging/windows/qt.json"
REPOSITORY = qt_archives.WINDOWS_REPOSITORY
# What aqtinstall wrote after extracting a desktop Qt: the prefix relative to bin.
QT_CONF = "[Paths]\nPrefix=..\n"


def validate_pin(pin):
    return qt_archives.validate_pin(pin, REPOSITORY, targets=True)


def cmake_version(prefix):
    """The Qt release of an installation, from its Qt6Core CMake package version file."""
    for name in ("Qt6CoreConfigVersionImpl.cmake", "Qt6CoreConfigVersion.cmake"):
        path = prefix / "lib" / "cmake" / "Qt6Core" / name
        if path.is_file():
            match = re.search(r'set\(PACKAGE_VERSION "(\d+\.\d+\.\d+)"\)', path.read_text(encoding="utf-8"))
            if match:
                return match[1]
    raise ValueError(f"No Qt6Core CMake version file under {prefix}.")


def check(output, pin):
    """The prefix of an installation of exactly this pin, with the tools the build uses."""
    validate_pin(pin)
    prefix = output / pin["prefix"]
    qt_archives.check_manifest(prefix, pin)
    actual = cmake_version(prefix)
    require(actual == pin["qt_version"], f"{prefix} holds Qt {actual}, expected {pin['qt_version']}.")
    for name in ("bin/windeployqt.exe", "bin/qtpaths.exe", "bin/Qt6Core.dll", "bin/Qt6Multimedia.dll"):
        require((prefix / name).is_file(), f"The Qt installation lacks {name}: {prefix}")
    return prefix


def install(pin, output, tool, base=REPOSITORY):
    validate_pin(pin)
    require(not output.exists(), f"Use a fresh Qt directory: {output}")
    downloads = output.with_name(output.name + ".downloads")
    # Verify and list every archive before extracting any of them.
    staged = qt_archives.stage(pin, base, downloads, tool)
    output.mkdir(parents=True)
    for archive, target in staged:
        destination = output / archive["target"]
        destination.mkdir(parents=True, exist_ok=True)
        tool.extract(target, destination)
        print(f"Extracted {target.name} into {destination}", flush=True)
    qt_archives.check_tree(output)
    prefix = output / pin["prefix"]
    (prefix / "bin" / "qt.conf").write_text(QT_CONF)
    qt_archives.write_manifest(prefix, pin)
    check(output, pin)
    qt_archives.finish(staged, downloads)
    return prefix


def seven_zip(value):
    """7-Zip as given, or the 7z on PATH (the windows-2025 runner image installs 7-Zip)."""
    program = value or shutil.which("7z")
    require(program, "7-Zip is required: put 7z on PATH or pass --seven-zip.")
    return qt_archives.SevenZip(program)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pin", type=Path, default=PIN)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--output", type=Path, help="fresh directory to install Qt into, like aqt --outputdir")
    action.add_argument("--check", type=Path, metavar="OUTPUT", help="confirm an installation of this pin")
    action.add_argument("--lock", action="store_true", help="refresh the archives from the official repository")
    parser.add_argument("--seven-zip", help="7-Zip executable [default: 7z on PATH]")
    args = parser.parse_args()
    pin = json.loads(args.pin.read_text())
    if args.lock:
        args.pin.write_text(json.dumps(qt_archives.lock(pin, REPOSITORY, targets=True), indent=2) + "\n")
        print(f"Locked Qt {pin['qt_version']} archives in {args.pin}.")
    elif args.check:
        prefix = check(args.check.resolve(), pin)
        print(f"{prefix} holds the pinned Qt {pin['qt_version']}.")
    else:
        tool = seven_zip(args.seven_zip)
        print(f"Extracting with {tool.describe()}", flush=True)
        prefix = install(pin, args.output.resolve(), tool)
        print(f"Installed verified Qt {pin['qt_version']} into {prefix}.")


if __name__ == "__main__":
    try:
        main()
    except ValueError as error:
        sys.exit(f"install-qt-windows: {error}")
