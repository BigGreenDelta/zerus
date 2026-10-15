"""Verified official Qt archives for the pinned macOS and Windows installations.

A pin (packaging/macos/qt.json, packaging/windows/qt.json) names its Qt online
repository, the PackageUpdate entries of that repository's Updates.xml and every
archive taken from them by path, size and SHA-256. A Windows pin also names the
Qt prefix and, for each archive, the directory its Extract operation names, as
aqtinstall and the Qt installer use it.

Installation downloads every archive (download.qt.io may redirect to a mirror),
reads at most its pinned size, checks its SHA-256 and lists its members before
any archive is extracted. A member with an absolute, drive, parent, device or
alternate-stream path stops the installation, as does a link member in a 7-Zip
listing and, after extraction, any link that resolves outside the installation.
A trusted system tool extracts the archives: bsdtar on macOS, the runner image's
7-Zip on Windows. Nothing from an archive runs here; only the standard library
is used.
"""
import hashlib
import http.client
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import time
import urllib.request
import xml.etree.ElementTree as ElementTree

MACOS_REPOSITORY = "https://download.qt.io/online/qtsdkrepository/mac_x64"
WINDOWS_REPOSITORY = "https://download.qt.io/online/qtsdkrepository/windows_x86"
PATH = re.compile(r"[A-Za-z0-9][A-Za-z0-9._/+-]{0,300}")
VERSION = re.compile(r"\d+\.\d+\.\d+")
DIGEST = re.compile(r"[0-9a-f]{64}")
PACKAGE = re.compile(r"[a-z0-9][a-z0-9_.]{0,200}")
MODULE = re.compile(r"[A-Za-z0-9][A-Za-z0-9_]{0,100}")
ARCHIVE_LIMIT = 2_000_000_000
MANIFEST = "zerus-qt.json"
# Characters Windows refuses or reinterprets in a member name, and its device names.
RESERVED = re.compile(r'[\x00-\x1f<>:"|?*]')
DEVICE = re.compile(r"(?i)(?:con|prn|aux|nul|com[0-9]|lpt[0-9])(?:\..*)?")
# A `Key = value` line of a 7-Zip technical listing, whose value may be empty, and
# a POSIX mode string in its attributes, such as -rw-r--r-- or lrwxrwxrwx.
FIELD = re.compile(r"([A-Za-z][A-Za-z ]*?) =(?: (.*))?")
MODE = re.compile(r"[-bcdlps][-rwxsStT]{9}")
# Download attempts per archive, the first retry after RETRY_DELAY seconds.
ATTEMPTS = 3
RETRY_DELAY = 10


def require(condition, message):
    if not condition:
        raise ValueError(message)


def safe_path(value):
    path = PurePosixPath(value)
    return bool(PATH.fullmatch(value)) and not path.is_absolute() and ".." not in path.parts


