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
# load; a weak (LC_LOAD_WEAK_DYLIB) one may be absent, but is checked when present.
REQUIRED_DYLIBS = {0xC, 0x8000001F, 0x80000023}
LC_LOAD_WEAK_DYLIB = 0x80000018
LC_RPATH = 0x8000001C
# dyld provides these from the system; any other absolute name must be inside the prefix.
SYSTEM_ROOTS = ("/usr/lib/", "/System/Library/")
# The pinned build tools need 16 image visits (8 images, 2 architectures); a far
# larger graph stops the installation.
VISIT_LIMIT = 1_000
ARCHITECTURES = {(0x0100000C, 0): "arm64", (0x01000007, 3): "x86_64"}


def validate_pin(pin):
    return qt_archives.validate_pin(pin, REPOSITORY)


def load_commands(data):
    """{architecture: (dylibs, run paths)} of a 64-bit thin or universal Mach-O image.

    An architecture is a CPU type and subtype, and each dylib a (name, weak) pair. Every
    count, offset and size must lie inside the file, the slice and its command table;
    anything else, including an empty or 64-bit universal header, is refused."""
    try:
        if data[:4] == b"\xca\xfe\xba\xbe":
            count = struct.unpack_from(">I", data, 4)[0]
            require(0 < count <= 16 and 8 + 20 * count <= len(data), "Malformed universal Mach-O header.")
            slices = []
            for index in range(count):
                cputype, cpusubtype, offset, size, _ = struct.unpack_from(">5I", data, 8 + 20 * index)
                require(8 + 20 * count <= offset and offset + size <= len(data), "Mach-O slice outside the file.")
                slices.append(((cputype, cpusubtype & 0xFFFFFF), offset, size))
        else:
            slices = [(None, 0, len(data))]
        images = {}
        for declared, offset, size in slices:
            require(size >= 32 and data[offset:offset + 4] == b"\xcf\xfa\xed\xfe", "Not a 64-bit Mach-O image.")
            cputype, cpusubtype, _, count, table = struct.unpack_from("<5I", data, offset + 4)
            architecture = (cputype, cpusubtype & 0xFFFFFF)
            require(declared in (None, architecture) and architecture not in images,
                    "Inconsistent Mach-O architectures.")
            cursor, end, dylibs, rpaths = offset + 32, offset + 32 + table, [], []
            require(end <= offset + size, "Mach-O load commands outside the image.")
            for _ in range(count):
                require(cursor + 8 <= end, "Mach-O load commands outside their table.")
                command, length = struct.unpack_from("<II", data, cursor)
                require(length >= 8 and length % 8 == 0 and cursor + length <= end, "Malformed Mach-O load command.")
                if command in REQUIRED_DYLIBS or command in (LC_LOAD_WEAK_DYLIB, LC_RPATH):
                    start = struct.unpack_from("<I", data, cursor + 8)[0]
                    raw = data[cursor + start:cursor + length]
                    require((12 if command == LC_RPATH else 24) <= start < length and b"\0" in raw,
                            "Malformed Mach-O load command name.")
                    name = raw.split(b"\0", 1)[0].decode()
                    if command == LC_RPATH:
                        rpaths.append(name)
                    else:
                        dylibs.append((name, command == LC_LOAD_WEAK_DYLIB))
                cursor += length
            require(cursor == end, "Mach-O load commands do not fill their table.")
            images[architecture] = (dylibs, rpaths)
        return images
    except (struct.error, UnicodeDecodeError) as error:
        raise ValueError(f"Malformed Mach-O image: {error}") from error


def check_tools(prefix, tools):
    """Find every image the tools load, for each of their architectures, without running any of them.

    As dyld does, an @rpath name is searched in the run paths of the loading image, then
    in those of the images that loaded it, and the first existing file is the one loaded;
    @loader_path is the directory of the loading image. That file, and the file of any
    absolute name outside the system roots, must be inside the prefix and provide the
    loading architecture. A weak image may be absent but is checked when present. Other
    load names and run paths are refused."""
    root = os.path.realpath(prefix)

    def inside(path):
        return os.path.commonpath([root, os.path.realpath(path)]) == root

    pending, seen, images = [], set(), {}
    for tool in tools:
        image = prefix / tool
        require(image.is_file() and inside(image), f"The Qt prefix has no {tool}.")
        images[image] = load_commands(image.read_bytes())
        pending += [(tool, architecture, image, ()) for architecture in images[image]]
    while pending:
        tool, architecture, image, inherited = pending.pop()
        if (architecture, image, inherited) in seen:
            continue
        seen.add((architecture, image, inherited))
        require(len(seen) <= VISIT_LIMIT, f"{tool}: more than {VISIT_LIMIT} images to check.")
        if image not in images:
            images[image] = load_commands(image.read_bytes())
        name = ARCHITECTURES.get(architecture, f"CPU {architecture[0]:#x}/{architecture[1]:#x}")
        require(architecture in images[image], f"{tool}: {image.name} has no {name} code.")
        dylibs, rpaths = images[image][architecture]
        own = []
        for rpath in rpaths:
            require(rpath.startswith(("@loader_path/", "/")), f"{tool}: {image.name} has the run path {rpath}.")
            own.append(os.path.normpath(os.path.join(image.parent, rpath[len("@loader_path/"):])
                                        if rpath.startswith("@loader_path/") else rpath))
        # A run path already searched earlier changes nothing, so cycles end.
        paths = tuple(dict.fromkeys(own + list(inherited)))
        for dylib, weak in dylibs:
            if dylib.startswith("@rpath/"):
                candidates = [Path(os.path.normpath(os.path.join(path, dylib[len("@rpath/"):]))) for path in paths]
            elif dylib.startswith(SYSTEM_ROOTS) and os.path.normpath(dylib) == dylib:
                continue
            else:
                require(dylib.startswith("/"), f"{tool}: {image.name} loads {dylib}, which is not supported.")
                candidates = [Path(dylib)]
            found = next((path for path in candidates if path.is_file()), None)
            if found is None and weak:
                continue
            require(found is not None, f"{tool}: {image.name} loads {dylib}, which the Qt prefix does not provide.")
            require(inside(found), f"{tool}: {image.name} loads {dylib} from {found}, outside the Qt prefix.")
            pending.append((tool, architecture, found, paths))


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
