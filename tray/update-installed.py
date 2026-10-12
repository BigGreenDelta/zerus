#!/usr/bin/env python3
"""Replace the installed desktop GUI with a new build, keeping drafts and native agents.

    update-installed.py <built hgs-tray | hgs-tray.app> <installed path>

tray/install.sh runs this whenever its build differs from the installed GUI.

1. Copy the build into a unique folder beside the installed GUI; the folder is
   removed on every outcome.
2. If the installed GUI is running, quit it the way its Quit action does:
   SIGTERM when the process handles it (current builds flush drafts on that
   signal), otherwise the tray menu over D-Bus on Linux or the quit event on
   macOS. A GUI that does not quit keeps running and nothing is replaced.
3. Keep exactly one rollback copy (~/.local/share/hgs/desktop-previous) and move
   the new GUI into place.
4. Start a GUI that was running through its service manager and wait for its
   single-instance socket; a new GUI that does not start is replaced by the
   previous one again. Then confirm the running image, the unchanged nonempty
   drafts and the native agent and tmux processes, and record
   ~/.local/share/hgs/build-provenance.json.
5. Remove hidden leftover folders of earlier hand-made updates beside the
   installed GUI (.zerus-*, stale .hgs-tray-update-*) when they are at least an
   hour old, hold only installer copies and no process uses them.

Native agents, tmux servers and other services are never signalled. When a check
fails, the private draft snapshot is kept and its location printed.
HGS_TRAY_QUIT_TIMEOUT and HGS_TRAY_START_TIMEOUT (seconds, default 35) bound the
waits for the old GUI to quit and the new one to start.
"""
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

MAC = sys.platform == 'darwin'
HOME = Path.home()
SHARE = HOME / '.local/share/hgs'
ROLLBACK = SHARE / 'desktop-previous'
PROVENANCE = SHARE / 'build-provenance.json'
SNAPSHOTS = HOME / '.local/state/hgs/desktop-update'
CLI = HOME / '.local/bin/hgs'
HELPER = HOME / '.local/bin/hgs_state.py'
LABEL = 'com.hgdev.hgs-tray'
PLIST = HOME / f'Library/LaunchAgents/{LABEL}.plist'
UNIT = 'hgs-tray.service'
NATIVE = {'codex', 'claude', 'kimi', 'dsh', 'node', 'codex-code-mode'}
# Hidden folders that earlier hand-made updates (and an interrupted run of this
# one) left beside the installed GUI, and the installer copies they may hold.
LEGACY = ('.zerus-', '.hgs-tray-update-')
LEGACY_FILES = {'hgs-tray', 'hgs', 'hgs_state.py'}
LEGACY_AGE = 3600
# Slow machines may allow longer; the defaults match the verified manual updates.
QUIT_SECONDS = float(os.environ.get('HGS_TRAY_QUIT_TIMEOUT') or 35)
START_SECONDS = float(os.environ.get('HGS_TRAY_START_TIMEOUT') or 35)


class UpdateError(Exception):
    pass


def say(text):
    print(f'hgs-tray update: {text}', flush=True)


def run(args, **kwargs):
    return subprocess.check_output([str(a) for a in args], text=True, stderr=subprocess.PIPE, **kwargs).strip()


def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def digest(path):
    hasher = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            hasher.update(block)
    return hasher.hexdigest()


def executable(gui):
    return gui / 'Contents/MacOS/hgs-tray' if MAC else gui


def drafts_dir():
    if MAC:
        return HOME / 'Library/Preferences/com.hgdev.hgs-tray.plist.drafts'
    config = os.environ.get('XDG_CONFIG_HOME') or str(HOME / '.config')
    return Path(config) / 'hgdev/hgs-tray.conf.drafts'


def remove(path):
    if path.is_dir() and not path.is_symlink():
        shutil.rmtree(path)
    elif path.exists() or path.is_symlink():
        path.unlink()


def write_json(path, data):
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(dir=path.parent, prefix=f'.{path.name}.')
    with os.fdopen(fd, 'w') as f:
        json.dump(data, f, indent=2)
        f.write('\n')
    os.replace(temporary, path)


# --- processes -----------------------------------------------------------------

