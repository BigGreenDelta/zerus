#!/usr/bin/env python3
"""Install the pinned official Qt for the macOS bundle from verified archives.

Every archive is downloaded from the Qt online repository (mirrors may serve the
bytes), checked against the size and SHA-256 recorded in packaging/macos/qt.json
and only then extracted with bsdtar (scripts/qt_archives.py). `--lock`
regenerates that list from the official repository metadata, its .sha256 files
and the archives themselves for a deliberate Qt update.
"""
import argparse
import json
from pathlib import Path
import subprocess

import qt_archives
from qt_archives import archives_from_updates, check_members, download, require  # noqa: F401

ROOT = Path(__file__).resolve().parents[1]
PIN = ROOT / "packaging/macos/qt.json"
REPOSITORY = qt_archives.MACOS_REPOSITORY


def validate_pin(pin):
    return qt_archives.validate_pin(pin, REPOSITORY)


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
