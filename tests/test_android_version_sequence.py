"""One APK identity advances safely across every supported publication channel."""
import fcntl
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import android_version_code as codes
import update_feed as feeds
from test_dev_updates import FakeGitHub as DevGitHub, publisher as dev_publisher
from test_update_feed import FakeGitHub as ReleaseGitHub, publisher as release_publisher


class AndroidVersionSequenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name)
        self.root = self.base / 'mirror'

    def tearDown(self):
        self.temp.cleanup()

    def catalog(self, schema, channel, code):
        path = self.root / 'updates' / f'v{schema}' / 'android' / (channel + '.json')
        path.parent.mkdir(parents=True, exist_ok=True)
        value = {'schemaVersion': schema, 'platform': 'android', 'channel': channel, 'versionCode': code}
        path.write_text(json.dumps(value))
        return path, value

    def stable(self, code, number=1):
        client = ReleaseGitHub(android=True)
        version = f'0.1.{30+number}'
        client.info.update(channel='stable', source_dirty=False, version_name=version, version_code=code)
        client.info.pop('source_snapshot')
        tag = 'android-v' + version
        client.release.update(id=1000+number, tag_name=tag, prerelease=False,
                              html_url='https://github.com/ufna/zerus/releases/tag/' + tag)
        client.seal()
        return client

    def development(self, number, code=None):
        client = DevGitHub(self.base, 'android', number)
        if code is not None:
            client.info['versionCode'] = code
            (client.path / 'dev-info.json').write_text(json.dumps(client.info))
        return client

    def test_reproducible_codes_advance_stable_dev_stable_dev(self):
        previous_dev = codes.dev_version_code(31, 294)
        self.assertEqual(previous_dev, 10294)
        stable = previous_dev + 1
        development = codes.dev_version_code(stable, 295)
        next_stable = development + 1
        next_dev = codes.dev_version_code(next_stable, 296)
        self.assertEqual([previous_dev, stable, development, next_stable, next_dev],
                         sorted(set([previous_dev, stable, development, next_stable, next_dev])))
        self.assertEqual(codes.dev_version_code(stable, 295), development)  # Retry keeps its sealed code.

    def test_invalid_and_overflowing_inputs_fail_before_gradle(self):
        for value in [0, -1, True, '31', None, codes.MAX_CODE+1]:
            with self.subTest(value=value):
                with self.assertRaises(ValueError): codes.dev_version_code(value, 1)
                with self.assertRaises(ValueError): codes.dev_version_code(31, value)
        with self.assertRaises(ValueError): codes.dev_version_code(codes.MAX_CODE, 1)
        self.assertEqual(codes.dev_version_code(codes.MAX_CODE-1, 1), codes.MAX_CODE)
        path = self.base / 'VERSION_CODE'
        for value in ['0', '-1', '00031', 'true', '10295\nextra', str(codes.MAX_CODE+1)]:
            path.write_text(value)
            with self.subTest(value=value), self.assertRaises(ValueError): codes.stable_version_code(path)

    def test_missing_feeds_allow_a_new_candidate_without_writing(self):
        candidate = {'platform': 'android', 'versionCode': 10295}
        feeds.check_android_sequence(self.root, candidate, self.root / 'updates/v1/android/stable.json')
        self.assertFalse(self.root.exists())

    def test_every_supported_channel_contributes_to_the_floor(self):
        for schema, channel in [(1, 'stable'), (1, 'dev'), (1, 'nightly'), (2, 'dev')]:
            path, _ = self.catalog(schema, channel, 10295)
            for code in [10294, 10295]:
                with self.subTest(schema=schema, channel=channel, code=code), self.assertRaises(ValueError):
                    feeds.check_android_sequence(self.root, {'platform': 'android', 'versionCode': code},
                                                 self.root / 'updates/v1/android/new.json')
            feeds.check_android_sequence(self.root, {'platform': 'android', 'versionCode': 10296},
                                         self.root / 'updates/v1/android/new.json')
            path.unlink()

    def test_malformed_wrong_scope_or_invalid_codes_fail_closed(self):
        path, original = self.catalog(1, 'stable', 10295)
        values = [[], {'schemaVersion': 2}, {**original, 'platform': 'desktop'},
                  {**original, 'channel': 'dev'}, {**original, 'schemaVersion': True}]
        values += [{**original, 'versionCode': code} for code in [True, '10295', 0, -1, codes.MAX_CODE+1]]
        for value in values:
            path.write_text(json.dumps(value))
            with self.subTest(value=value), self.assertRaises(ValueError):
                feeds.check_android_sequence(self.root, {'platform': 'android', 'versionCode': 10300},
                                             self.root / 'updates/v2/android/dev.json')
        for raw in ['{', 'x' * (256*1024+1)]:
            path.write_text(raw)
            with self.assertRaises(ValueError):
                feeds.check_android_sequence(self.root, {'platform': 'android', 'versionCode': 10300},
                                             self.root / 'updates/v2/android/dev.json')

    def test_symlinked_catalog_or_ancestor_cannot_bypass_observation(self):
        path, _ = self.catalog(1, 'stable', 10295)
        outside = self.base / 'outside.json'
        path.rename(outside); path.symlink_to(outside)
        with self.assertRaises(ValueError):
            feeds.check_android_sequence(self.root, {'platform': 'android', 'versionCode': 10300}, path)
        path.unlink()
        parent = self.root / 'updates' / 'v1' / 'android'
        moved = self.base / 'catalogs'; parent.rename(moved); parent.symlink_to(moved, target_is_directory=True)
        with self.assertRaises(ValueError):
            feeds.check_android_sequence(self.root, {'platform': 'android', 'versionCode': 10300}, path)

    def test_actual_publications_advance_across_stable_dev_stable_dev(self):
        self.root.mkdir()
        first = self.development(294)
        dev_publisher.synchronize(first, self.root, ['android'], True)
        stable_code = first.info['versionCode'] + 1
        release_publisher.synchronize(self.stable(stable_code), self.root, ['android/stable'], True)
        second = self.development(295, codes.dev_version_code(stable_code, 295))
        dev_publisher.synchronize(second, self.root, ['android'], True)
        release_publisher.synchronize(self.stable(second.info['versionCode']+1, 2), self.root, ['android/stable'], True)
        third = self.development(296, codes.dev_version_code(second.info['versionCode']+1, 296))
        dev_publisher.synchronize(third, self.root, ['android'], True)
        self.assertEqual(json.loads((self.root/'updates/v2/android/dev.json').read_text())['versionCode'], third.info['versionCode'])

    def test_old_source_dev_and_new_stable_both_observe_the_other_channel_in_dry_run(self):
        self.root.mkdir()
        release_publisher.synchronize(self.stable(10295), self.root, ['android/stable'], True)
        older = self.development(294)
        with self.assertRaises(ValueError): dev_publisher.synchronize(older, self.root, ['android'])
        with self.assertRaises(ValueError): dev_publisher.synchronize(older, self.root, ['android'], True)
        self.assertFalse((self.root/'updates/v2/android/dev.json').exists())
        self.catalog(2, 'dev', 10300)
        for code in [10299, 10300]:
            with self.subTest(code=code), self.assertRaises(ValueError):
                release_publisher.synchronize(self.stable(code, 2), self.root, ['android/stable'])

    def test_new_candidate_loses_to_a_higher_publication_in_either_race_order(self):
        self.root.mkdir()
        release_publisher.synchronize(self.stable(10300), self.root, ['android/stable'], True)
        obsolete = self.development(295)  # Built from old source: code 10295.
        with self.assertRaises(ValueError): dev_publisher.synchronize(obsolete, self.root, ['android'], True)
        newer = self.development(296, 10301)
        dev_publisher.synchronize(newer, self.root, ['android'], True)
        with self.assertRaises(ValueError):
            release_publisher.synchronize(self.stable(10300, 2), self.root, ['android/stable'], True)

    def test_exact_current_stable_and_dev_revalidate_after_the_other_channel_advances(self):
        self.root.mkdir()
        stable = self.stable(10295)
        release_publisher.synchronize(stable, self.root, ['android/stable'], True)
        developer = self.development(295, 10590)
        dev_publisher.synchronize(developer, self.root, ['android'], True)
        release_publisher.synchronize(stable, self.root, ['android/stable'], True)
        newest_stable = self.stable(10591, 2)
        release_publisher.synchronize(newest_stable, self.root, ['android/stable'], True)
        dev_publisher.synchronize(developer, self.root, ['android'], True)
        target = self.root/'downloads'/f'dev-android-{developer.run["id"]}'/'zerus.apk'
        target.write_bytes(b'Changed retained APK')
        with self.assertRaises(ValueError): dev_publisher.synchronize(developer, self.root, ['android'], True)
        (self.root/'downloads'/newest_stable.release['tag_name']/'zerus.apk').write_bytes(b'Changed stable APK')
        with self.assertRaises(ValueError):
            release_publisher.synchronize(newest_stable, self.root, ['android/stable'], True)

    def test_floor_is_observed_under_the_existing_shared_lock_for_both_publishers(self):
        developer = self.development(295, 10590)
        stable = self.stable(10591)
        original = feeds.check_android_sequence
        def locked(root, value, current):
            with (root/'.mirror.lock').open('a') as handle:
                with self.assertRaises(BlockingIOError): fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return original(root, value, current)
        for flags in [['--execute'], []]:
            with patch.object(dev_publisher, 'DevGitHub', return_value=developer), \
                 patch.object(dev_publisher, 'check_android_sequence', side_effect=locked) as guard, \
                 patch.object(sys, 'argv', ['publisher', '--root', str(self.root), '--platform', 'android', *flags]):
                self.assertEqual(dev_publisher.main(), 0)
                guard.assert_called_once()
            with patch.object(release_publisher, 'GitHub', return_value=stable), \
                 patch.object(release_publisher, 'check_android_sequence', side_effect=locked) as guard, \
                 patch.object(sys, 'argv', ['publisher', '--root', str(self.root), '--channel', 'android/stable', *flags]):
                self.assertEqual(release_publisher.main(), 0)
                guard.assert_called_once()

    def test_same_release_id_cannot_replace_sealed_bytes_after_other_channel_advances(self):
        self.root.mkdir()
        stable = self.stable(10295)
        release_publisher.synchronize(stable, self.root, ['android/stable'], True)
        developer = self.development(295, 10590)
        dev_publisher.synchronize(developer, self.root, ['android'], True)
        stable.payload['zerus.apk'] = b'Different upstream APK with the same release ID'
        stable.seal()
        with self.assertRaises(ValueError):
            release_publisher.synchronize(stable, self.root, ['android/stable'], True)
        marker = self.root/'downloads'/f'dev-android-{developer.run["id"]}'/'dev-info.json'
        changed = json.loads(marker.read_text())
        changed['versionCode'] += 1
        marker.write_text(json.dumps(changed))
        with self.assertRaises(ValueError):
            dev_publisher.synchronize(developer, self.root, ['android'], True)


if __name__ == '__main__':
    unittest.main()