def gui_pids(installed):
    """This user's processes running the installed GUI (never one from another path)."""
    target = str(executable(installed))
    pids = []
    if MAC:
        for line in run(['ps', '-x', '-o', 'pid=,comm=']).splitlines():
            pid, _, command = line.strip().partition(' ')
            if command.strip() == target:
                pids.append(int(pid))
        return sorted(pids)
    for process in Path('/proc').iterdir():
        if not process.name.isdigit():
            continue
        try:
            if process.stat().st_uid != os.getuid() or (process / 'comm').read_text().strip() != 'hgs-tray':
                continue
            exe = os.readlink(process / 'exe').removesuffix(' (deleted)')
            argv = [part.decode(errors='replace') for part in (process / 'cmdline').read_bytes().split(b'\0')[:3]]
        except OSError:
            continue
        # A script started through its #! line has its path among the first arguments.
        if exe == target or target in argv:
            pids.append(int(process.name))
    return sorted(pids)


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    if not MAC:
        # An exited process its parent has not reaped yet is gone.
        try:
            return Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()[0] != 'Z'
        except (OSError, IndexError):
            return False
    return True


def native():
    """Native agents and tmux servers of this user with their start times, keyed by PID."""
    records = {}
    if MAC:
        for line in run(['ps', '-x', '-o', 'pid=,lstart=,comm=']).splitlines():
            parts = line.split(None, 6)
            if len(parts) == 7:
                name = Path(parts[6]).name
                if name in NATIVE or name.startswith('tmux'):
                    records[parts[0]] = {'name': name, 'start': ' '.join(parts[1:6])}
        return records
    for process in Path('/proc').iterdir():
        if not process.name.isdigit():
            continue
        try:
            if process.stat().st_uid != os.getuid():
                continue
            name = (process / 'comm').read_text().strip()
            if name in NATIVE or name.startswith('tmux'):
                fields = (process / 'stat').read_text().rsplit(')', 1)[1].split()
                records[process.name] = {'name': name, 'start': fields[19]}
        except OSError:
            pass
    return records


def socket_path(pid):
    """The GUI's single-instance socket, read from the process itself."""
    if MAC:
        try:
            names = run(['lsof', '-n', '-a', '-p', str(pid), '-U', '-F', 'n'])
        except (OSError, subprocess.CalledProcessError):
            return None
        found = [line[1:] for line in names.splitlines() if line.startswith('n') and line.endswith('/hgs-tray')]
        return found[0] if found else None
    inodes = set()
    for fd in Path(f'/proc/{pid}/fd').iterdir():
        try:
            match = re.fullmatch(r'socket:\[(\d+)\]', os.readlink(fd))
        except OSError:
            continue
        if match:
            inodes.add(match.group(1))
    for line in Path('/proc/net/unix').read_text().splitlines()[1:]:
        fields = line.split()
        if len(fields) >= 8 and fields[6] in inodes and fields[7].endswith('/hgs-tray'):
            return fields[7]
    return None


def handles_sigterm(pid):
    if MAC:
        return False  # not observable here; the quit event works for every build
    for line in Path(f'/proc/{pid}/status').read_text().splitlines():
        if line.startswith('SigCgt:'):
            return bool(int(line.split()[1], 16) & (1 << (signal.SIGTERM - 1)))
    return False


# --- quitting and starting ------------------------------------------------------

def menu_quit(pid):
    """Click Quit in the tray menu that this process owns (StatusNotifierItem)."""
    try:
        items = run(['qdbus6', 'org.kde.StatusNotifierWatcher', '/StatusNotifierWatcher',
                     'org.kde.StatusNotifierWatcher.RegisteredStatusNotifierItems']).splitlines()
    except (OSError, subprocess.CalledProcessError):
        items = []
    services = []
    for item in items:
        service = item.split('/')[0]
        try:
            owner = int(run(['qdbus6', 'org.freedesktop.DBus', '/org/freedesktop/DBus',
                             'org.freedesktop.DBus.GetConnectionUnixProcessID', service]))
        except (OSError, ValueError, subprocess.CalledProcessError):
            continue
        if owner == pid:
            services.append(service)
    if len(services) != 1:
        raise UpdateError('the running Zerus predates graceful service stops and its tray menu is not reachable; '
                          'quit Zerus from its tray menu, then run the installer again')
    layout = json.loads(run(['busctl', '--user', '--json=short', '--', 'call', services[0], '/MenuBar',
                             'com.canonical.dbusmenu', 'GetLayout', 'iias', '0', '-1', '0']))
    actions = []

    def walk(node):
        if isinstance(node, dict):
            for child in node.values():
                walk(child)
        elif isinstance(node, list):
            if len(node) == 3 and isinstance(node[0], int) and isinstance(node[1], dict):
                if node[1].get('label', {}).get('data', '').replace('&', '') == 'Quit':
                    actions.append(node[0])
            for child in node:
                walk(child)

    walk(layout)
    if len(actions) != 1:
        raise UpdateError('cannot find the Quit action in the running Zerus tray menu; quit it, then run the installer again')
    run(['busctl', '--user', '--', 'call', services[0], '/MenuBar', 'com.canonical.dbusmenu',
         'Event', 'isvu', str(actions[0]), 'clicked', 'i', '0', '0'])


