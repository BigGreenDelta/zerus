#!/usr/bin/env python3
"""Explicitly install the optional Linux speech service without touching agents."""
import argparse
import os
from pathlib import Path
import subprocess


def unit_argument(value):
    value = str(value)
    if any(ord(char) < 32 for char in value):
        raise ValueError("Service paths cannot contain control characters")
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"').replace("%", "%%") + '"'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--python", required=True, type=Path, help="Python in the prepared speech environment")
    parser.add_argument("--config", type=Path,
                        help="Existing service configuration; omitted uses the optional default speech.json")
    parser.add_argument("--start", action="store_true", help="Enable and start only zerus-speech.service")
    args = parser.parse_args()
    runtime = args.python.absolute()
    config = (args.config if args.config is not None else
              Path(os.environ.get("HGS_CONFIG_DIR", str(Path.home() / ".config/hgs"))) / "speech.json").absolute()
    if not runtime.is_file() or not os.access(runtime, os.X_OK):
        parser.error("Prepare an executable Python environment first")
    if (args.config is not None or config.exists() or config.is_symlink()) and not config.is_file():
        parser.error("An explicit or existing service configuration must be a regular file")
    subprocess.run([str(runtime), "-c", "import zerus_speech"], check=True)
    directory = Path.home() / ".config/systemd/user"
    directory.mkdir(parents=True, exist_ok=True)
    unit = directory / "zerus-speech.service"
    unit.write_text("\n".join([
        "[Unit]", "Description=Zerus private speech recognition", "", "[Service]",
        "Type=simple", f"ExecStart={unit_argument(runtime)} -m zerus_speech --config {unit_argument(config)}",
        "Restart=on-failure", "RestartSec=5", "TimeoutStopSec=15", "UMask=0077",
        "NoNewPrivileges=true", "PrivateTmp=true", "", "[Install]", "WantedBy=default.target", "",
    ]))
    subprocess.run(["systemctl", "--user", "daemon-reload"], check=True)
    if args.start:
        subprocess.run(["systemctl", "--user", "enable", "--now", "zerus-speech.service"], check=True)
    print(f"Installed {unit}")


if __name__ == "__main__":
    main()
