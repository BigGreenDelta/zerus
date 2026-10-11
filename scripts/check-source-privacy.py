#!/usr/bin/env python3
"""Check tracked/new source files without printing matching secret values.

This is a portable-path guard, not a replacement for Gitleaks or manual review.
Third-party copyright/author attribution is intentionally preserved.
"""
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SYNTHETIC_USERS = {'example', 'user', 'test', 'test user', 'remote', 'alice', 'bob'}
HOME_PATH = re.compile(r'/(?:Users|home)/([^/"\'`\n<>]+)(?:/|["\'`])')
PRIVATE_KEY = re.compile(rb'-----BEGIN (?:[A-Z0-9]+ )?PRIVATE KEY-----')
EMAIL = re.compile(r'(?<![\w.+-])[\w.+-]+@(?:[\w-]+\.)+[A-Za-z]{2,}')
# The owner explicitly approved this public maintainer address for AUR/Git.
PUBLIC_CONTACTS = {'ufna@ufna.dev'}
# Vendored third-party code and verbatim license texts keep their authors'
# contact addresses.
THIRD_PARTY = ('tray/vendor/', 'packaging/windows/licenses/')
# Beads attributes records to the Git identities the team works with; the owner
# accepted those addresses there. Free text in the same records is still checked.
BEADS_ATTRIBUTION = {'owner', 'created_by', 'assignee', 'author'}


def without_attribution(value):
    if isinstance(value, dict):
        return {key: without_attribution(item) for key, item in value.items() if key not in BEADS_ATTRIBUTION}
    if isinstance(value, list):
        return [without_attribution(item) for item in value]
    return value


def findings(name, text):
    """Line numbers and reasons for one decoded text file."""
    problems = []
    for line_no, line in enumerate(text.splitlines(), 1):
        addresses = line
        if name.endswith('.jsonl'):
            # Beads escapes angle brackets as Unicode; decode before scanning
            # so the escape cannot become part of a maintainer email address.
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                pass
            else:
                line = addresses = json.dumps(record, ensure_ascii=False)
                if name.startswith('.beads/'):
                    addresses = json.dumps(without_attribution(record), ensure_ascii=False)
        for match in HOME_PATH.finditer(line):
            if match[1] not in SYNTHETIC_USERS:
                problems.append((line_no, 'non-example absolute home path'))
        if re.search(r'(?<![A-Za-z0-9])/' + 'w/', line):
            problems.append((line_no, 'absolute workspace path'))
        if not name.startswith(THIRD_PARTY):
            for address in EMAIL.findall(addresses):
                domain = address.rsplit('@', 1)[1].lower()
                if address in PUBLIC_CONTACTS | {'git@github.com', 'aur@aur.archlinux.org'} or domain.endswith(('.png', '.svg', '.icns')):
                    continue
                if not (domain in {'example.com', 'example.org', 'example.net', 'example.test',
                                   'users.noreply.github.com'} or domain.endswith(('.example', '.test', '.invalid'))):
                    problems.append((line_no, 'non-example email address'))
    return problems


def main():
    paths = subprocess.check_output(
        ['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard'], cwd=ROOT
    ).decode().split('\0')
    problems = []
    checked = 0
    for name in sorted(set(filter(None, paths))):
        path = ROOT / name
        if not path.is_file():
            continue
        checked += 1
        if path.name in {'.env', 'id_rsa', 'id_ed25519', 'credentials.json', 'auth.json'}:
            problems.append((name, 0, 'credential filename'))
        data = path.read_bytes()
        if PRIVATE_KEY.search(data):
            problems.append((name, 0, 'private key material'))
        # Compressed image bytes can accidentally resemble paths/email addresses.
        # Binary assets need a separate metadata/visual audit.
        if b'\0' in data:
            continue
        problems += [(name, line, reason) for line, reason in findings(name, data.decode('utf-8', errors='replace'))]
    for name, line, reason in sorted(set(problems)):
        print(f'{name}:{line}: {reason}')
    print(f'Checked {checked} source files; {len(set(problems))} privacy guard findings.')
    return bool(problems)


if __name__ == '__main__':
    sys.exit(main())
