#!/usr/bin/env python3
"""Bounded forced command: use one CI job's read token without retaining it."""
import argparse
import fcntl
from importlib.util import module_from_spec, spec_from_file_location
import json
from pathlib import Path
import sys

if sys.version_info < (3, 11):
    # Checked before the mirror modules load. Exit 1: retrying cannot help, and
    # only exit 75 asks Publish Dev to retry.
    print('Dev publication requires Python 3.11 or newer; previous catalogs retained', file=sys.stderr)
    raise SystemExit(1)

from dev_updates import DevGitHub, identity
from update_feed import MIRROR_FAILURES, REPOSITORY, private_root, require


def publisher():
    spec = spec_from_file_location('dev_mirror', Path(__file__).with_name('publish-dev-feed.py'))
    module = module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def receive(raw, root, max_bytes=20*1024**3):
    require(len(raw) <= 8192, 'Oversized publication request')
    request = json.loads(raw)
    require(isinstance(request, dict) and set(request) == {'runId', 'token'}, 'Invalid publication request')
    run_id = request['runId']
    credential = request['token']
    require(type(run_id) is int and 0 < run_id < 2**53, 'Invalid CI run identity')
    require(isinstance(credential, str) and 1 <= len(credential) <= 4096
            and all(33 <= ord(c) <= 126 for c in credential), 'Invalid job credential')
    require(root.is_absolute() and not root.is_symlink(), 'Dedicated absolute mirror root required')
    require(max_bytes > 0, 'Invalid mirror budget')
    client = DevGitHub(credential)
    run = identity(client.api(f'/repos/{REPOSITORY}/actions/runs/{run_id}'))
    require(run['id'] == run_id, 'CI run identity mismatch')
    root = private_root(root)
    lock = root / '.mirror.lock'
    require(not lock.is_symlink(), 'Symlinked mirror lock')
    with lock.open('a') as handle:
        fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if client.tag_commit('dev') != run['head_sha']:
            return [{'buildId': run_id, 'mode': 'superseded'}]
        return publisher().synchronize(client, root, ['desktop', 'android'], True, max_bytes)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    args = parser.parse_args()
    try:
        result = receive(sys.stdin.buffer.read(8193), args.root)
        print(json.dumps({'results': result}, sort_keys=True))
        return 0
    except BlockingIOError:
        print('Mirror busy; retry the same verified CI run', file=sys.stderr)
        return 75
    except MIRROR_FAILURES:
        # A job token or signed CDN URL must never enter publication logs.
        print('Dev publication validation or transport failed; previous catalogs retained', file=sys.stderr)
        return 1
    except Exception as error:
        # Publish Dev logs are public: defects report only their class name.
        # Exit 1 like any failure; only a busy mirror (75) is retried.
        print(f'Dev publication failed unexpectedly ({type(error).__name__}); previous catalogs retained',
              file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