def validate_pin(pin, repository, targets=False):
    """The pin, after checking its source, its packages and every archive field.

    With targets the pin also names its Qt prefix, and each archive the directory
    under the installation that it is extracted into, inside that prefix."""
    fields = {"qt_version", "repository", "packages", "archives"} | ({"prefix"} if targets else set())
    require(isinstance(pin, dict) and set(pin) == fields, "Unexpected Qt pin fields.")
    version = pin["qt_version"]
    require(isinstance(version, str) and VERSION.fullmatch(version), "Invalid Qt version.")
    require(pin["repository"] == repository, "Qt must come from the official Qt online repository.")
    if targets:
        prefix = pin["prefix"]
        require(isinstance(prefix, str) and safe_path(prefix) and PurePosixPath(prefix).parts[0] == version,
                "The Qt prefix must be a relative path under the Qt version.")
    require(isinstance(pin["archives"], list) and pin["archives"], "No Qt archives are pinned.")
    archive_fields = {"path", "size", "sha256"} | ({"target"} if targets else set())
    seen = set()
    for archive in pin["archives"]:
        require(isinstance(archive, dict) and set(archive) == archive_fields, "Unexpected archive fields.")
        path = archive["path"]
        require(isinstance(path, str) and safe_path(path), f"Unsafe archive path: {path!r}")
        require(path not in seen, f"Duplicate Qt archive: {path}")
        require(type(archive["size"]) is int and 0 < archive["size"] < ARCHIVE_LIMIT, f"Invalid archive size: {path}")
        require(isinstance(archive["sha256"], str) and DIGEST.fullmatch(archive["sha256"]),
                f"Invalid archive digest: {path}")
        if targets:
            target = archive["target"]
            require(isinstance(target, str) and safe_path(target) and
                    (target == prefix or target.startswith(prefix + "/")),
                    f"Archive target outside the Qt prefix: {path}")
        seen.add(path)
    # Every archive is one of the named modules of one pinned package, at this version.
    require(isinstance(pin["packages"], list) and pin["packages"], "No Qt packages are pinned.")
    remaining = set(seen)
    for package in pin["packages"]:
        require(isinstance(package, dict) and set(package) == {"directory", "name", "archives"} and
                isinstance(package["directory"], str) and safe_path(package["directory"]) and
                isinstance(package["name"], str) and PACKAGE.fullmatch(package["name"]) and
                isinstance(package["archives"], list) and package["archives"] and
                all(isinstance(module, str) and MODULE.fullmatch(module) for module in package["archives"]),
                "Unexpected Qt package specification.")
        folder = f"{package['directory']}/{package['name']}/"
        for module in package["archives"]:
            name = re.compile(rf"{re.escape(version)}-\d+-\d+{re.escape(module)}-[^/]+")
            found = [path for path in remaining if path.startswith(folder) and name.fullmatch(path[len(folder):])]
            require(len(found) == 1, f"No single pinned {version} archive for {module} in {package['name']}.")
            remaining.discard(found[0])
    require(not remaining, "Qt archives outside the pinned packages: " + ", ".join(sorted(remaining)))
    return pin


def package_archives(text, name, modules):
    """(file, target) for the requested modules of one package of an Updates.xml.

    The file is the package version followed by the archive name; the target is
    the directory under @TargetDir@ that its Extract operation names, or None."""
    matches = [item for item in ElementTree.fromstring(text).findall("PackageUpdate") if item.findtext("Name") == name]
    require(len(matches) == 1, f"Qt package not found exactly once: {name}")
    version = matches[0].findtext("Version", "")
    archives = [value.strip() for value in matches[0].findtext("DownloadableArchives", "").split(",") if value.strip()]
    targets = {}
    for operation in matches[0].findall("Operations/Operation[@name='Extract']"):
        arguments = [(argument.text or "").strip() for argument in operation.findall("Argument")]
        require(len(arguments) == 2 and arguments[0].startswith("@TargetDir@/") and arguments[1] not in targets,
                f"Unexpected Extract operation in {name}: {arguments}")
        targets[arguments[1]] = arguments[0][len("@TargetDir@/"):]
    selected = []
    for module in modules:
        found = [archive for archive in archives if archive.startswith(module + "-")]
        require(len(found) == 1, f"Qt archive not found exactly once: {module} in {name}")
        selected.append((version + found[0], targets.get(found[0])))
    return selected


def archives_from_updates(text, name, modules):
    """Return '<version><archive>' file names for the requested modules of one package."""
    return [file for file, _ in package_archives(text, name, modules)]


def check_member(name):
    """Reject a member name that could land outside, or alias, its extraction directory.

    Both separators count: 7-Zip on Windows lists members with backslashes."""
    parts = re.split(r"[\\/]", name)
    if parts and parts[-1] == "":
        parts.pop()  # A directory entry ends with a separator.
    require(bool(parts) and parts[0] != "", f"Unsafe Qt archive member: {name!r}")
    for part in parts:
        require(part not in ("", "..") and not RESERVED.search(part) and not DEVICE.fullmatch(part) and
                (part == "." or not part.endswith((".", " "))), f"Unsafe Qt archive member: {name!r}")


def check_members(names):
    for name in names:
        check_member(name)


