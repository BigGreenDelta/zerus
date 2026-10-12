#!/usr/bin/env python3
"""Portable source checks without rewriting legacy formatting."""
import ast
from pathlib import Path
import re
import subprocess
import sys
import tomllib

ROOT = Path(__file__).resolve().parents[2]


def main():
    subprocess.run([sys.executable, "-m", "unittest", "discover", "-q", "-s", "tests", "-p", "test_source_privacy.py"], cwd=ROOT, check=True)
    subprocess.run([sys.executable, str(ROOT / "scripts/check-source-privacy.py")], check=True)
    subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)
    version = (ROOT / "VERSION").read_text().strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        raise SystemExit("VERSION must contain one stable semantic version.")
    cmake = (ROOT / "tray/CMakeLists.txt").read_text()
    if f"project(hgs-tray VERSION {version} " not in cmake:
        raise SystemExit("VERSION and the desktop version must match before release.")
    cargo = tomllib.loads((ROOT / "Cargo.toml").read_text())
    if cargo["package"]["license"] != "MIT":
        raise SystemExit("Review the package license and notices.")
    # GitHub enforces these scoped cache permissions. actionlint 1.7.12 does
    # not recognize the new key yet; validate its fixed enum before ignoring
    # only that known syntax warning in actionlint.
    for workflow in (ROOT / ".github/workflows").glob("*.yml"):
        for line in workflow.read_text().splitlines():
            match = re.match(r"^( *)cache-mode:\s*([^#]*)(?:#.*)?$", line)
            if match and (len(match[1]) not in {0, 4} or
                          match[2].strip() not in {"read", "write", "write-only", "none"}):
                raise SystemExit(f"{workflow.name}: cache-mode requires a fixed workflow/job enum.")
    paths = subprocess.check_output(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"], cwd=ROOT).decode().split("\0")
    for name in sorted(set(filter(None, paths))):
        path = ROOT / name
        if not path.is_file():
            continue
        if path.suffix == ".py" or name in {"packaging/linux/zerus-setup", "packaging/macos/zerus-setup"}:
            ast.parse(path.read_text(), filename=name)
        elif path.read_bytes().split(b"\n", 1)[0].startswith((b"#!/bin/sh", b"#!/usr/bin/env bash")) or path.name in {"PKGBUILD", "PKGBUILD.in"}:
            subprocess.run(["bash", "-n", str(path)], check=True)
    print("Source syntax, privacy and release version checks passed.")


if __name__ == "__main__":
    main()
