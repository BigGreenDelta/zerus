"""Hook clipping reconciliation on isolated public history, including old caches."""
import hashlib
import json
import sqlite3
import unittest
import test_public_history_contract as fixtures


class StopClipping(unittest.TestCase):
    def fixture(self):
        f=fixtures.PublicHistoryContract(methodName='runTest');f.setUp();self.addCleanup(f.doCleanups);return f

    def test_long_ascii_and_unicode_both_arrival_orders(self):
        for prefix in ('A'*1200, '🧭Ж🙂α'*300):
            for provider_first in (False,True):
                with self.subTest(unicode=prefix[0]!='A',provider_first=provider_first):
                    f=self.fixture();full=prefix+' public final continuation';hook=prefix+'…'
                    if provider_first:
                        f.write_source([f.message(0,at=100,text=full)]);before=f.complete()
                    else:
                        f.journal([{'type':'Stop','at':100,'detail':hook}]);before=f.complete()
                    if provider_first:f.journal([{'type':'Stop','at':101,'detail':hook}])
                    else:f.append_source([f.message(0,at=101,text=full)])
                    after=f.complete();self.assertEqual(len(after['events']),1);row=after['events'][0]
                    self.assertEqual(row['detail'],full);self.assertEqual(after['head']['total_incoming'],1)
                    self.assertEqual(row['history_id'],before['events'][0]['history_id']);self.assertEqual(after['history_epoch'],before['history_epoch'])

    def test_legacy_numbered_duplicate_cache_repaired_without_source_rebuild(self):
        f=self.fixture();prefix='🧭Ж🙂α'*300;full=prefix+' final public reply'
        f.write_source([f.message(0,at=100,text=full)]);before=f.complete();provider=before['events'][0]
        f.journal([{'type':'Stop','at':101,'detail':prefix+'…'}])
        index=next((f.state/'public_history').glob('*.sqlite3'))
        origin='journal:journal:AgentMessage:native:1'
        hook_id=hashlib.sha256(json.dumps([index.stem,origin],separators=(',',':')).encode()).hexdigest()
        hook=json.dumps({'type':'Stop','at':101,'detail':prefix+'…','seq':1},ensure_ascii=False,separators=(',',':'))
        with sqlite3.connect(index) as db:
            # Reproduce the already-finalized old literal matcher cache. It
            # retained both source rows and had consumed journal/transcript.
            db.execute('INSERT INTO messages(id,at,role,stream,kind,ordinal,body,incoming) VALUES(?,?,?,?,?,?,?,?)',
                       (hook_id,101,'incoming','journal','Stop','0:'+str(1).zfill(20),hook,2))
            db.execute('INSERT INTO aliases(original,id) VALUES(?,?)',(origin,hook_id))
            db.execute("INSERT OR REPLACE INTO meta(key,value) VALUES('journal_seq','1')")
            db.execute("DELETE FROM meta WHERE key IN ('reconcile_version','reconcile_upgrade_after')")
            db.execute('DELETE FROM reconcile_work')
            db.execute("INSERT OR REPLACE INTO meta(key,value) VALUES('reconciled','1')")
            prior_offsets=db.execute("SELECT key,value FROM meta WHERE key LIKE 'offset:%'").fetchall()
        after=f.complete();self.assertEqual(len(after['events']),1);row=after['events'][0]
        self.assertEqual(row['detail'],full);self.assertEqual(after['head']['total_incoming'],1)
        self.assertNotEqual(after['history_epoch'],before['history_epoch'])
        self.assertIn('history:'+provider['history_id'],row['original_ids']);self.assertIn(origin,row['original_ids'])
        f.history(f.request(around=provider['history_cursor']),reject=True)
        f.history(f.request(around_incoming_seq=1,history_epoch=before['history_epoch']),reject=True)
        with sqlite3.connect(index) as db:
            self.assertEqual(db.execute("SELECT key,value FROM meta WHERE key LIKE 'offset:%'").fetchall(),prior_offsets)
            self.assertEqual(db.execute("SELECT value FROM meta WHERE key='reconcile_version'").fetchone()[0],'3')
        repeat=f.complete();self.assertEqual(repeat['history_epoch'],after['history_epoch']);self.assertEqual(repeat['events'],after['events'])

    def test_clean_existing_cache_upgrade_retains_epoch_and_cursors(self):
        f=self.fixture();f.write_source([f.message(0,at=100,text='Clean public reply')]);before=f.complete()
        index=next((f.state/'public_history').glob('*.sqlite3'))
        with sqlite3.connect(index) as db:db.execute("DELETE FROM meta WHERE key IN ('reconcile_version','reconcile_upgrade_after')")
        after=f.complete();self.assertEqual(after['history_epoch'],before['history_epoch']);self.assertEqual(after['events'],before['events'])
        around=f.history(around=before['events'][0]['history_cursor']);self.assertEqual(around['events'],after['events'])

    def test_literal_short_ellipsis_mismatch_time_and_ambiguity_preserved(self):
        prefix='Ж'*1200
        for case in ('short_literal','mismatch','late','two_hooks','two_providers'):
            with self.subTest(case=case):
                f=self.fixture();hook='Short literal…' if case=='short_literal' else prefix+'…'
                full='Short literal continues' if case=='short_literal' else 'Unrelated' if case=='mismatch' else prefix+' final'
                f.journal([{'type':'Stop','at':100,'detail':hook}])
                if case=='two_hooks':f.journal([{'type':'Stop','at':101,'detail':hook}])
                messages=[f.message(0,at=104 if case=='late' else 101,text=full)]
                if case=='two_providers':messages.append(f.message(1,at=102,text=full))
                f.write_source(messages);after=f.complete();self.assertEqual(len(after['events']),3 if case.startswith('two_') else 2)
                self.assertTrue(all(len(row['original_ids'])==1 for row in after['events']))

if __name__=='__main__':unittest.main()
