"""The owner rule for signing the Windows portable zip (owner, 2026-10-17):
«dll класть можно, нельзя на них подпись менять!» Bundled third-party DLLs and
EXEs ship, but their bytes, and so their signatures, never change.

Whichever signing provider runs between the tested unsigned zip and the sealed
release zip, only the executables built from this repository may differ
afterwards; every other entry, the entry set and the layout stay identical.
Nothing here signs anything (docs/superpowers/specs/2026-10-18-windows-installer-design.md).
"""
import hashlib
import zipfile

from release_common import require
from windows_portable import TOP, inspect

# PE files built from this repository: the only ones a signing step may change.
ZERUS_BUILT = ("hgs.exe", "hgs-tray.exe")


def digests(path):
    """SHA-256 of every entry of a checked portable zip, by name below TOP."""
    inspect(path)
    result = {}
    with zipfile.ZipFile(path) as archive:
        for entry in archive.infolist():
            digest = hashlib.sha256()
            with archive.open(entry) as stream:
                for block in iter(lambda: stream.read(1 << 20), b""):
                    digest.update(block)
            result[entry.filename[len(TOP) + 1:]] = digest.hexdigest()
    return result


def changes(unsigned, signed):
    """The entries a signing step changed, after checking that it changed only
    ZERUS_BUILT files and neither added nor removed any."""
    before, after = digests(unsigned), digests(signed)
    require(before.keys() == after.keys(),
            "Signing changed the zip's files: " + ", ".join(sorted(before.keys() ^ after.keys())))
    changed = sorted(name for name in before if before[name] != after[name])
    foreign = [name for name in changed if name not in ZERUS_BUILT]
    require(not foreign, "Signing changed files not built from this repository: " + ", ".join(foreign))
    return changed