def quit_gui(pid, installed):
    if handles_sigterm(pid):
        os.kill(pid, signal.SIGTERM)
    elif MAC:
        run(['osascript', '-e', f'ignoring application responses\ntell application id "{LABEL}" to quit\nend ignoring'])
    else:
        menu_quit(pid)
    deadline = time.monotonic() + QUIT_SECONDS
    while alive(pid) and time.monotonic() < deadline:
        time.sleep(0.25)
    if alive(pid):
        raise UpdateError(f'Zerus (pid {pid}) did not quit within {QUIT_SECONDS:g} s, for example while a swarm change '
                          'is in progress; it keeps running and nothing was replaced')
    time.sleep(1)
    if gui_pids(installed):
        raise UpdateError('Zerus started again during the update; nothing was replaced')


def start(installed):
    if MAC:
        domain = f'gui/{os.getuid()}'
        subprocess.run(['launchctl', 'bootout', f'{domain}/{LABEL}'], capture_output=True)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and subprocess.run(
                ['launchctl', 'print', f'{domain}/{LABEL}'], capture_output=True).returncode == 0:
            time.sleep(0.25)
        run(['launchctl', 'bootstrap', domain, PLIST])
    else:
        run(['systemctl', '--user', 'start', UNIT])


def wait_started(installed, path, old_pid):
    deadline = time.monotonic() + START_SECONDS
    while time.monotonic() < deadline:
        pids = [pid for pid in gui_pids(installed) if pid != old_pid]
        if len(pids) == 1:
            if path is None:
                time.sleep(2)
                if alive(pids[0]):
                    return pids[0]
            else:
                try:
                    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                        client.settimeout(1)
                        client.connect(path)
                        # The window that was open before the update comes back.
                        client.sendall(b'sessions\n')
                    return pids[0]
                except OSError:
                    pass
        time.sleep(0.5)
    return None


def stop_started(installed):
    for pid in gui_pids(installed):
        os.kill(pid, signal.SIGTERM)
        deadline = time.monotonic() + QUIT_SECONDS
        while alive(pid) and time.monotonic() < deadline:
            time.sleep(0.25)


# --- files and checks -----------------------------------------------------------

def snapshot(destination):
    source = drafts_dir()
    if not source.exists():
        destination.mkdir(mode=0o700)
        return 0
    if any(p.is_symlink() for p in source.rglob('*')):
        raise UpdateError(f'draft storage {source} contains symbolic links; nothing was replaced')
    shutil.copytree(source, destination)
    for p in destination.rglob('*'):
        os.chmod(p, 0o700 if p.is_dir() else 0o600)
    os.chmod(destination, 0o700)
    return sum(1 for _ in destination.rglob('draft.json'))


def changed_drafts(saved):
    """Nonempty drafts saved when the old GUI quit that are missing or different now."""
    current, problems, preserved = drafts_dir(), [], 0
    for before in saved.rglob('draft.json'):
        relative = before.relative_to(saved).parent
        original = json.loads(before.read_text())
        if not (original.get('text') or original.get('attachments') or original.get('form_state')):
            continue
        after = current / relative / 'draft.json'
        if not after.exists():
            problems.append(f'{relative}: missing')
            continue
        restored = json.loads(after.read_text())
        if any(original.get(key) != restored.get(key) for key in ('key', 'text', 'attachments', 'form_state')):
            problems.append(f'{relative}: changed')
            continue
        for attachment in original.get('attachments') or []:
            blob = after.parent / (attachment['sha256'] + '.bin')
            if not blob.exists() or digest(blob) != attachment['sha256']:
                problems.append(f'{relative}: attachment changed')
        preserved += 1
    return preserved, problems


