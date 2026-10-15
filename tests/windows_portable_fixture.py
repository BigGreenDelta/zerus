"""Synthetic Windows portable zips for the stable and nightly publication contracts."""
import copy
import hashlib
import json
from pathlib import Path
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import conpty_pair
import windows_portable


def portable_zip(path, commit="a" * 40, version="0.37.0", entries=()):
    """A synthetic Windows portable zip with the complete required layout."""
    manifest = {"product": "Zerus", "version": version, "cli_version": "1.48.0", "platform": "Windows",
                "architecture": "x86_64", "qt_version": "6.10.3", "native_agents_bundled": False,
                "source_commit": commit}
    with zipfile.ZipFile(path, "w") as archive:
        for name in windows_portable.REQUIRED:
            data = json.dumps(manifest) if name == "build-info.json" else "tested " + name
            archive.writestr(f"{windows_portable.TOP}/{name}", data)
        for name, data in entries:
            archive.writestr(name, data)
    return path


def pinned_pair():
    """The ConPTY pin with the stand-ins of `portable_zip` as its pair."""
    pin = copy.deepcopy(conpty_pair.load())
    for name in conpty_pair.NAMES:
        data = f"tested {name}".encode()
        pin["files"][name].update(size=len(data), sha256=hashlib.sha256(data).hexdigest())
    return pin
