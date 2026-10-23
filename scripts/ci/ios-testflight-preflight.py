#!/usr/bin/env python3
"""Read-only App Store Connect duplicate check before any archive or keychain import.

Uses Apple's GET /v1/apps and GET /v1/builds, a two-minute GET-only ES256 token,
and the existing OpenSSL. Unknown/failed/expired records refuse an upload.
"""
import base64
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time
import urllib.parse
import urllib.request


def encode(value):
    raw = json.dumps(value, separators=(',', ':')).encode() if isinstance(value, dict) else value
    return base64.urlsafe_b64encode(raw).rstrip(b'=')


def raw_signature(der):
    # OpenSSL emits a short DER sequence of exactly two positive P-256 integers.
    if not 8 <= len(der) <= 72 or der[:2] != bytes((0x30, len(der) - 2)):
        raise ValueError('Invalid ES256 signature')
    offset, values = 2, []
    for _ in range(2):
        if offset + 2 > len(der) or der[offset] != 2:
            raise ValueError('Invalid ES256 integer')
        size = der[offset + 1]
        offset += 2
        value = der[offset:offset + size]
        offset += size
        if not 1 <= size <= 33 or len(value) != size or value[0] & 0x80:
            raise ValueError('Invalid ES256 integer size/sign')
        if size > 1 and value[0] == 0 and not value[1] & 0x80:
            raise ValueError('Noncanonical ES256 integer')
        integer = int.from_bytes(value, 'big')
        if not 0 < integer < 2**256:
            raise ValueError('Invalid ES256 scalar')
        values.append(integer.to_bytes(32, 'big'))
    if offset != len(der):
        raise ValueError('Trailing ES256 bytes')
    return b''.join(values)


def token(key, key_id, issuer, now):
    header = encode({'alg': 'ES256', 'kid': key_id, 'typ': 'JWT'})
    payload = encode({'iss': issuer, 'iat': now, 'exp': now + 120, 'aud': 'appstoreconnect-v1',
                      'scope': ['GET /v1/apps', 'GET /v1/builds']})
    body = header + b'.' + payload
    signature = subprocess.check_output(['openssl', 'dgst', '-sha256', '-sign', str(key)],
                                        input=body, stderr=subprocess.PIPE, timeout=10)
    return (body + b'.' + encode(raw_signature(signature))).decode('ascii')


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise ValueError('ASC read must not redirect')


def get(endpoint, query, bearer):
    url = 'https://api.appstoreconnect.apple.com/v1/' + endpoint + '?' + urllib.parse.urlencode(query)
    request = urllib.request.Request(url, headers={'Authorization': 'Bearer ' + bearer})
    with urllib.request.build_opener(NoRedirect).open(request, timeout=20) as response:
        raw = response.read(512 * 1024 + 1)
    if len(raw) > 512 * 1024:
        raise ValueError('ASC read exceeds bounds')
    data = json.loads(raw)
    if not isinstance(data.get('data'), list) or data.get('links', {}).get('next'):
        raise ValueError('Incomplete ASC read')
    return data['data']


def upload_needed(apps, builds, number):
    if (len(apps) != 1 or apps[0].get('type') != 'apps'
            or apps[0].get('attributes', {}).get('bundleId') != 'app.zerus.mobile'):
        raise ValueError('Exact Zerus app not found')
    if not builds:
        return True
    if len(builds) != 1:
        raise ValueError('Ambiguous existing iOS build')
    build = builds[0]
    attrs = build.get('attributes', {})
    if (build.get('type') != 'builds' or attrs.get('version') != str(number)
            or attrs.get('expired') is not False or attrs.get('processingState') not in ('VALID', 'PROCESSING')):
        raise ValueError('Existing iOS build is unknown, failed or expired; do not reupload its number')
    return False


def main():
    number = os.environ['BUILD_NUMBER']
    channel = os.environ['CHANNEL']
    if channel not in ('dev', 'stable') or not re.fullmatch('[1-9][0-9]{0,8}', number):
        raise ValueError('Exact iOS channel/CI sequence required')
    version = (Path(__file__).resolve().parents[2] / 'mobile/ios' /
               ('DEV_VERSION' if channel == 'dev' else 'VERSION')).read_text().strip()
    if not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', version):
        raise ValueError('Invalid iOS marketing version')
    key_id, issuer = os.environ['ASC_KEY_ID'], os.environ['ASC_ISSUER_ID']
    if not re.fullmatch('[A-Z0-9]{8,16}', key_id) or not re.fullmatch('[0-9a-f-]{36}', issuer):
        raise ValueError('Invalid ASC key identity')
    with tempfile.TemporaryDirectory(prefix='zerus-asc-read-') as root:
        key = Path(root) / 'key.p8'
        with key.open('x') as out:
            os.chmod(key, 0o600)
            out.write(os.environ['ASC_KEY'])
        bearer = token(key, key_id, issuer, int(time.time()))
        apps = get('apps', {'filter[bundleId]': 'app.zerus.mobile', 'limit': '2'}, bearer)
        if len(apps) != 1 or not re.fullmatch('[0-9]{1,20}', apps[0].get('id', '')):
            raise ValueError('Exact ASC app identity unavailable')
        builds = get('builds', {'filter[app]': apps[0]['id'], 'filter[version]': number,
                               'filter[preReleaseVersion.version]': version,
                               'filter[preReleaseVersion.platform]': 'IOS', 'limit': '2'}, bearer)
        needed = upload_needed(apps, builds, number)
    with Path(os.environ['GITHUB_OUTPUT']).open('a') as out:
        out.write(f'upload={str(needed).lower()}\n')
    print('iOS build number is unused; archive may proceed.' if needed else
          'iOS build already exists in App Store Connect; no archive or repeat upload.')


if __name__ == '__main__':
    main()