def running_image_matches(pid, installed):
    exe = executable(installed)
    if MAC:
        text = run(['lsof', '-n', '-a', '-p', str(pid), '-d', 'txt', '-F', 'ni'])
        entry = []
        for line in text.splitlines():
            if line.startswith('f'):
                entry = []
            entry.append(line)
            if line == 'n' + str(exe):
                return 'i' + str(exe.stat().st_ino) in entry
        return False
    running = Path(f'/proc/{pid}/exe')
    if os.readlink(running) == str(exe):
        return running.stat().st_ino == exe.stat().st_ino
    # A script GUI: its interpreter runs the installed file.
    return str(exe) in (Path(f'/proc/{pid}/cmdline').read_bytes().decode(errors='replace').split('\0')[:3])


def build_info(path):
    try:
        info = json.loads(run([executable(path), '--build-info'], timeout=30))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        raise UpdateError(f'{path} does not report its build identity: {error}') from None
    if not isinstance(info, dict) or not info.get('version'):
        raise UpdateError(f'{path} reports an invalid build identity')
    return info


def provenance(installed, info, installed_at, preserved, retained):
    record = {'schema': 1, 'sourceCommit': info.get('sourceCommit') or None, 'sourceDirty': info.get('sourceDirty'),
              'guiVersion': info.get('version'), 'guiVersionName': info.get('versionName'),
              'guiSha256': digest(executable(installed)), 'installedAt': installed_at, 'verifiedAt': now(),
              'preservedDrafts': preserved, 'nativeRetained': retained}
    if CLI.exists():
        record['cliSha256'] = digest(CLI)
        try:
            record['cliVersion'] = run([CLI, '--version'], timeout=30)
        except (OSError, subprocess.SubprocessError):
            record['cliVersion'] = None
    if HELPER.exists():
        record['helperSha256'] = digest(HELPER)
    write_json(PROVENANCE, record)


# --- leftovers of earlier updates -------------------------------------------------

def legacy_copies(folder):
    """Installer copies inside a leftover folder, or None when it holds anything else."""
    copies = []
    for root, dirs, files in os.walk(folder):
        base = Path(root)
        depth = len(base.relative_to(folder).parts)
        for name in list(dirs):
            path = base / name
            if path.is_symlink():
                return None
            if MAC and name == 'hgs-tray.app':
                copies.append(path)
                dirs.remove(name)
            elif depth >= 1:
                return None
        for name in files:
            path = base / name
            if MAC and name == '.DS_Store':
                continue
            if path.is_symlink() or name not in LEGACY_FILES:
                return None
            copies.append(path)
    return copies


def in_use(folder):
    """Whether a process of this user runs from, or sits in, the folder."""
    prefix = str(folder) + os.sep
    if MAC:
        return any(line.strip().partition(' ')[2].strip().startswith(prefix)
                   for line in run(['ps', '-x', '-o', 'pid=,comm=']).splitlines())
    for process in Path('/proc').iterdir():
        if not process.name.isdigit():
            continue
        try:
            if process.stat().st_uid != os.getuid():
                continue
            paths = [os.readlink(process / 'exe'), os.readlink(process / 'cwd')]
            paths += [part.decode(errors='replace') for part in (process / 'cmdline').read_bytes().split(b'\0')[:3]]
        except OSError:
            continue
        if any(path.startswith(prefix) for path in paths):
            return True
    return False


def sweep(parent, current):
    """Remove leftover staging folders that are empty or hold only unused installer copies."""
    removed = []
    for folder in sorted(parent.iterdir()):
        if (not folder.name.startswith(LEGACY) or folder == current or folder.is_symlink()
                or not folder.is_dir()):
            continue
        # A concurrent update may still be using a recent folder.
        if time.time() - folder.stat().st_mtime < LEGACY_AGE:
            continue
        copies = legacy_copies(folder)
        if copies is None:
            say(f'kept {folder}: it holds files other than installer copies')
            continue
        if in_use(folder):
            say(f'kept {folder}: a process still uses it')
            continue
        if MAC:
            register = Path('/System/Library/Frameworks/CoreServices.framework/Frameworks/'
                            'LaunchServices.framework/Support/lsregister')
            for bundle in copies:
                if bundle.suffix == '.app' and register.exists():
                    subprocess.run([str(register), '-u', str(bundle)], capture_output=True)
        shutil.rmtree(folder)
        removed.append(folder.name)
    if removed:
        say(f'removed {len(removed)} leftover folders of earlier updates: {", ".join(removed)}')


# --- the update ---------------------------------------------------------------

