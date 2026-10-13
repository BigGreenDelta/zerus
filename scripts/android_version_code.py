"""Derive reproducible Android Dev codes from the sealed source and main CI sequence."""
import argparse
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
MAX_CODE = 2_100_000_000


def stable_version_code(path):
    value = path.read_text().strip()
    if not re.fullmatch(r'[1-9][0-9]{0,9}', value) or int(value) > MAX_CODE:
        raise ValueError('Invalid committed Android version code')
    return int(value)


def dev_version_code(stable_code, build_number):
    if type(stable_code) is not int or not 0 < stable_code <= MAX_CODE:
        raise ValueError('Invalid Stable version code')
    if type(build_number) is not int or not 0 < build_number <= MAX_CODE:
        raise ValueError('Invalid main CI build number')
    code = max(10_000, stable_code) + build_number
    if code > MAX_CODE:
        raise ValueError('Android Dev version code exceeds the supported range')
    return code


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dev-build-number', required=True, type=int)
    args = parser.parse_args()
    try:
        print(dev_version_code(stable_version_code(ROOT / 'mobile/android/VERSION_CODE'), args.dev_build_number))
    except (ValueError, OSError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
