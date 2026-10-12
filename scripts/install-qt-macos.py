#!/usr/bin/env python3
"""Install the pinned official Qt for the macOS bundle from verified archives.

Every archive is downloaded from the Qt online repository (mirrors may serve the
bytes), checked against the size and SHA-256 recorded in packaging/macos/qt.json
and only then extracted with bsdtar. `--lock` regenerates that list from the
official repository metadata and .sha256 files for a deliberate Qt update.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import urllib.request
import xml.etree.ElementTree as ElementTree

ROOT = Path(__file__).resolve().parents[1]
PIN = ROOT / "packaging/macos/qt.json"
REPOSITORY = "https://download.qt.io/online/qtsdkrepository/mac_x64"
PATH = re.compile(r"[A-Za-z0-9][A-Za-z0-9._/+-]{0,300}")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def safe_path(value):
    path = PurePosixPath(value)
    return bool(PATH.fullmatch(value)) and not path.is_absolute() and ".." not in path.parts


def validate_pin(pin):
    require(isinstance(pin, dict) and set(pin) == {"qt_version", "repository", "packages", "archives"},
            "Unexpected Qt pin fields.")
    require(re.fullmatch(r"\d+\.\d+\.\d+", str(pin["qt_version"])), "Invalid Qt version.")
    require(pin["repository"] == REPOSITORY, "Qt must come from the official Qt online repository.")
    require(isinstance(pin["archives"], list) and pin["archives"], "No Qt archives are pinned.")
    seen = set()
    for archive in pin["archives"]:
        require(isinstance(archive, dict) and set(archive) == {"path", "size", "sha256"}, "Unexpected archive fields.")
        require(isinstance(archive["path"], str) and safe_path(archive["path"]), "Unsafe archive path.")
        require(archive["path"] not in seen, "Duplicate Qt archive.")
        require(type(archive["size"]) is int and 0 < archive["size"] < 2_000_000_000, "Invalid archive size.")
        require(isinstance(archive["sha256"], str) and re.fullmatch(r"[0-9a-f]{64}", archive["sha256"]),
                "Invalid archive digest.")
        seen.add(archive["path"])
    for package in pin["packages"]:
        require(set(package) == {"directory", "name", "archives"} and safe_path(package["directory"]),
                "Unexpected Qt package specification.")
    return pin


def archives_from_updates(text, name, modules):
    """Return '<version><archive>' file names for the requested modules of one package."""
    matches = [item for item in ElementTree.fromstring(text).findall("PackageUpdate") if item.findtext("Name") == name]
    require(len(matches) == 1, f"Qt package not found exactly once: {name}")
    version = matches[0].findtext("Version", "")
    archives = [value.strip() for value in matches[0].findtext("DownloadableArchives", "").split(",") if value.strip()]
    selected = []
    for module in modules:
        found = [archive for archive in archives if archive.startswith(module + "-")]
        require(len(found) == 1, f"Qt archive not found exactly once: {module} in {name}")
        selected.append(version + found[0])
    return selected


def check_members(names):
    for name in names:
        path = PurePosixPath(name)
        require(bool(name) and not path.is_absolute() and ".." not in path.parts, f"Unsafe Qt archive member: {name!r}")


def download(base, archive, target):
    url = base.rstrip("/") + "/" + archive["path"]
    target.parent.mkdir(parents=True, exist_ok=True)
    stage = target.with_name(target.name + ".partial")
    digest, size = hashlib.sha256(), 0
    try:
        with urllib.request.urlopen(url, timeout=60) as response, stage.open("wb") as output:
            while block := response.read(1 << 20):
                size += len(block)
                require(size <= archive["size"], f"Qt archive is larger than pinned: {archive['path']}")
                digest.update(block)
                output.write(block)
        require(size == archive["size"], f"Qt archive size differs: {archive['path']}")
        require(digest.hexdigest() == archive["sha256"], f"Qt archive checksum differs: {archive['path']}")
        stage.replace(target)
    finally:
        stage.unlink(missing_ok=True)


def install(pin, output, base=REPOSITORY, check_version=True):
    validate_pin(pin)
    require(not output.exists(), f"Use a fresh Qt prefix: {output}")
    downloads = output.with_name(output.name + ".downloads")
    targets = []
    # Verify every archive before extracting any of them.
    for archive in pin["archives"]:
        target = downloads / PurePosixPath(archive["path"]).name
        download(base, archive, target)
        listing = subprocess.run(["bsdtar", "-tf", str(target)], check=True, capture_output=True, text=True).stdout
        check_members(listing.splitlines())
        targets.append(target)
    output.mkdir(parents=True)
    for target in targets:
        subprocess.run(["bsdtar", "-xf", str(target), "-C", str(output)], check=True)
    if check_version:
        actual = subprocess.check_output([str(output / "bin/qmake"), "-query", "QT_VERSION"], text=True).strip()
        require(actual == pin["qt_version"], f"Installed Qt reports {actual}, expected {pin['qt_version']}")
    (output / "zerus-qt.json").write_text(json.dumps({"qt_version": pin["qt_version"],
                                                     "archives": pin["archives"]}, indent=2) + "\n")
    for target in targets:
        target.unlink()
    downloads.rmdir()


def fetch_text(url):
    with urllib.request.urlopen(url, timeout=60) as response:
        return response.read(16 << 20).decode()


def lock(pin):
    archives = []
    for package in pin["packages"]:
        directory = f"{REPOSITORY}/{package['directory']}"
        for name in archives_from_updates(fetch_text(directory + "/Updates.xml"), package["name"], package["archives"]):
            path = f"{package['directory']}/{package['name']}/{name}"
            digest = fetch_text(f"{REPOSITORY}/{path}.sha256").split()[0]
            request = urllib.request.Request(f"{REPOSITORY}/{path}", method="HEAD")
            with urllib.request.urlopen(request, timeout=60) as response:
                size = int(response.headers["Content-Length"])
            archives.append({"path": path, "size": size, "sha256": digest})
    return validate_pin(dict(pin, archives=archives))


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
