"""Compatibility CLI that delegates administration and serving to the Rust relay."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import sys


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Zerus trusted mobile relay (TLS transport, no E2EE)"
    )
    parser.add_argument(
        "--database",
        default=str(Path.home() / ".local/share/zerus-mobile/relay.sqlite3"),
    )
    parser.add_argument(
        "--config",
        help="Private JSON configuration, including optional PostgreSQL database_url",
    )
    commands = parser.add_subparsers(dest="command", required=True)
    provision = commands.add_parser(
        "provision",
        help="Create workspace, computer credential and one-time phone invitation",
    )
    provision.add_argument("--name", required=True)
    provision.add_argument("--computer-name", default="Computer")
    node = commands.add_parser("node", help="Add a computer credential")
    node.add_argument("--workspace", required=True)
    node.add_argument("--name", required=True)
    invitation = commands.add_parser(
        "invite", help="Create a ten-minute, single-use phone invitation"
    )
    invitation.add_argument("--workspace", required=True)
    for command in ("revoke-device", "revoke-node", "revoke-computer"):
        commands.add_parser(command).add_argument("--id", required=True)
    serve = commands.add_parser("serve")
    serve.add_argument("--host", default="127.0.0.1")
    serve.add_argument("--port", type=int, default=8787)
    for command in (
        provision,
        node,
        invitation,
        serve,
        *[commands.choices[c] for c in ("revoke-device", "revoke-node", "revoke-computer")],
    ):
        command.add_argument(
            "--config",
            default=argparse.SUPPRESS,
            help="Private JSON relay configuration",
        )
    parser.parse_args(argv)
    os.umask(0o077)
    binary = shutil.which("zerus-relay")
    if binary is None:
        parser.error("install the Rust zerus-relay binary; see services/mobile/relay/README.md")
    os.execv(binary, [binary, *(sys.argv[1:] if argv is None else argv)])


if __name__ == "__main__":
    main()
