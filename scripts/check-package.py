#!/usr/bin/env python3
"""Check staged package completeness and its executables using an isolated home."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--package-name", default="zerus-git")
    args = parser.parse_args()
    root = args.root.resolve()
    required = ["usr/bin/hgs", "usr/bin/hgs-tray", "usr/bin/hgs_state.py", "usr/bin/zerus-setup",
                "usr/share/applications/hgs-tray.desktop", "usr/lib/systemd/user/hgs-tray.service",
                "usr/lib/systemd/user/hgs-swarm.service", "usr/lib/systemd/user/hgs-recovery.service",
                f"usr/share/licenses/{args.package_name}/LICENSE", f"usr/share/licenses/{args.package_name}/libvterm-LICENSE",
                f"usr/share/licenses/{args.package_name}/md4c-LICENSE",
                f"usr/share/licenses/{args.package_name}/rust/inventory.json", "usr/share/doc/zerus/build-info.json"]
    for name in required:
        if not (root / name).is_file():
            raise SystemExit(f"Missing package file: {name}")
    for path in root.iterdir():
        if path.name not in {"usr", ".PKGINFO", ".BUILDINFO", ".MTREE"}:
            raise SystemExit(f"Unexpected package root: {path.name}")
    manifest = json.loads((root / "usr/share/doc/zerus/build-info.json").read_text())
    with tempfile.TemporaryDirectory(prefix="zerus-package-test-") as home:
        env = {key: value for key, value in os.environ.items() if not key.startswith("HGS_")}
        env.update(HOME=home, HGS_STATE_DIR=home+"/state", HGS_CONFIG_DIR=home+"/config")
        desktop = manifest.get("desktop_build", {})
        expected = {"hgs": "hgs " + manifest["cli_version"], "hgs-tray": "Zerus " + desktop.get("versionName", manifest["version"])}
        for name, version in expected.items():
            result = subprocess.run([str(root / "usr/bin" / name), "--version"], env=env,
                                    capture_output=True, text=True, timeout=20, check=True)
            if result.stdout.strip() != version:
                raise SystemExit(f"Wrong {name} version: {result.stdout.strip()}")
        if desktop:
            actual = json.loads(subprocess.check_output([str(root / "usr/bin/hgs-tray"), "--build-info"], env=env, text=True, timeout=20))
            if actual != desktop:
                raise SystemExit("GUI embedded build identity does not match the package manifest")
    subprocess.run(["desktop-file-validate", str(root / "usr/share/applications/hgs-tray.desktop")], check=True)
    inventory = json.loads((root / f"usr/share/licenses/{args.package_name}/rust/inventory.json").read_text())
    if not inventory["packages"]:
        raise SystemExit("Dependency license inventory is empty.")
    print(f"Package layout and executable versions verified; {len(inventory['packages'])} dependency notices included.")


if __name__ == "__main__":
    main()
