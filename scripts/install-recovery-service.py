#!/usr/bin/env python3
"""Install the per-user recovery worker. It performs no recovery until enabled."""
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys

import launchd_agent

binary = str(Path(sys.argv[1]).resolve())
restart = '--restart' in sys.argv[2:]
home = Path.home()
path = os.pathsep.join(part for part in dict.fromkeys([str(home / '.local/bin'), str(home / '.cargo/bin'),
                                    '/opt/homebrew/bin', '/usr/local/bin',
                                    *os.environ.get('PATH', '/usr/bin:/bin').split(os.pathsep)]) if part)

def run(*args, check=True):
    return subprocess.run(args, check=check, stdin=subprocess.DEVNULL, capture_output=True, text=True)

if sys.platform == 'darwin':
    label = 'com.hgdev.hgs-recovery'
    destination = home / 'Library/LaunchAgents' / (label + '.plist')
    destination.parent.mkdir(parents=True, exist_ok=True)
    logs = home / 'Library/Logs/hgs'; logs.mkdir(parents=True, exist_ok=True)
    content = plistlib.dumps(dict(Label=label, ProgramArguments=[binary, 'recovery', 'worker'],
                                 RunAtLoad=True, KeepAlive=True, ThrottleInterval=10,
                                 EnvironmentVariables={'PATH': path},
                                 StandardErrorPath=str(logs / 'recovery.log')))
    state, reason = launchd_agent.install(label, destination, content, restart)
    if state == 'loaded':
        print('Installed automatic recovery worker (disabled until enabled in Settings).')
    elif state == 'deferred':
        print('Recovery service installed for next login: ' + reason)
elif sys.platform.startswith('linux') and shutil.which('systemctl'):
    destination = home / '.config/systemd/user/hgs-recovery.service'
    destination.parent.mkdir(parents=True, exist_ok=True)
    def quote(value):
        return '"' + value.replace('\\', '\\\\').replace('"', '\\"').replace('%', '%%').replace('\n', '\\n') + '"'
    content = f'''[Unit]
Description=HGS automatic recovery worker

[Service]
Type=simple
ExecStart={quote(binary)} recovery worker
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
        print('Recovery service installed; user service manager is unavailable: ' + result.stderr.strip())
    else:
        result = run('systemctl', '--user', 'enable', '--now', 'hgs-recovery.service', check=False)
        if result.returncode:
            raise RuntimeError(result.stderr.strip())
        if restart:
            run('systemctl', '--user', 'restart', 'hgs-recovery.service')
        print('Installed automatic recovery worker (disabled until enabled in Settings).')
