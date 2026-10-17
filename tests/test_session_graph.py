"""Bounded graph projection in private homes; no real agents or read markers."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

HGS = Path(os.environ.get('HGS_TEST_BIN', Path(__file__).resolve().parents[1] / 'target/debug/hgs')).resolve()


class SessionGraph(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='zerus-graph-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.state = self.root / 'state'; self.state.mkdir()
        self.home = self.root / 'home'; self.home.mkdir()
        self.env = {k: v for k, v in os.environ.items() if not k.startswith('HGS_')}
        self.env.update(HOME=str(self.home), HGS_CONFIG_DIR=str(self.root/'config'), HGS_STATE_DIR=str(self.state))
        self.marker = self.state/'read-marks.json'
        self.marker.write_bytes(b'{"fixture":"untouched"}\n')

    def record(self, name='codex/example/one', conversation='native-one', **extra):
        value = dict(version=1, name=name, agent='codex', run_id='run-'+conversation,
                     conversation_id=conversation, account_id='fixture', subagents={})
        value.update(extra)
        path = self.state / (hashlib.sha256(name.encode()).hexdigest()+'.json')
        path.write_text(json.dumps(value))
        return path, value

    def graph(self, *args, success=True, timeout=5):
        result = subprocess.run([str(HGS), 'graph', *args, '--json'], env=self.env,
                                text=True, capture_output=True, timeout=timeout)
        self.assertEqual(self.marker.read_bytes(), b'{"fixture":"untouched"}\n')
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(result.stdout)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, '')

    def full(self):
        # A bounded native slice may advance only one source under slow I/O.
        # Keep the original 12 x 5 s budget, rather than assuming 12 slices.
        deadline = time.monotonic() + 60
        value = None
        while (remaining := deadline - time.monotonic()) > 0:
            value = self.graph('snapshot', timeout=min(5, remaining))
            if value['coverage']['bindings_complete']:
                break
        self.assertIsNotNone(value)
        self.assertTrue(value['coverage']['bindings_complete'])
        rows = value['items'][:]
        while value['has_more']:
            value = self.graph('snapshot', '--cursor', value['cursor'])
            self.assertFalse(value['reset_required'])
            rows += value['items']
        return rows, value['cursor']

    def test_real_cli_projects_only_whitelisted_metadata_and_preserves_source(self):
        path, record = self.record(subagents={'child': dict(name='Reviewer', started=1.0)},
                                   detail='SECRET_BODY', prompt='SECRET_PROMPT')
        before = path.read_bytes()
        rows, _ = self.full()
        self.assertEqual(path.read_bytes(), before)
        kinds = [r['item']['kind'] for r in rows]
        self.assertEqual(kinds.count('node'), 1)
        self.assertEqual(kinds.count('child'), 1)
        self.assertEqual(kinds.count('relation'), 1)
        self.assertNotIn('SECRET_', json.dumps(rows))
        self.assertFalse((self.state/'events.sqlite3').exists())
        self.assertFalse((self.state/'input_receipts').exists())

    def test_current_bindings_and_cached_stamps_do_not_wait_for_archive_backfill(self):
        import uuid
        path, current = self.record()
        bindings = self.state/'bindings'; bindings.mkdir()
        (bindings/path.name).write_text(json.dumps(current)); path.unlink()
        archives = self.state/'archives'; archives.mkdir()
        for i in range(120):
            archive = str(uuid.uuid4())
            saved = dict(current, name=f'codex/example/archived-{i}', run_id=f'archive-run-{i}',
                         conversation_id=f'archived-native-{i}', state='archived', archive_id=archive)
            (archives/(archive+'.json')).write_text(json.dumps(saved))
        first = self.graph('snapshot')
        self.assertTrue(any(r['item']['kind']=='binding' and
                            r['item']['value']['name']==current['name'] for r in first['items']))
        self.full()
        # A warm unchanged inventory checks metadata rather than spending the
        # 32 changed-record budget on old archives again: rereading all 121
        # sources takes at least four slices, checking their stamps one. A slow
        # runner may still end a slice at its 50 ms budget, so up to three pass.
        for _ in range(3):
            if self.graph('snapshot')['coverage']['bindings_complete']:
                break
        else:
            self.fail('the warm rescan reread old archives')

    def test_partial_scan_retains_inventory_and_changes_include_exact_deletion(self):
        paths = [self.record(name=f'codex/example/s{i}', conversation=f'native-{i}')[0] for i in range(75)]
        first = self.graph('snapshot')
        self.assertFalse(first['coverage']['bindings_complete'])
        rows, cursor = self.full()
        self.assertEqual(sum(r['item']['kind']=='node' for r in rows), 75)
        paths[0].unlink()
        self.full()
        changes = self.graph('changes', '--after', cursor)
        deleted = [r for r in changes['items'] if r['deleted']]
        self.assertEqual(len(deleted), 3)  # exact node, binding, provisional alias

    def test_old_scan_order_upgrade_keeps_live_sources_and_exact_removal(self):
        import sqlite3
        from contextlib import closing
        path, _ = self.record()
        rows, _ = self.full()
        directory = self.state/'bindings'; directory.mkdir()
        new = directory/path.name
        new.write_bytes(path.read_bytes()); path.unlink()
        self.full()
        with closing(sqlite3.connect(self.state/'graph/index.sqlite3')) as db:
            with db:
                db.execute("DELETE FROM meta WHERE key='scan_order'")
                db.execute("UPDATE meta SET value='archives/old.json' WHERE key='scan_after'")
        repaired, _ = self.full()
        self.assertEqual(repaired, rows)
        new.unlink()
        removed, _ = self.full()
        self.assertEqual(removed, [])

    def test_corrupt_source_retains_last_good_and_exposes_coverage_gap(self):
        path, _ = self.record()
        before, _ = self.full()
        path.write_text('{invalid private data')
        value = self.graph('snapshot')
        self.assertEqual(value['coverage']['source_errors'], 1)
        self.assertEqual(value['items'], before)
        self.assertNotIn('private data', json.dumps(value))

    def test_rename_shadow_and_canonical_binding_do_not_duplicate_conversation(self):
        old, record = self.record()
        self.full()
        new_name = 'codex/example/renamed'
        route = self.state/'name_routes'; route.mkdir()
        bindings = self.state/'bindings'; bindings.mkdir()
        record['name'] = new_name
        digest = hashlib.sha256(new_name.encode()).hexdigest()+'.json'
        (route/old.name).write_text('{}')
        (route/digest).write_text('{}')
        (bindings/digest).write_text(json.dumps(record))
        rows, _ = self.full()
        self.assertEqual(sum(r['item']['kind']=='node' for r in rows), 1)
        names = [r['item']['value']['name'] for r in rows if r['item']['kind']=='binding']
        self.assertEqual(names, [new_name])

    def test_pagination_reset_foreign_cursor_and_invalid_limits(self):
        path, record = self.record()
        first = self.graph('snapshot', '--limit', '1')
        self.assertTrue(first['has_more'])
        record['name'] = 'codex/example/another'
        self.record(**{k:v for k,v in record.items() if k!='conversation_id'}, conversation='new-native')
        self.graph('snapshot')
        self.assertTrue(self.graph('snapshot', '--cursor', first['cursor'])['reset_required'])
        self.graph('changes', '--after', 'malformed', success=False)
        self.graph('snapshot', '--limit', '1000000', success=False)
        self.graph('snapshot', '--unknown', success=False)

    def test_archived_identity_survives_live_name_reuse_without_transcripts(self):
        import uuid
        path,old=self.record(subagents={'child':dict(name='Reviewer',started=1.0)})
        first,_=self.full()
        original=next(r['id'] for r in first if r['item']['kind']=='node')
        archive=str(uuid.uuid4());old.update(archive_id=archive,state='archived')
        directory=self.state/'archives';directory.mkdir()
        (directory/(archive+'.json')).write_text(json.dumps(old))
        path.unlink()
        self.record(conversation='replacement')
        rows,_=self.full()
        nodes=[r['id'] for r in rows if r['item']['kind']=='node']
        self.assertEqual(len(nodes),2);self.assertIn(original,nodes)
        archived=[r['item']['value'] for r in rows if r['item']['kind']=='binding'
                  and r['item']['value']['archived']]
        self.assertEqual(len(archived),1)
        self.assertEqual(archived[0]['archive_id'],archive)
        self.assertEqual(sum(r['item']['kind']=='child' for r in rows),1)
        self.assertFalse((self.state/'events.sqlite3').exists())

    @unittest.skipIf(os.name=='nt', 'Unix symlink fixture')
    def test_symlink_binding_never_reads_external_record(self):
        path, value = self.record()
        outside = self.root/'private.json'; outside.write_text(json.dumps(value))
        path.unlink(); path.symlink_to(outside)
        reply = self.graph('snapshot')
        self.assertEqual(reply['items'], [])
        self.assertEqual(reply['coverage']['source_errors'], 1)


if __name__ == '__main__':
    unittest.main()