def replace(stage, installed):
    """Keep the installed GUI as the only rollback copy, then move the stage into place."""
    previous = ROLLBACK / installed.name
    ROLLBACK.mkdir(parents=True, exist_ok=True)
    if previous.exists() or previous.is_symlink():
        remove(previous)
    if installed.exists():
        shutil.move(str(installed), str(previous))
    try:
        os.rename(stage, installed)
    except OSError:
        if previous.exists() and not installed.exists():
            shutil.move(str(previous), str(installed))
        raise
    if MAC:
        run(['codesign', '--verify', '--deep', '--strict', installed])
        register = Path('/System/Library/Frameworks/CoreServices.framework/Frameworks/'
                        'LaunchServices.framework/Support/lsregister')
        if register.exists():
            subprocess.run([str(register), '-f', str(installed)], capture_output=True)


def roll_back(installed):
    previous = ROLLBACK / installed.name
    if not previous.exists():
        return False
    stop_started(installed)
    remove(installed)
    shutil.move(str(previous), str(installed))
    return True


def update(built, installed):
    built, installed = built.resolve(), installed.absolute()
    info = build_info(built)
    if MAC:
        run(['codesign', '--verify', '--deep', '--strict', built])
    installed.parent.mkdir(parents=True, exist_ok=True)
    folder = Path(tempfile.mkdtemp(prefix='.hgs-tray-update-', dir=installed.parent))
    saved = None
    try:
        stage = folder / installed.name
        if MAC:
            shutil.copytree(built, stage, symlinks=True)
            run(['codesign', '--verify', '--deep', '--strict', stage])
        else:
            shutil.copy2(built, stage)
            os.chmod(stage, 0o755)
        expected = digest(executable(stage))
        pids = gui_pids(installed)
        if len(pids) > 1:
            raise UpdateError(f'several copies of Zerus are running ({pids}); quit them, then run the installer again')
        if not pids:
            replace(stage, installed)
            say(f'replaced the stopped GUI with {info.get("versionName", info["version"])}')
            sweep(installed.parent, folder)
            return 0
        pid = pids[0]
        path, before = socket_path(pid), native()
        say(f'quitting Zerus (pid {pid}) so it saves its drafts')
        quit_gui(pid, installed)
        try:
            SNAPSHOTS.mkdir(mode=0o700, parents=True, exist_ok=True)
            saved = Path(tempfile.mkdtemp(prefix='drafts-', dir=SNAPSHOTS))
            os.rmdir(saved)
            snapshot(saved)
            replace(stage, installed)
        except (OSError, subprocess.CalledProcessError, UpdateError) as error:
            # The previous GUI is still installed: bring it back before reporting.
            start(installed)
            raise UpdateError(f'{error}; the previous GUI was started again') from None
        installed_at = now()
        start(installed)
        new = wait_started(installed, path, pid)
        if new is None:
            restored = roll_back(installed)
            if restored:
                start(installed)
                wait_started(installed, path, pid)
            raise UpdateError('the new GUI did not start' + ('; the previous GUI was restored and started'
                                                             if restored else ''))
        problems = []
        if digest(executable(installed)) != expected or not running_image_matches(new, installed):
            problems.append('the running image is not the new build')
        preserved, draft_problems = changed_drafts(saved)
        problems += draft_problems
        after = native()
        replaced = [p for p, value in before.items() if p in after and after[p] != value]
        if replaced:
            problems.append(f'native process identities changed: {replaced}')
        if problems:
            raise UpdateError('the new GUI runs, but ' + '; '.join(problems))
        retained = sum(after.get(p) == value for p, value in before.items())
        provenance(installed, info, installed_at, preserved, retained)
        shutil.rmtree(saved)
        saved = None
        sweep(installed.parent, folder)
        say(f'Zerus {info.get("versionName", info["version"])} runs as pid {new}; {preserved} nonempty drafts kept, '
            f'{retained} native processes untouched; previous GUI kept in {ROLLBACK}')
        return 0
    except UpdateError:
        if saved is not None and saved.exists():
            say(f'drafts saved when Zerus quit remain in {saved}')
        raise
    finally:
        if folder.exists():
            shutil.rmtree(folder)


def main():
    if len(sys.argv) != 3:
        print(__doc__.split('\n\n')[1], file=sys.stderr)
        return 2
    try:
        return update(Path(sys.argv[1]), Path(sys.argv[2]))
    except UpdateError as error:
        print(f'hgs-tray update failed: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
