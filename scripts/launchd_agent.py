"""Load or refresh one per-user launchd agent without racing its removal."""
import os
import subprocess
import time


def launchctl(*arguments):
    return subprocess.run(['launchctl', *arguments], stdin=subprocess.DEVNULL,
                          capture_output=True, text=True, timeout=30)


def install(label, destination, content, restart, run=launchctl, clock=time.monotonic, sleep=time.sleep):
    """Write the definition and make launchd run it.

    Returns ('loaded' | 'restarted' | 'unchanged', '') or ('deferred', reason)
    when launchd cannot load it now, for example over SSH before login.
    """
    domain = f'gui/{os.getuid()}'
    target = f'{domain}/{label}'
    changed = not destination.exists() or destination.read_bytes() != content
    if changed:
        destination.write_bytes(content)
        destination.chmod(0o644)
    loaded = run('print', target).returncode == 0
    if loaded and changed:
        # bootout can return while a worker is still exiting. Restarting or
        # loading it meanwhile fails and can leave the agent unloaded.
        run('bootout', target)
        deadline = clock() + 15
        while run('print', target).returncode == 0:
            if clock() > deadline:
                raise SystemExit(f'{label}: launchd is still removing the previous worker; run the installer again.')
            sleep(0.2)
        loaded = False
    if not loaded:
        result = run('bootstrap', domain, str(destination))
        return ('deferred', result.stderr.strip()) if result.returncode else ('loaded', '')
    if not restart:
        return ('unchanged', '')
    result = run('kickstart', '-k', target)
    if result.returncode:
        raise SystemExit(f'Cannot restart {label}: {result.stderr.strip()}')
    return ('restarted', '')
