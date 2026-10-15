#!/usr/bin/env python3
"""Publish the Dev cask only after the exact signed candidate is public on the mirror."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import tempfile

from dev_updates import DevGitHub, identity, validate
from macos_release import render_cask
from update_feed import GitHub, ORIGIN, REPOSITORY, https, require

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('release_publisher', ROOT / 'scripts/publish-release.py')
publisher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publisher)


class PublicMirror(GitHub):
    def asset_origin(self, url):
        return https(url, {'zerus.dev'})


def verified_candidate(client, public, run_id, root):
    run = identity(client.api(f'/repos/{REPOSITORY}/actions/runs/{run_id}'))
    require(run['id'] == run_id, 'Candidate run mismatch')
    client.candidate(run, 'desktop', root)
    manifest = root / 'dev-info.json'
    require(manifest.stat().st_size <= 256*1024, 'Oversized Dev seal')
    info = validate(json.loads(manifest.read_text()), root, run)
    require(info['platform'] == 'desktop', 'Wrong candidate platform')
    if 'macos' not in info:
        return None
    current = json.loads(b''.join(public.stream(ORIGIN + '/updates/v2/desktop/dev.json', 256*1024)))
    require(isinstance(current, dict) and current.get('schemaVersion') == 2
            and current.get('platform') == 'desktop' and current.get('channel') == 'dev'
            and type(current.get('buildNumber')) is int, 'Invalid public Dev catalog')
    if current['buildNumber'] > run['run_number']:
        return None
    require(current == info, 'The exact candidate is not publicly mirrored')
    for item in info['macos']['downloads']:
        digest, size = hashlib.sha256(), 0
        for block in public.stream(item['url'], item['size'], accept='application/octet-stream'):
            digest.update(block)
            size += len(block)
        require(size == item['size'] and digest.hexdigest() == item['sha256'],
                'Public macOS bytes differ from the verified CI candidate')
    return info


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-id', type=int, required=True)
    parser.add_argument('--tap-ssh-key', type=Path, required=True)
    args = parser.parse_args()
    require(0 < args.run_id < 2**53, 'Invalid CI run ID')
    with tempfile.TemporaryDirectory(prefix='zerus-dev-cask-') as temporary:
        workspace = Path(temporary)
        candidate = workspace / 'candidate'
        candidate.mkdir()
        info = verified_candidate(DevGitHub(os.environ.get('GH_TOKEN')), PublicMirror(), args.run_id, candidate)
        if info is None:
            print('No current macOS candidate to publish; the Homebrew tap is unchanged.')
            return 0
        archive = info['macos']['downloads'][0]
        (candidate / 'homebrew').mkdir()
        (candidate / 'homebrew/zerus@dev.rb').write_text(render_cask(
            info['versionName'], archive['sha256'], 'dev', f'dev-desktop-{info["buildId"]}'))
        _, env = publisher.ssh_environment(args.tap_ssh_key, ROOT / 'packaging/homebrew/known_hosts',
                                           publisher.GITHUB_FINGERPRINT, 'GitHub')
        plan = publisher.prepare_tap(candidate, workspace, env, info['versionName'], 'dev')
        if plan is not None:
            publisher.git(plan, 'push', 'origin', 'HEAD:main', env=env)
        print(f'Homebrew zerus@dev {info["versionName"]}: public signed bytes verified and cask synchronized.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
