"""The bundled ConPTY pair of the Windows session host (spec D6).

packaging/windows/conpty.json pins Microsoft's NuGet package
Microsoft.Windows.Console.ConPTY by URL, size and SHA-256, and the x64
conpty.dll and OpenConsole.exe inside it by path, size and SHA-256. `fetch`
downloads the package (or reads a given copy), checks it before opening it, and
writes exactly those two files once they match too. Nothing else from the
package is used, and nothing in it is run here.

The session host loads conpty.dll by absolute path from its image folder, and
conpty.dll starts the OpenConsole.exe next to it (without one there it tries an
x64 subfolder, then silently uses the inbox conhost.exe). `hgs session-host
install` stages both from the folder of the hgs.exe it runs as (`PAIR` in
src/host/staging.rs), so the portable zip carries them next to hgs.exe.
"""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import tempfile
import urllib.request
import zipfile
import zlib

from release_common import require, sha256

ROOT = Path(__file__).resolve().parents[1]
PIN_FILE = ROOT / "packaging/windows/conpty.json"
PACKAGE = "Microsoft.Windows.Console.ConPTY"
FEED = "https://api.nuget.org/v3-flatcontainer"
# The file names the host image holds (src/host/staging.rs PAIR, src/host/conpty.rs).
NAMES = ("conpty.dll", "OpenConsole.exe")
# Bounds for pinned sizes; the pinned package is under 2 MB.
PACKAGE_LIMIT = 64 * 1024 * 1024
FILE_LIMIT = 16 * 1024 * 1024
DIGEST = re.compile(r"[0-9a-f]{64}")
VERSION = re.compile(r"\d+\.\d+\.\d+")


def sized(entry, limit, what):
    require(type(entry.get("size")) is int and 0 < entry["size"] <= limit, f"Invalid pinned size of {what}.")
    require(isinstance(entry.get("sha256"), str) and DIGEST.fullmatch(entry["sha256"]),
            f"Invalid pinned SHA-256 of {what}.")


def validate(pin):
    """The pin, after checking that it names the official package and its x64 pair."""
    require(isinstance(pin, dict) and set(pin) == {"package", "version", "url", "size", "sha256", "license", "files"},
            "Unexpected ConPTY pin fields.")
    require(pin["package"] == PACKAGE, "The ConPTY pair must come from Microsoft.Windows.Console.ConPTY.")
    require(isinstance(pin["version"], str) and VERSION.fullmatch(pin["version"]), "Invalid ConPTY version.")
    lower, version = PACKAGE.lower(), pin["version"]
    require(pin["url"] == f"{FEED}/{lower}/{version}/{lower}.{version}.nupkg",
            "The ConPTY package must come from nuget.org under its own name and version.")
    sized(pin, PACKAGE_LIMIT, "the ConPTY package")
    require(pin["license"] == "MIT", "Review the ConPTY license and notices.")
    files = pin["files"]
    require(isinstance(files, dict) and sorted(files) == sorted(NAMES), "The ConPTY pin must name conpty.dll and "
            "OpenConsole.exe.")
    for name, entry in files.items():
        require(isinstance(entry, dict) and set(entry) == {"path", "size", "sha256"},
                f"Unexpected ConPTY pin fields for {name}.")
        path = entry["path"]
        parts = PurePosixPath(path).parts if isinstance(path, str) else ()
        require(bool(parts) and not path.startswith("/") and ".." not in parts and "\\" not in path and
                parts[-1] == name and ("win-x64" in parts or "x64" in parts),
                f"The ConPTY pin must name the x64 {name} by its package path.")
        sized(entry, FILE_LIMIT, name)
    return pin


def load(path=PIN_FILE):
    return validate(json.loads(Path(path).read_text(encoding="utf-8")))


PIN = load()


def matches(entry, data):
    """Whether `data` is the pinned file `entry` (its size and SHA-256)."""
    return len(data) == entry["size"] and hashlib.sha256(data).hexdigest() == entry["sha256"]


def download(url, target):
    """Download `url` to `target`, reading at most one byte more than pinned."""
    limit = PIN["size"]
    received = 0
    with urllib.request.urlopen(url, timeout=60) as response, open(target, "wb") as output:
        while block := response.read(1 << 20):
            received += len(block)
            require(received <= limit, "The ConPTY package is larger than pinned.")
            output.write(block)


def check_package(path):
    """Refuse a package that is not the pinned one, before anything opens it."""
    path = Path(path)
    require(path.is_file() and not path.is_symlink(), f"The ConPTY package is not a regular file: {path}")
    require(path.stat().st_size == PIN["size"] and sha256(path) == PIN["sha256"],
            f"{path.name} is not the pinned {PACKAGE} {PIN['version']} package.")


def extract(path):
    """The pinned pair's bytes from the checked package at `path`, by file name."""
    pair = {}
    try:
        with zipfile.ZipFile(path) as archive:
            for name, entry in PIN["files"].items():
                try:
                    info = archive.getinfo(entry["path"])
                except KeyError:
                    raise ValueError(f"The ConPTY package lacks {entry['path']}.") from None
                require(info.file_size == entry["size"], f"{entry['path']} in the ConPTY package is not the pinned build.")
                with archive.open(info) as stream:
                    data = stream.read(entry["size"] + 1)
                require(matches(entry, data), f"{entry['path']} in the ConPTY package is not the pinned build.")
                pair[name] = data
    except (zipfile.BadZipFile, zlib.error, EOFError):
        raise ValueError("The ConPTY package is not a valid archive.") from None
    return pair


def check_folder(folder):
    """{name: path} of the pinned pair in `folder`; anything else there is ignored."""
    folder = Path(folder)
    found = {}
    for name, entry in PIN["files"].items():
        path = folder / name
        require(path.is_file() and not path.is_symlink(), f"{path} is missing; run scripts/fetch-conpty.py.")
        require(path.stat().st_size == entry["size"] and sha256(path) == entry["sha256"],
                f"{path} is not the {name} of the pinned {PACKAGE} {PIN['version']}.")
        found[name] = path
    return found


def fetch(output, package=None, url=None):
    """Write the pinned pair into the folder `output` and return {name: path}.

    The package comes from `package`, a local copy, or else is downloaded from
    `url` (the pinned nuget.org URL by default) into a temporary folder. It is
    checked before it is opened, and both files are checked before either is
    written; each is written under a temporary name and then renamed."""
    output = Path(output)
    with tempfile.TemporaryDirectory(prefix="zerus-conpty-") as temporary:
        if package is None:
            package = Path(temporary) / PurePosixPath(PIN["url"]).name
            download(url or PIN["url"], package)
        check_package(package)
        pair = extract(package)
    output.mkdir(parents=True, exist_ok=True)
    for name, data in pair.items():
        partial = output / f".{name}.partial"
        try:
            partial.write_bytes(data)
            os.replace(partial, output / name)
        finally:
            partial.unlink(missing_ok=True)
    return check_folder(output)