def seven_zip_members(listing):
    """(name, link) for every member of a `7z l -slt` listing of a 7z archive.

    A member is a link when its Windows attributes mark a reparse point, its POSIX
    mode is not that of a file or directory, or 7-Zip reports a link target. An
    anti-item, which deletes a file on extraction, counts as one too."""
    header, separator, body = listing.replace("\r\n", "\n").partition("\n----------\n")
    require(separator and re.search(r"(?m)^Type = 7z$", header), "7-Zip did not list a 7z archive.")
    members = []
    for block in re.split(r"\n[ \t]*\n", body.strip("\n")):
        lines = [line for line in block.split("\n") if line.strip()]
        if not lines:
            continue
        pairs = [FIELD.fullmatch(line) for line in lines]
        require(all(pairs), f"Unexpected 7-Zip listing entry: {lines[:3]}")
        fields = {pair[1]: pair[2] or "" for pair in pairs}
        require("Path" in fields and len(fields) == len(lines), f"Unexpected 7-Zip listing entry: {lines[:3]}")
        tokens = fields.get("Attributes", "").split()
        windows = tokens[0] if tokens and not MODE.fullmatch(tokens[0]) else ""
        modes = [token for token in tokens if MODE.fullmatch(token)]
        link = ("L" in windows or any(mode[0] not in "-d" for mode in modes) or fields.get("Anti") == "+" or
                any(fields.get(key) for key in ("Symbolic Link", "Hard Link", "Link")))
        members.append((fields["Path"], link))
    return members


def output_of(command):
    """Standard output of a command that must succeed, with its error output on failure."""
    result = subprocess.run(command, capture_output=True, encoding="utf-8", errors="strict")
    require(result.returncode == 0, f"{' '.join(command[:2])} failed with exit status {result.returncode}:\n"
            f"{(result.stdout + result.stderr)[-4000:]}")
    return result.stdout


class Bsdtar:
    """The system bsdtar, which itself refuses absolute, parent and symlinked extraction paths."""

    def __init__(self, program="bsdtar"):
        self.program = program

    def members(self, archive):
        names = output_of([self.program, "-tf", str(archive)]).splitlines()
        check_members(names)
        return names

    def extract(self, archive, destination):
        subprocess.run([self.program, "-xf", str(archive), "-C", str(destination)], check=True)


class SevenZip:
    """7-Zip: its technical listing is checked before extraction, which overwrites like aqtinstall did."""

    def __init__(self, program):
        self.program = str(program)

    def describe(self):
        banner = [line.strip() for line in output_of([self.program]).splitlines() if line.strip()]
        return f"{self.program}: {banner[0] if banner else 'no banner'}"

    def members(self, archive):
        members = seven_zip_members(output_of([self.program, "l", "-slt", "-sccUTF-8", "--", str(archive)]))
        check_members(name for name, _ in members)
        links = [name for name, link in members if link]
        require(not links, f"Qt archive {Path(archive).name} contains links: {', '.join(links[:5])}")
        return [name for name, _ in members]

    def extract(self, archive, destination):
        # Errors still reach the log through standard error.
        result = subprocess.run([self.program, "x", "-aoa", "-bd", "-bso0", "-bsp0", "-y", "-sccUTF-8",
                                 f"-o{destination}", "--", str(archive)])
        require(result.returncode == 0, f"7-Zip failed with exit status {result.returncode} on {archive}")


def check_tree(root):
    """Every link and reparse point under root must resolve inside it."""
    base = os.path.realpath(root)
    for directory, folders, files in os.walk(root):
        for name in folders + files:
            path = os.path.join(directory, name)
            reparse = getattr(os.lstat(path), "st_file_attributes", 0) & 0x400  # FILE_ATTRIBUTE_REPARSE_POINT
            if os.path.islink(path) or reparse:
                target = os.path.realpath(path)
                require(os.path.commonpath([base, target]) == base, f"Qt link leaves the installation: {path}")


