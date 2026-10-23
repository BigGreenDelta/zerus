#!/usr/bin/env python3
"""Select explicit CI coverage; uncertain PR inventories keep both extra platforms."""
import os
from pathlib import Path
import re
import subprocess


def platforms(paths):
    windows = ios = False
    if not paths:
        return True, True
    for path in paths:
        if not path or path.startswith('/') or '..' in path.split('/'):
            return True, True
        if path.startswith('mobile/ios/'):
            ios = True
        elif path.startswith(('tray/', 'packaging/windows/')) or path.startswith('scripts/windows'):
            windows = True
        elif path.startswith(('mobile/android/', 'packaging/macos/', 'docs/', 'design/')):
            continue
        elif path.startswith(('scripts/android', 'scripts/macos', 'tests/test_android', 'tests/test_macos')):
            continue
        elif path.endswith('.md') or path in ('LICENSE', 'NOTICE'):
            continue
        else:
            # Rust, shared mobile protocols, fixtures, build/workflow code and
            # unknown paths can affect both clients. Prefer extra coverage.
            windows = ios = True
    return windows, ios


def changed_paths(base, head):
    if not all(re.fullmatch('[0-9a-f]{40}', value or '') for value in (base, head)):
        raise ValueError('Unknown PR revisions')
    ancestor = subprocess.check_output(['git', 'merge-base', base, head], timeout=30, text=True).strip()
    raw = subprocess.check_output(['git', 'diff', '--no-renames', '--name-only', '-z', ancestor, head], timeout=30)
    if len(raw) > 2 * 1024**2 or raw and not raw.endswith(b'\0'):
        raise ValueError('Unknown bounded PR inventory')
    paths = raw[:-1].decode('utf-8').split('\0') if raw else []
    if len(paths) > 10000:
        raise ValueError('PR inventory exceeds bounds')
    return paths


def select(env):
    selected = {}
    for name in ('linux', 'arch', 'macos', 'windows', 'ios'):
        value = env.get('RUN_' + name.upper())
        if value not in ('true', 'false'):
            raise ValueError('Explicit boolean coverage required: ' + name)
        selected['run_' + name] = value == 'true'
    if env.get('FILTER_PR_PLATFORMS') not in ('true', 'false'):
        raise ValueError('Explicit PR selection boolean required')
    if env['FILTER_PR_PLATFORMS'] == 'true':
        if env.get('EVENT_NAME') != 'pull_request':
            raise ValueError('Path selection is only for PR checks')
        try:
            windows, ios = platforms(changed_paths(env.get('PR_BASE'), env.get('PR_HEAD')))
        except (OSError, ValueError, UnicodeError, subprocess.SubprocessError):
            windows = ios = True
            print('PR inventory unavailable; retaining Windows and iOS coverage.')
        # PR path selection may add coverage, never suppress an explicit request.
        selected['run_windows'] |= windows
        selected['run_ios'] |= ios
    return selected


def main():
    chosen = select(os.environ)
    with Path(os.environ['GITHUB_OUTPUT']).open('a') as out:
        for key, enabled in chosen.items():
            out.write(f'{key}={str(enabled).lower()}\n')
    with Path(os.environ['GITHUB_STEP_SUMMARY']).open('a') as out:
        out.write('### CI coverage\n\nShared source, relay, connector and Android checks always run.\n\n')
        for key, enabled in chosen.items():
            out.write(f'- {key}: {str(enabled).lower()}\n')


if __name__ == '__main__':
    main()
