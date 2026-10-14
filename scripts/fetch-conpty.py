#!/usr/bin/env python3
"""Fetch the pinned ConPTY pair, conpty.dll and OpenConsole.exe, into a folder.

The NuGet package Microsoft.Windows.Console.ConPTY pinned in
packaging/windows/conpty.json is downloaded from nuget.org (or read from
--nupkg) and checked against its pinned size and SHA-256 before it is opened.
Only the x64 conpty.dll and OpenConsole.exe are taken from it, each checked
against its own pinned SHA-256; nothing in the package is run. The Windows CI
job passes the folder to the session host tests (HGS_TEST_CONPTY_DIR) and to
scripts/package-windows.py (--conpty).
"""
import argparse
from pathlib import Path

import conpty_pair


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, required=True, help="folder to write the two files into")
    parser.add_argument("--nupkg", type=Path, help="a downloaded copy of the pinned package [default: download it]")
    args = parser.parse_args()
    pair = conpty_pair.fetch(args.output, package=args.nupkg)
    pin = conpty_pair.PIN
    print(f"Fetched {conpty_pair.PACKAGE} {pin['version']} (sha256 {pin['sha256']}):")
    for name, path in pair.items():
        print(f"  {path}  sha256 {pin['files'][name]['sha256']}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError) as error:
        raise SystemExit(f"fetch-conpty: {error}") from None