def download(base, archive, target):
    """Write the archive to target only once its size and SHA-256 match the pin; return the serving URL."""
    url = base.rstrip("/") + "/" + archive["path"]
    target.parent.mkdir(parents=True, exist_ok=True)
    partial = target.with_name(target.name + ".partial")
    digest, size, served = hashlib.sha256(), 0, url
    try:
        with urllib.request.urlopen(url, timeout=60) as response, partial.open("wb") as output:
            served = response.geturl()
            while block := response.read(1 << 20):
                size += len(block)
                require(size <= archive["size"], f"Qt archive is larger than its pinned {archive['size']} bytes: "
                        f"{archive['path']} from {served}")
                digest.update(block)
                output.write(block)
        require(size == archive["size"], f"Qt archive size {size} differs from the pinned {archive['size']}: "
                f"{archive['path']} from {served}")
        require(digest.hexdigest() == archive["sha256"], f"Qt archive checksum {digest.hexdigest()} differs from "
                f"the pinned {archive['sha256']}: {archive['path']} from {served}")
        partial.replace(target)
        return served
    finally:
        partial.unlink(missing_ok=True)


def fetch(base, archive, target, attempts=ATTEMPTS):
    """download, retried a bounded number of times: each failure is reported and no unverified file is kept."""
    for attempt in range(1, attempts + 1):
        try:
            return download(base, archive, target)
        except (OSError, ValueError, http.client.HTTPException) as error:
            if attempt == attempts:
                raise ValueError(f"Could not download a verified {archive['path']} in {attempts} attempts: "
                                 f"{error}") from error
            print(f"Attempt {attempt} for {archive['path']} failed: {error}", file=sys.stderr, flush=True)
            time.sleep(RETRY_DELAY * attempt)


def stage(pin, base, downloads, extractor, attempts=ATTEMPTS):
    """Download, verify and list every pinned archive before any is extracted."""
    staged = []
    for index, archive in enumerate(pin["archives"]):
        target = downloads / f"{index:02d}-{PurePosixPath(archive['path']).name}"
        served = fetch(base, archive, target, attempts)
        members = extractor.members(target)
        print(f"Verified {archive['path']} ({archive['size']} bytes, {len(members)} members) from {served}",
              flush=True)
        staged.append((archive, target))
    return staged


def finish(staged, downloads):
    for _, target in staged:
        target.unlink()
    downloads.rmdir()


def write_manifest(prefix, pin):
    (prefix / MANIFEST).write_text(json.dumps({"qt_version": pin["qt_version"], "archives": pin["archives"]},
                                              indent=2) + "\n")


def check_manifest(prefix, pin):
    """The installation under prefix was made from exactly this pin."""
    path = prefix / MANIFEST
    require(path.is_file(), f"No {MANIFEST} in {prefix}: it was not installed from a Qt pin.")
    manifest = json.loads(path.read_text())
    require(manifest == {"qt_version": pin["qt_version"], "archives": pin["archives"]},
            f"{path} records another Qt installation than the pin.")


def fetch_text(url):
    with urllib.request.urlopen(url, timeout=60) as response:
        return response.read(16 << 20).decode()


def measure(url, limit=ARCHIVE_LIMIT):
    """(size, SHA-256) of the bytes served at url."""
    digest, size = hashlib.sha256(), 0
    with urllib.request.urlopen(url, timeout=60) as response:
        while block := response.read(1 << 20):
            size += len(block)
            require(size < limit, f"Qt archive exceeds {limit} bytes: {url}")
            digest.update(block)
    return size, digest.hexdigest()


def lock(pin, repository, targets=False):
    """The pin with its archives recomputed from Updates.xml, the published .sha256 files and the archives."""
    archives = []
    for package in pin["packages"]:
        directory = f"{repository}/{package['directory']}"
        selected = package_archives(fetch_text(directory + "/Updates.xml"), package["name"], package["archives"])
        for name, target in selected:
            path = f"{package['directory']}/{package['name']}/{name}"
            published = fetch_text(f"{repository}/{path}.sha256").split()[0]
            size, digest = measure(f"{repository}/{path}")
            require(digest == published, f"The downloaded {path} does not match its published SHA-256.")
            entry = {"path": path, "size": size, "sha256": digest}
            if targets:
                require(target is not None, f"No Extract operation names the directory of {path}.")
                entry["target"] = target
            archives.append(entry)
            print(f"Locked {path} ({size} bytes)", flush=True)
    return validate_pin(dict(pin, archives=archives), repository, targets)
