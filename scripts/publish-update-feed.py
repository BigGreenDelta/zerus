#!/usr/bin/env python3
"""Pull verified GitHub assets; dry-run unless --execute is supplied."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile

if sys.version_info < (3, 11):
    # Checked before the mirror modules load: no incidental traceback.
    print('Mirror requires Python 3.11 or newer; previous catalogs retained',file=sys.stderr);raise SystemExit(1)

from update_feed import (MIRROR_FAILURES, ChannelMismatch, GitHub, candidates, check_android_sequence,
                         check_previous, mirror_lock, publish, require, validate_and_fetch)

CHANNELS={'android/dev','android/stable','android/nightly','desktop/stable','desktop/nightly'}


def token():
    credential=os.environ.get('GITHUB_TOKEN_FILE')
    if credential:
        path=Path(credential)
        require(path.is_file() and not path.is_symlink() and path.stat().st_size<=4096,'Invalid GitHub credential file')
        value=path.read_text().strip()
    else:value=os.environ.get('GH_TOKEN') or os.environ.get('GITHUB_TOKEN')
    require(value is None or 1<=len(value)<=4096 and all(33<=ord(c)<=126 for c in value),'Invalid GitHub credential')
    return value


def mirror_bytes(root):
    total=0
    if not root.exists():return total
    for directory,dirs,files in os.walk(root,followlinks=False):
        require(not any((Path(directory)/name).is_symlink() for name in dirs+files),'Symlinked mirror content')
        for name in files:total+=(Path(directory)/name).stat().st_size
    return total


def synchronize(client,root,channels,execute=False,max_bytes=20*1024**3):
    rows=client.releases();reports=[]
    for selector in channels:
        platform,channel=selector.split('/')
        eligible=candidates(rows,platform,channel);require(eligible,'No published release for '+selector)
        selected=False
        for release in eligible:
            with tempfile.TemporaryDirectory(prefix='zerus-release-') as staging:
                try:feed,files=validate_and_fetch(client,release,Path(staging),platform,channel,root if root.exists() else None)
                except ChannelMismatch:
                    # Only channel mismatch permits selecting another Android
                    # release. Corruption of the selected release always fails.
                    require(platform=='android','Desktop release metadata has wrong channel');continue
                current_path=root/'updates'/'v1'/platform/(channel+'.json')
                check_previous(current_path,feed)
                check_android_sequence(root,feed,current_path)
                target=root/'downloads'/release['tag_name']
                additional=sum(path.stat().st_size for name,path in files.items() if not (target/name).exists())
                require(mirror_bytes(root)+additional+256*1024<=max_bytes,'Retained mirror disk budget exceeded')
                if execute:publish(root,feed,files)
                reports.append({'channel':selector,'releaseId':feed['releaseId'],'version':feed['versionName'],
                                'artifactCount':len(feed['artifacts']),'mode':'published' if execute else 'dry-run'})
                selected=True;break
        require(selected,'No sealed release for '+selector)
    return reports


def load_dev_publisher():
    spec = importlib.util.spec_from_file_location('dev_mirror', Path(__file__).with_name('publish-dev-feed.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def synchronize_channels(root, channels, dev_platforms, execute, max_bytes, credential):
    reports = synchronize(GitHub(credential), root, channels, execute, max_bytes)
    if dev_platforms:
        from dev_updates import DevGitHub
        reports.extend(load_dev_publisher().synchronize(
            DevGitHub(credential), root, dev_platforms, execute, max_bytes))
    return reports


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',required=True,type=Path,help='Dedicated absolute static-content directory')
    parser.add_argument('--channel',action='append',choices=sorted(CHANNELS),required=True)
    parser.add_argument('--dev-platform', action='append', choices=['desktop', 'android'], default=[],
                        help='Also synchronize the tagged CI Dev channel using the same root and lock')
    parser.add_argument('--execute',action='store_true',help='Publish immutable assets and atomic catalogs')
    parser.add_argument('--max-mirror-bytes',type=int,default=20*1024**3,help='Retained disk budget; no automatic deletion')
    args=parser.parse_args()
    try:
        require(args.root.is_absolute() and not args.root.is_symlink(),'Dedicated absolute mirror root required')
        require(args.max_mirror_bytes>0,'Invalid mirror disk budget')
        credential=token()
        channels=list(dict.fromkeys(args.channel))
        dev_platforms=list(dict.fromkeys(args.dev_platform))
        with mirror_lock(args.root,args.execute) as root:
            reports=synchronize_channels(root,channels,dev_platforms,args.execute,args.max_mirror_bytes,credential)
        print(json.dumps({'results':reports},sort_keys=True));return 0
    except BlockingIOError:
        print('Mirror already running',file=sys.stderr);return 1
    except MIRROR_FAILURES:
        # Never print exceptions from network/subprocesses: signed CDN URLs and
        # credential context do not belong in a scheduled job's logs.
        print('Mirror validation or transport failed; previous catalogs retained',file=sys.stderr);return 1
    except Exception as error:
        # Defects keep their text out of logs too; the class name locates them.
        # KeyboardInterrupt and SystemExit are not Exceptions and still propagate.
        print(f'Mirror failed unexpectedly ({type(error).__name__}); previous catalogs retained',file=sys.stderr);return 1


if __name__=='__main__':sys.exit(main())
