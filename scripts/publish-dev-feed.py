#!/usr/bin/env python3
"""Mirror verified dev CI artifacts; dry-run unless --execute is supplied."""
import argparse
import importlib.util
import json
from pathlib import Path
import tempfile
import zipfile

from dev_updates import DevGitHub, check_previous, collect, identity, publish, scoped_path, validate
from update_feed import check_android_sequence, mirror_lock, require

spec = importlib.util.spec_from_file_location('release_mirror', Path(__file__).with_name('publish-update-feed.py'))
mirror = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mirror)


def synchronize(client, root, platforms, execute=False, max_bytes=20*1024**3):
    runs = sorted((identity(r) for r in client.runs()), key=lambda r: r['run_number'], reverse=True)
    require(runs, 'No successful dev build')
    commit = client.tag_commit('dev')
    tagged = [r for r in runs if r['head_sha'] == commit]
    require(tagged, 'Dev tag does not name a successful CI candidate')
    run = tagged[0]
    reports = []
    if execute:
        collect(root)
    for platform in platforms:
        feed = scoped_path(root, 'updates', 'v2', platform, 'dev.json')
        target = scoped_path(root, 'downloads', f'dev-{platform}-{run["id"]}')
        if feed.exists():
            require(feed.is_file() and feed.stat().st_size <= 256*1024, 'Invalid existing dev catalog')
            cached = json.loads(feed.read_text())
            require(cached.get('platform') == platform and cached.get('channel') == 'dev'
                    and cached.get('schemaVersion') == 2, 'Existing dev catalog scope mismatch')
            if cached.get('buildId') == run['id']:
                # The current public seal outlives expiring Actions storage.
                # Verify every retained byte; never rebuild an already public ID.
                marker = scoped_path(target, 'dev-info.json')
                require(marker.is_file() and marker.stat().st_size <= 256*1024
                        and json.loads(marker.read_text()) == cached, 'Stored dev seal differs from catalog')
                validate(cached, target, run)
                check_android_sequence(root, cached, feed)
                reports.append({'platform': platform, 'buildId': run['id'], 'version': cached['versionName'],
                                'mode': 'unchanged' if execute else 'dry-run'})
                continue
        with tempfile.TemporaryDirectory(prefix='zerus-dev-') as temp:
            staging = Path(temp)
            client.candidate(run, platform, staging)
            require((staging/'dev-info.json').stat().st_size <= 256*1024, 'Oversized dev metadata')
            info = validate(json.loads((staging/'dev-info.json').read_text()), staging, run)
            require(info['platform'] == platform, 'Wrong dev platform')
            check_previous(feed, info)
            check_android_sequence(root, info, feed)
            additional = sum(p.stat().st_size for p in staging.iterdir() if not (target/p.name).exists())
            require(mirror.mirror_bytes(root) + additional + 256*1024 <= max_bytes, 'Dev mirror disk budget exceeded')
            if execute:
                publish(root, info, staging)
                collect(root)
            reports.append({'platform': platform, 'buildId': run['id'], 'version': info['versionName'],
                            'mode': 'published' if execute else 'dry-run'})
    return reports


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--platform', choices=['desktop', 'android'], action='append', required=True)
    parser.add_argument('--execute', action='store_true')
    parser.add_argument('--max-mirror-bytes', type=int, default=20*1024**3)
    args = parser.parse_args()
    try:
        require(args.root.is_absolute() and not args.root.is_symlink(), 'Dedicated absolute root required')
        require(args.max_mirror_bytes > 0, 'Invalid mirror budget')
        client = DevGitHub(mirror.token())
        with mirror_lock(args.root, args.execute) as root:
            result = synchronize(client, root, list(dict.fromkeys(args.platform)), args.execute, args.max_mirror_bytes)
        print(json.dumps({'results': result}, sort_keys=True))
        return 0
    except (ValueError, OSError, KeyError, TypeError, UnicodeError, zipfile.BadZipFile):
        print('Dev mirror validation or transport failed; previous catalogs retained')
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
