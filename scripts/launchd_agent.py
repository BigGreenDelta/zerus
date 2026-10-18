"""Load or refresh one per-user launchd agent without racing its removal."""
import os
import subprocess
import time


def launchctl(*arguments):
    return subprocess.run(['launchctl', *arguments], stdin=subprocess.DEVNULL,
                          capture_output=True, text=True, timeout=30)


def stalled(printed):
    """Whether a loaded agent has no process because launchd cannot start it.

    An agent can carry a launch constraint from its Login Items (Background Task
    Management) record ("managed LWCR"). For an ad-hoc signed hgs it names the
    old build; once launchd cannot renew it, every start fails with "Unable to
    get updated LWCR" (exit status 78, EX_CONFIG) and KeepAlive retries forever.
    A kickstart is one more such start; loading the agent again renews it.
    """
    # Properties of the agent itself are indented by one tab; nested ones by more.
    lines = printed.splitlines()
    running = any(line.startswith('\tpid = ') for line in lines)
    failed = any(line.startswith(('\tjob state = spawn failed', '\tlast exit code = 78')) for line in lines)
    return not running and failed


def install(label, destination, content, restart, run=launchctl, clock=time.monotonic, sleep=time.sleep):
    """Write the definition and make launchd run it.

    Returns ('loaded' | 'restarted' | 'unchanged', '') or ('deferred', reason)
    when launchd cannot load it now, for example over SSH before login. A
    restart, and an agent launchd can no longer start, load it again.
    """
    domain = f'gui/{os.getuid()}'
    target = f'{domain}/{label}'
    changed = not destination.exists() or destination.read_bytes() != content
    if changed:
        destination.write_bytes(content)
        destination.chmod(0o644)
    printed = run('print', target)
    if printed.returncode:
        result = run('bootstrap', domain, str(destination))
        return ('deferred', result.stderr.strip()) if result.returncode else ('loaded', '')
    if not (changed or restart or stalled(printed.stdout)):
        return ('unchanged', '')
    # bootout can return while a worker is still exiting. Loading it meanwhile
    # fails and can leave the agent unloaded.
    run('bootout', target)
    deadline = clock() + 15
    while run('print', target).returncode == 0:
        if clock() > deadline:
            raise SystemExit(f'{label}: launchd is still removing the previous worker; run the installer again.')
        sleep(0.2)
    result = run('bootstrap', domain, str(destination))
    if result.returncode:
        return ('deferred', result.stderr.strip())
    return ('loaded', '') if changed else ('restarted', '')
