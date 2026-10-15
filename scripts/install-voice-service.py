#!/usr/bin/env python3
"""Install the per-user voice worker. It only processes voice messages explicitly accepted by this computer."""
import argparse
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys

import launchd_agent

parser = argparse.ArgumentParser(description="Install the per-user Zerus voice message worker.")
parser.add_argument("binary", type=Path, help="Path to the installed hgs executable")
parser.add_argument("--restart", action="store_true", help="Restart only the voice worker after installation")
args = parser.parse_args()
binary_path = args.binary.expanduser().resolve()
if not binary_path.is_file() or not os.access(binary_path, os.X_OK):
    parser.error("binary must name an existing executable file")
binary = str(binary_path)
restart = args.restart
home = Path.home()
path = os.pathsep.join(part for part in dict.fromkeys([str(home / '.local/bin'), str(home / '.cargo/bin'),
                                    '/opt/homebrew/bin', '/usr/local/bin',
                                    *os.environ.get('PATH', '/usr/bin:/bin').split(os.pathsep)]) if part)

def run(*args, check=True):
    return subprocess.run(args, check=check, stdin=subprocess.DEVNULL, capture_output=True, text=True)

if sys.platform == 'darwin':
    label = 'com.hgdev.hgs-voice'
    destination = home / 'Library/LaunchAgents' / (label + '.plist')
    destination.parent.mkdir(parents=True, exist_ok=True)
    logs = home / 'Library/Logs/hgs'; logs.mkdir(parents=True, exist_ok=True)
    content = plistlib.dumps(dict(Label=label, ProgramArguments=[binary, 'voice', 'worker'],
                                 RunAtLoad=True, KeepAlive=True, ThrottleInterval=10,
                                 EnvironmentVariables={'PATH': path},
                                 StandardErrorPath=str(logs / 'voice.log')))
    state, reason = launchd_agent.install(label, destination, content, restart)
    if state == 'loaded':
        print('Installed voice message worker.')
    elif state == 'deferred':
        print('Voice service installed for next login: ' + reason)
elif sys.platform.startswith('linux') and shutil.which('systemctl'):
    destination = home / '.config/systemd/user/hgs-voice.service'
    destination.parent.mkdir(parents=True, exist_ok=True)
    def quote(value):
        return '"' + value.replace('\\', '\\\\').replace('"', '\\"').replace('%', '%%').replace('\n', '\\n') + '"'
    content = f'''[Unit]
Description=HGS voice message worker

[Service]
Type=simple
ExecStart={quote(binary)} voice worker
Environment={quote('PATH=' + path)}
Restart=always
RestartSec=10
# Kill only the worker itself, never tmux servers, agents or hosts it started.
KillMode=process

[Install]
WantedBy=default.target
'''
    if not destination.exists() or destination.read_text() != content:
        destination.write_text(content); destination.chmod(0o644)
    result = run('systemctl', '--user', 'daemon-reload', check=False)
    if result.returncode:
        print('Voice service installed; user service manager is unavailable: ' + result.stderr.strip())
    else:
        result = run('systemctl', '--user', 'enable', '--now', 'hgs-voice.service', check=False)
        if result.returncode:
            raise RuntimeError(result.stderr.strip())
        if restart:
            run('systemctl', '--user', 'restart', 'hgs-voice.service')
        print('Installed voice message worker.')
