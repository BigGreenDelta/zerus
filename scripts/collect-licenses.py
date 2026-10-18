#!/usr/bin/env python3
"""Collect exact host-target Cargo dependency notices for binary distribution."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
# Reviewed notice texts for crates that publish none; see the README there.
FALLBACK = ROOT / "packaging/rust-licenses"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path)
    # Each locked Cargo project whose binary the distribution ships; the root
    # workspace (hgs) by default. Linux packages add the mobile connector.
    parser.add_argument("--manifest-path", type=Path, action="append", dest="manifests")
    args = parser.parse_args()
    target = next(line[6:] for line in subprocess.check_output(["rustc", "-vV"], text=True).splitlines()
                  if line.startswith("host: "))
    packages = {}
    for manifest in args.manifests or [None]:
        command = ["cargo", "metadata", "--locked", "--offline", "--format-version", "1", "--filter-platform", target]
        if manifest is not None:
            command += ["--manifest-path", str(manifest)]
        metadata = json.loads(subprocess.check_output(command, text=True))
        resolved = {node["id"] for node in metadata["resolve"]["nodes"]}
        for package in metadata["packages"]:
            if package.get("source") and package["id"] in resolved:
                packages[(package["name"], package["version"])] = package
    destination = args.destination.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    inventory = []
    for _, package in sorted(packages.items()):
        source = Path(package["manifest_path"]).parent
        notices = [path for path in source.rglob("*") if path.is_file()
                   and path.name.upper().startswith(("LICENSE", "LICENCE", "COPYING", "NOTICE", "UNLICENSE"))]
        if package.get("license_file"):
            notices.append(source / package["license_file"])
        notices = sorted(set(notices))
        if not notices and (FALLBACK / package["name"]).is_dir():
            source = FALLBACK / package["name"]
            notices = sorted(path for path in source.iterdir() if path.is_file())
        if not notices:
            raise SystemExit(f"No license text found for {package['name']} {package['version']}; review before distribution.")
        folder = destination / f"{package['name']}-{package['version']}"
        for notice in notices:
            relative = notice.relative_to(source)
            output = folder / relative
            output.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(notice, output)
        if package["name"] == "libsqlite3-sys":
            # This crate's own MIT notice does not describe bundled SQLite.
            # Preserve the exact upstream public-domain dedication too.
            header = (source / "sqlite3/sqlite3.h").read_text().split("*************************************************************************", 1)[0]
            if "The author disclaims copyright" not in header:
                raise SystemExit("Review the bundled SQLite dedication before distribution.")
            (folder / "SQLite-public-domain.txt").write_text(header + "*/\nhttps://www.sqlite.org/copyright.html\n")
        inventory.append({key: package.get(key) for key in ("name", "version", "license", "repository")})
    (destination / "inventory.json").write_text(json.dumps({"target": target, "packages": inventory}, indent=2) + "\n")
    print(f"Collected notices for {len(inventory)} host-target dependencies.")


if __name__ == "__main__":
    main()
