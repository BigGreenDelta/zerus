"""Dev candidates, channel advancement and retention on isolated synthetic data."""
import copy
import datetime as dt
import importlib.util
import hashlib
import io
import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import dev_updates as dev
spec = importlib.util.spec_from_file_location('dev_publisher', Path(dev.__file__).with_name('publish-dev-feed.py'))
publisher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publisher)

COMMIT = 'a' * 40
NOW = dt.datetime(2026, 10, 11, tzinfo=dt.timezone.utc)


def run(number=1):
    return {'id': number + 100, 'run_number': number, 'run_attempt': 1, 'head_sha': COMMIT,
            'head_branch': 'main', 'head_repository': {'full_name': 'ufna/zerus'},
            'path': '.github/workflows/ci.yml', 'event': 'push',
            'status': 'completed', 'conclusion': 'success'}


def candidate(base, platform='desktop', number=1):
    source = base / f'input-{platform}-{number}'
    source.mkdir()
    (source / 'zerus-source.tar.gz').write_bytes(b'Exact synthetic source')
    (source / 'VERSION').write_text('0.39.0')
    apk = None
    if platform == 'desktop':
        (source / 'zerus-0.39.0-1-x86_64.pkg.tar.zst').write_bytes(b'Synthetic checked package')
    else:
        (source / 'zerus.apk').write_bytes(b'Synthetic signed APK')
        apk = {'version_name': f'0.1.28-dev.{number}', 'version_code': 10000+number,
               'package_name': 'app.zerus.mobile', 'min_sdk': 26, 'signing_cert_sha256': 'b'*64}
    sealed = base / f'candidate-{platform}-{number}'
    info = dev.seal(source, sealed, platform, number+100, number, COMMIT, NOW.isoformat(), apk)
    return info, sealed


class FakeGitHub:
    def __init__(self, base, platform='desktop', number=1):
        self.run = run(number)
        self.info, self.path = candidate(base, platform, number)
        self.commit = COMMIT
    def runs(self):
        return [self.run]
    def tag_commit(self, tag):
        assert tag == 'dev'
        return self.commit
    def candidate(self, selected, platform, root):
        for path in self.path.iterdir():
            shutil.copyfile(path, root / path.name)


class DevUpdateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name)
        self.root = self.base / 'mirror'
        self.client = FakeGitHub(self.base)
    def tearDown(self):
        self.temp.cleanup()
    def sync(self, execute=False, platform='desktop'):
        return publisher.synchronize(self.client, self.root, [platform], execute)
    def test_dry_run_has_no_mirror_or_release_side_effects(self):
        self.assertEqual(self.sync()[0]['buildId'], 101)
        self.assertFalse(self.root.exists())
    def test_publish_platform_scoped_feed_and_immutable_bytes(self):
        self.root.mkdir()
        self.sync(True)
        path = self.root / 'updates/v2/desktop/dev.json'
        self.assertEqual(json.loads(path.read_text()), self.client.info)
        before = path.read_bytes()
        self.sync(True)
        self.assertEqual(path.read_bytes(), before)
        asset = self.root / 'downloads/dev-desktop-101/zerus-source.tar.gz'
        asset.write_bytes(b'Corrupt cached source')
        with self.assertRaises(ValueError):
            self.sync(True)
        self.assertEqual(path.read_bytes(), before)
    def test_android_uses_same_package_and_exact_signer(self):
        self.client = FakeGitHub(self.base, 'android')
        result = self.sync(platform='android')
        self.assertEqual(result[0]['platform'], 'android')
        apk = next(a for a in self.client.info['artifacts'] if a['kind'] == 'apk')
        self.assertEqual(apk['packageName'], 'app.zerus.mobile')
        self.assertEqual(apk['signingCertSha256'], 'b'*64)
    def test_current_public_seal_remains_available_after_actions_artifact_expiry(self):
        self.root.mkdir()
        self.sync(True)
        def expired(*args):
            raise AssertionError('Current public bytes must not need an expired Actions download')
        self.client.candidate = expired
        self.client.run['run_attempt'] = 2
        self.assertEqual(self.sync(True)[0]['mode'], 'unchanged')
        self.assertEqual(self.sync()[0]['mode'], 'dry-run')
        marker = self.root / 'downloads/dev-desktop-101/dev-info.json'
        changed = json.loads(marker.read_text())
        changed['publishedAt'] = (NOW + dt.timedelta(days=1)).isoformat()
        marker.write_text(json.dumps(changed))
        with self.assertRaises(ValueError):
            self.sync(True)
    def test_rejects_foreign_failed_manual_pr_and_wrong_workflow(self):
        for key, value in [('head_branch', 'feature'), ('event', 'pull_request'),
                           ('event', 'workflow_dispatch'), ('conclusion', 'failure'),
                           ('status', 'in_progress'), ('path', '.github/workflows/other.yml'),
                           ('head_repository', {'full_name': 'fork/zerus'})]:
            with self.subTest(key=key, value=value):
                bad = copy.deepcopy(run())
                bad[key] = value
                with self.assertRaises(ValueError):
                    dev.identity(bad)
    def test_dev_tag_must_match_successful_candidate(self):
        self.client.commit = 'c'*40
        with self.assertRaises(ValueError):
            self.sync()
        self.assertFalse(self.root.exists())
    def test_seal_mismatch_retains_catalog(self):
        self.root.mkdir()
        self.sync(True)
        path = self.root / 'updates/v2/desktop/dev.json'
        before = path.read_bytes()
        self.client = FakeGitHub(self.base, number=2)
        (self.client.path / 'zerus-source.tar.gz').write_bytes(b'Changed upstream bytes')
        with self.assertRaises(ValueError):
            self.sync(True)
        self.assertEqual(path.read_bytes(), before)
    def test_wrong_platform_build_identity_and_dirty_source_rejected(self):
        for key, value in [('buildId', 999), ('buildNumber', 3), ('sourceCommit', 'c'*40),
                           ('buildUrl', 'https://example.invalid/build'), ('channelTag', 'stable'),
                           ('sourceDirty', True), ('schemaVersion', True)]:
            bad = {**self.client.info, key: value}
            with self.subTest(key=key), self.assertRaises(ValueError):
                dev.validate(bad, self.client.path, self.client.run)
    def test_unlisted_payload_or_wrong_url_cannot_be_published(self):
        extra = self.client.path / 'surprise.txt'
        extra.write_bytes(b'Unexpected file')
        with self.assertRaises(ValueError):
            self.sync()
        extra.unlink()
        bad = copy.deepcopy(self.client.info)
        bad['artifacts'][0]['url'] = 'https://example.invalid/source.tar.gz'
        with self.assertRaises(ValueError):
            dev.validate(bad, self.client.path, self.client.run)
    def test_old_completion_and_same_code_android_replacement_rejected(self):
        self.root.mkdir()
        path = self.root / 'android.json'
        info, _ = candidate(self.base, 'android', 2)
        path.write_text(json.dumps(info))
        older = {**info, 'buildId': 103, 'buildNumber': 1}
        with self.assertRaises(ValueError):
            dev.check_previous(path, older)
        same_code = {**info, 'buildId': 103, 'buildNumber': 3}
        with self.assertRaises(ValueError):
            dev.check_previous(path, same_code)
        changed = {**info, 'versionName': 'changed'}
        with self.assertRaises(ValueError):
            dev.check_previous(path, changed)
    def test_disk_budget_failure_creates_no_catalog(self):
        with self.assertRaises(ValueError):
            publisher.synchronize(self.client, self.root, ['desktop'], max_bytes=1)
        self.assertFalse(self.root.exists())
    def test_zip_traversal_symlink_duplicate_and_expansion_are_rejected(self):
        for kind in ('traversal', 'symlink', 'duplicate', 'directory'):
            raw = io.BytesIO()
            with zipfile.ZipFile(raw, 'w') as output:
                entry = zipfile.ZipInfo('../escape' if kind == 'traversal' else 'fixture')
                if kind == 'symlink':
                    entry.external_attr = (0o120777 << 16)
                if kind == 'directory':
                    entry.filename = 'fixture/'
                output.writestr(entry, b'fixture')
                if kind == 'duplicate':
                    output.writestr('fixture', b'duplicate')
            with tempfile.TemporaryDirectory() as temp, self.subTest(kind=kind), self.assertRaises(ValueError):
                dev.unpack(raw.getvalue(), Path(temp))
        self.assertFalse((self.base / 'escape').exists())
    def test_retention_keeps_three_and_starts_grace_after_retirement(self):
        self.root.mkdir()
        for number in range(1, 6):
            info, path = candidate(self.base, 'desktop', number+10)
            dev.publish(self.root, info, path)
        dev.collect(self.root, now=NOW)
        self.assertEqual(len(list((self.root/'downloads').glob('dev-desktop-*'))), 5)
        dev.collect(self.root, now=NOW + dt.timedelta(hours=71))
        self.assertEqual(len(list((self.root/'downloads').glob('dev-desktop-*'))), 5)
        dev.collect(self.root, now=NOW + dt.timedelta(hours=72))
        self.assertEqual(len(list((self.root/'downloads').glob('dev-desktop-*'))), 3)
        self.assertTrue((self.root/'downloads/dev-desktop-115').is_dir())
    def test_retention_does_not_delete_stable_or_unmarked_or_symlinked_content(self):
        self.root.mkdir()
        downloads = self.root / 'downloads'
        downloads.mkdir()
        stable = downloads / 'v0.39.0'
        stable.mkdir()
        (stable/'source.tar.gz').write_bytes(b'Stable source')
        dev.collect(self.root, now=NOW)
        self.assertTrue(stable.exists())
        bad = downloads/'dev-desktop-999'
        bad.mkdir()
        with self.assertRaises(ValueError):
            dev.collect(self.root, now=NOW)
        self.assertTrue(bad.exists())
    def test_actions_cdn_allowlist_excludes_other_azure_tenants(self):
        client = dev.DevGitHub()
        client.asset_origin('https://productionresultssa10.blob.core.windows.net/actions/file')
        with self.assertRaises(ValueError):
            client.asset_origin('https://untrusted.blob.core.windows.net/file')

    def test_collection_and_dry_run_reject_symlinked_parent_paths(self):
        self.root.mkdir()
        outside = self.base / 'outside'
        outside.mkdir()
        (self.root/'downloads').symlink_to(outside, target_is_directory=True)
        with self.assertRaises(ValueError):
            dev.collect(self.root, now=NOW)
        with self.assertRaises(ValueError):
            self.sync()
        (self.root/'downloads').unlink()
        (self.root/'updates').symlink_to(outside, target_is_directory=True)
        with self.assertRaises(ValueError):
            dev.collect(self.root, now=NOW)
        self.assertEqual(list(outside.iterdir()), [])

    def test_failed_job_retry_preserves_other_platform_and_verifies_archive_digest(self):
        raw = io.BytesIO()
        with zipfile.ZipFile(raw, 'w') as archive:
            for path in self.client.path.iterdir():
                archive.writestr(path.name, path.read_bytes())
        data = raw.getvalue()
        evidence = {'id': 101, 'head_sha': COMMIT, 'head_branch': 'main'}
        def artifact(attempt, identifier):
            return {'id': identifier, 'name': f'zerus-dev-desktop-a{attempt}', 'expired': False,
                    'size_in_bytes': len(data), 'workflow_run': evidence,
                    'digest': 'sha256:' + hashlib.sha256(data).hexdigest()}
        rows = {'total_count': 1, 'artifacts': [artifact(1, 123)]}
        class Client(dev.DevGitHub):
            def api(self, path):
                return rows
            def stream(self, url, *args, **kwargs):
                self.downloaded = url
                yield data
        client = Client()
        retry = {**run(), 'run_attempt': 2}
        with tempfile.TemporaryDirectory() as temp:
            client.candidate(retry, 'desktop', Path(temp))
            self.assertTrue(client.downloaded.endswith('/123/zip'))
        rows['artifacts'].append(artifact(2, 124))
        rows['total_count'] = 2
        with tempfile.TemporaryDirectory() as temp:
            client.candidate(retry, 'desktop', Path(temp))
            self.assertTrue(client.downloaded.endswith('/124/zip'))
        rows['artifacts'][1]['digest'] = 'sha256:' + '0'*64
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaises(ValueError):
                client.candidate(retry, 'desktop', Path(temp))
            self.assertEqual(list(Path(temp).iterdir()), [])


if __name__ == '__main__':
    unittest.main()
