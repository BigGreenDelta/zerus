#!/usr/bin/env python3
"""Seal CI output; no publication, signing, or rebuilding."""
import argparse
import importlib.util
from pathlib import Path
from dev_updates import seal

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--input', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--platform', choices=['android', 'desktop'], required=True)
parser.add_argument('--run-id', type=int, required=True)
parser.add_argument('--run-number', type=int, required=True)
parser.add_argument('--source-commit', required=True)
parser.add_argument('--published-at', required=True)
parser.add_argument('--aapt2', type=Path)
parser.add_argument('--apksigner', type=Path)
args = parser.parse_args()
apk = None
if args.platform == 'android':
    spec = importlib.util.spec_from_file_location('mobile_release', Path(__file__).with_name('prepare-mobile-release.py'))
    mobile = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mobile)
    paths = list(args.input.glob('*.apk'))
    if len(paths) != 1 or not args.aapt2 or not args.apksigner:
        raise SystemExit('One APK and its verification tools are required.')
    apk = mobile.apk_metadata(paths[0], args.aapt2, args.apksigner)
seal(args.input, args.output, args.platform, args.run_id, args.run_number,
     args.source_commit, args.published_at, apk)
if apk:
    if mobile.apk_metadata(args.output / paths[0].name, args.aapt2, args.apksigner) != apk:
        raise SystemExit('Frozen APK identity differs from the verified build.')
