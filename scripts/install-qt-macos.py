#!/usr/bin/env python3
"""Install the pinned official Qt for the macOS bundle from verified archives.

Every archive is downloaded from the Qt online repository (mirrors may serve the
bytes), checked against the size and SHA-256 recorded in packaging/macos/qt.json
and only then extracted with bsdtar (scripts/qt_archives.py). Before any Qt tool
runs, the Mach-O load commands of the build tools must name only Qt frameworks
the prefix provides. `--lock` regenerates that list from the official repository
metadata, its .sha256 files and the archives themselves for a deliberate Qt update.
"""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess

import qt_archives
from qt_archives import archives_from_updates, check_members, download, require  # noqa: F401

ROOT = Path(__file__).resolve().parents[1]
PIN = ROOT / "packaging/macos/qt.json"
REPOSITORY = qt_archives.MACOS_REPOSITORY
# The prefix tools that this installer, the tray build and scripts/package-macos.py run.
BUILD_TOOLS = ("bin/qmake", "bin/lrelease", "bin/macdeployqt", "libexec/moc", "libexec/rcc")
# LC_LOAD_DYLIB, LC_REEXPORT_DYLIB and LC_LOAD_UPWARD_DYLIB name images dyld must
# load; a weak one may be absent.
REQUIRED_DYLIBS = {0xC, 0x8000001F, 0x80000023}
LC_RPATH = 0x8000001C


def validate_pin(pin):
    return qt_archives.validate_pin(pin, REPOSITORY)


def load_commands(data):
    """(required dylib names, run paths) of each slice of a 64-bit thin or universal Mach-O image."""
    offsets = [0]
    if data[:4] == b"\xca\xfe\xba\xbe":
        offsets = [struct.unpack_from(">5I", data, 8 + 20 * index)[2]
                   for index in range(struct.unpack_from(">I", data, 4)[0])]
    slices = []
    for offset in offsets:
        require(data[offset:offset + 4] == b"\xcf\xfa\xed\xfe", "Not a 64-bit Mach-O image.")
        cursor, dylibs, rpaths = offset + 32, [], []
        for _ in range(struct.unpack_from("<I", data, offset + 16)[0]):
            command, size = struct.unpack_from("<II", data, cursor)
            if command in REQUIRED_DYLIBS or command == LC_RPATH:
                start = struct.unpack_from("<I", data, cursor + 8)[0]
                name = data[cursor + start:cursor + size].split(b"\0", 1)[0].decode()
                (rpaths if command == LC_RPATH else dylibs).append(name)
            cursor += size
        slices.append((dylibs, rpaths))
    return slices


def check_tools(prefix, tools):
    """Find every Qt image the tools load inside the prefix, without running any of them.

    As dyld does, an @rpath name is searched in the run paths of the loading image,
    then in those of the images that loaded it, and @loader_path is the directory of
    the loading image. The official Qt uses no other @ names; an absolute name is a
    system library."""
    root = os.path.realpath(prefix)
    pending, seen = [(tool, prefix / tool, ()) for tool in tools], set()
    while pending:
        tool, image, inherited = pending.pop()
        if (image, inherited) in seen:
            continue
        seen.add((image, inherited))
        require(image.is_file(), f"The Qt prefix has no {tool}.")
        for dylibs, rpaths in load_commands(image.read_bytes()):
            paths = tuple(os.path.normpath(image.parent / rpath[len("@loader_path/"):])
                          for rpath in rpaths if rpath.startswith("@loader_path/")) + inherited
            for name in dylibs:
                if not name.startswith("@"):
                    continue
                found = [Path(path, name[len("@rpath/"):]) for path in paths if name.startswith("@rpath/")]
                found = [path for path in found if path.is_file() and
                         os.path.commonpath([root, os.path.realpath(path)]) == root]
                require(found, f"{tool}: {image.name} loads {name}, which the Qt prefix does not provide.")
                pending.append((tool, found[0], paths))


def install(pin, output, base=REPOSITORY, check_version=True):
    validate_pin(pin)
    require(not output.exists(), f"Use a fresh Qt prefix: {output}")
    downloads = output.with_name(output.name + ".downloads")
    tool = qt_archives.Bsdtar()
    # Verify every archive before extracting any of them.
    staged = qt_archives.stage(pin, base, downloads, tool)
    output.mkdir(parents=True)
    for _, target in staged:
        tool.extract(target, output)
    # Framework links stay inside the prefix.
    qt_archives.check_tree(output)
    if check_version:
        check_tools(output, BUILD_TOOLS)
        actual = subprocess.check_output([str(output / "bin/qmake"), "-query", "QT_VERSION"], text=True).strip()
        require(actual == pin["qt_version"], f"Installed Qt reports {actual}, expected {pin['qt_version']}")
    qt_archives.write_manifest(output, pin)
    qt_archives.finish(staged, downloads)


def lock(pin):
    return qt_archives.lock(pin, REPOSITORY)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pin", type=Path, default=PIN)
    parser.add_argument("--output", type=Path, help="fresh Qt prefix to create")
    parser.add_argument("--lock", action="store_true", help="refresh archive digests from the official repository")
    args = parser.parse_args()
    pin = json.loads(args.pin.read_text())
    if args.lock:
        args.pin.write_text(json.dumps(lock(pin), indent=2) + "\n")
        print(f"Locked Qt {pin['qt_version']} archives in {args.pin}.")
        return
    if args.output is None:
        parser.error("--output is required unless --lock is given")
    install(pin, args.output.resolve())
    print(f"Installed verified Qt {pin['qt_version']} into {args.output}.")


if __name__ == "__main__":
    main()
