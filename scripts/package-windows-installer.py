#!/usr/bin/env python3
"""Build the per-user Windows installer from the tested portable zip.

The zip is checked as release assembly checks it, its files are extracted
unchanged next to installation.json and the payload list, and Inno Setup 6
compiles packaging/windows/zerus.iss over them. Nothing is rebuilt or re-signed:
every payload file keeps the zip's bytes. --stage-only writes the payload and
prints the compiler command without running it, on any platform.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from release_common import require
import windows_installer

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "packaging" / "windows" / "zerus.iss"
ICON = ROOT / "tray" / "resources" / "icons" / "hgs-zerus.ico"


def compiler(explicit):
    """ISCC.exe: --iscc, else Inno Setup 6 where its installer puts it."""
    if explicit:
        return Path(explicit)
    for variable in ("ProgramFiles(x86)", "ProgramFiles"):
        base = os.environ.get(variable)
        if base and (Path(base) / "Inno Setup 6" / "ISCC.exe").is_file():
            return Path(base) / "Inno Setup 6" / "ISCC.exe"
    found = shutil.which("ISCC.exe") or shutil.which("iscc")
    require(found, "Inno Setup 6 is not installed; pass --iscc.")
    return Path(found)


def command(iscc, payload, output, version):
    return [str(iscc), "/Q",
            f"/DAppVersion={version}",
            f"/DNumericVersion={windows_installer.numeric_version(version)}",
            f"/DPayloadDir={payload}",
            f"/DOutputDir={output.parent}",
            f"/DOutputName={output.stem}",
            f"/DIconFile={ICON}",
            str(SCRIPT)]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("zip", type=Path, help="tested portable zip (scripts/package-windows.py)")
    parser.add_argument("--version", help="release version the installer carries [default: the zip's product version]")
    parser.add_argument("--output", type=Path, required=True, help="installer to write (.exe)")
    parser.add_argument("--iscc", help="ISCC.exe [default: Inno Setup 6 under Program Files]")
    parser.add_argument("--stage-only", type=Path, metavar="FOLDER",
                        help="write the payload into this empty folder and print the compiler command")
    args = parser.parse_args()
    require(args.output.suffix == ".exe", "The installer must be an .exe file.")
    output = args.output.resolve()
    if args.stage_only:
        args.stage_only.mkdir(parents=True, exist_ok=True)
        manifest, expected = windows_installer.stage(args.zip, args.stage_only)
        version = args.version or manifest["version"]
        print(" ".join(command(Path("ISCC.exe"), args.stage_only.resolve(), output, version)))
        print(f"Staged {len(expected)} files of Zerus {manifest['version']} in {args.stage_only}.")
        return
    iscc = compiler(args.iscc)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="zerus-installer-") as temporary:
        payload = Path(temporary) / "payload"
        payload.mkdir()
        manifest, expected = windows_installer.stage(args.zip, payload)
        version = args.version or manifest["version"]
        subprocess.run(command(iscc, payload, output, version), check=True, stdin=subprocess.DEVNULL)
    require(output.is_file(), f"Inno Setup wrote no {output}.")
    print(f"{output.name}: {output.stat().st_size} bytes, {len(expected)} files of Zerus {version} "
          f"({manifest['source_commit']}).")


if __name__ == "__main__":
    try:
        main()
    except ValueError as error:
        sys.exit(f"package-windows-installer: {error}")
