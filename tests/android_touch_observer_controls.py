"""Android-only SDK-free source-bound memory stand-ins; no Kotlin, Window or MotionEvent execution."""
import importlib.util
from pathlib import Path
import re
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / "mobile/android/app/src/androidTest/java/app/zerus/mobile/OwnedWindowTouchTrace.kt"
FIXTURE = ROOT / "mobile/android/app/src/androidTest/java/app/zerus/mobile/ApprovalChoicesSmokeInstrumentation.kt"

FORWARD = '''    private fun <T> forward(record: () -> EventRecord?, dispatch: () -> T): T {
        val before = try { record() } catch (_: Throwable) { uncertain("before-record-failed"); null }
        return try {
            val result = dispatch()
            finish(before, result, null)
            result
        } catch (error: Throwable) {
            finish(before, null, error)
            throw error
        }
    }
'''
FINISH = '''    private fun finish(record: EventRecord?, result: Any?, error: Throwable?) {
        try {
            if (record == null) return
            record.outcome = if (error != null) "throw" else when (result) { is Boolean -> result.toString(); Unit -> "unit"; else -> "UNKNOWN" }
            record.after = state().take(160)
        } catch (_: Throwable) { uncertain("after-record-failed") }
    }
'''
CLOSE = '''    fun close() {
        check(onMain())
        if (!installed) return
        if (window.callback !== wrapper) { uncertain("callback-changed"); error("Owned touch callback changed; restoration UNKNOWN") }
        window.callback = original
        if (window.callback !== original) { uncertain("restore-unverified"); error("Owned touch callback restoration UNKNOWN") }
        installed = false
        restored = true
    }
'''


def bound_source(source):
    """Only these exact Kotlin bodies admit the finite translation below; no general transpiler claim."""
    for exact in (FORWARD, FINISH, CLOSE):
        assert source.count(exact) == 1
    assert 'object : Window.Callback by original {' in source
    assert 'override fun dispatchTouchEvent(event: MotionEvent): Boolean = forward({ touch(event) }) { original.dispatchTouchEvent(event) }' in source
    assert 'forward({ focus(hasFocus) }) { original.onWindowFocusChanged(hasFocus) }' in source
    tap_limit = int(re.search(r'if \(taps.size >= (\d+)\)', source)[1])
    event_limit = int(re.search(r'if \(tap.records.size >= (\d+)\)', source)[1])
    assert (tap_limit, event_limit) == (8, 4)
    namespace = {}
    # Straight-line translation of FORWARD, including the original exception object and totalized FINISH.
    exec(compile('''def forward(record, dispatch, finish, uncertain):
    try:
        before = record()
    except BaseException:
        uncertain("before-record-failed")
        before = None
    try:
        result = dispatch()
        finish(before, result, None)
        return result
    except BaseException as error:
        finish(before, None, error)
        raise error
''', '<owned-window-forward-exact-source-standin>', 'exec'), namespace)
    return namespace['forward'], tap_limit, event_limit


class MemoryTrace:
    """Memory-only Window/record stand-in with exact-source delegation body above; never Android OS evidence."""
    def __init__(self, source=None, state=lambda: 'fixed-state'):
        self.forward, self.tap_limit, self.event_limit = bound_source(HELPER.read_text() if source is None else source)
        self.original = SimpleNamespace(touch=self.original_touch, focus=self.original_focus)
        self.wrapper = object()
        self.window = SimpleNamespace(callback=self.wrapper, identity=9)
        self.calls = []
        self.result = True
        self.error = None
        self.unknown = None
        self.taps = []
        self.pending = None
        self.installed = True
        self.restored = False
        self.token_overflow = False
        self.state = state

    def uncertain(self, reason):
        if self.unknown is None:
            self.unknown = reason

    def begin(self, label='Send answer', expected_window=9):
        if label != 'Send answer':
            return None
        if self.pending is not None:
            self.uncertain('nested-tap'); return None
        if len(self.taps) >= self.tap_limit:
            self.token_overflow = True
            self.uncertain('tap-overflow'); return None
        if not self.installed or self.window.callback is not self.wrapper:
            self.uncertain('callback-changed'); return None
        tap = SimpleNamespace(id=len(self.taps) + 1, expected_window=expected_window, owned_window=self.window.identity,
                              down=100, x=898.0, y=1762.0, records=[], ended=False, overflow=False)
        if tap.expected_window != tap.owned_window:
            self.uncertain('window-mismatch')
        self.taps.append(tap)
        self.pending = tap
        return tap.id

    def end(self, token):
        if self.pending is None or self.pending.id != token:
            self.uncertain('token-mismatch'); return
        self.pending.ended = True
        self.pending = None
        if self.window.callback is not self.wrapper:
            self.uncertain('callback-changed')

    def append(self, kind, metadata):
        tap = self.pending
        if len(tap.records) >= self.event_limit:
            tap.overflow = True
            self.uncertain('record-overflow'); return None
        row = SimpleNamespace(kind=kind, metadata=metadata, before=self.state()[:160], after='UNKNOWN', outcome='UNKNOWN')
        tap.records.append(row)
        return row

    def touch_record(self, event):
        tap = self.pending
        if tap is None or event.action not in (0, 1, 3):
            return None
        matches = tap.expected_window == tap.owned_window and event.down == tap.down and event.time >= tap.down and \
            event.pointers == 1 and event.source == 4098 and event.raw_x == tap.x and event.raw_y == tap.y
        if not matches:
            self.uncertain('touch-metadata-mismatch')
        return self.append('touch', (id(event), event.action, matches))

    def finish(self, row, result, error):
        try:
            if row is None:
                return
            row.outcome = 'throw' if error is not None else str(result).lower() if isinstance(result, bool) else 'unit' if result is None else 'UNKNOWN'
            row.after = self.state()[:160]
        except BaseException:
            self.uncertain('after-record-failed')

    def original_touch(self, event):
        self.calls.append(('touch', event))
        if self.error is not None:
            raise self.error
        return self.result

    def original_focus(self, value):
        self.calls.append(('focus', value))
        if self.error is not None:
            raise self.error

    def touch(self, event):
        return self.forward(lambda: self.touch_record(event), lambda: self.original.touch(event), self.finish, self.uncertain)

    def focus(self, value):
        return self.forward(lambda: self.append('focus', value) if self.pending is not None else None,
                            lambda: self.original.focus(value), self.finish, self.uncertain)

    def close(self):
        if not self.installed:
            return
        if self.window.callback is not self.wrapper:
            self.uncertain('callback-changed')
            raise RuntimeError('Owned touch callback changed; restoration UNKNOWN')
        self.window.callback = self.original
        self.installed = False
        self.restored = True

    def classification(self):
        return 'UNKNOWN' if self.unknown is not None else 'window-consumption-only'


def event(**changes):
    values = dict(action=0, down=100, time=101, pointers=1, source=4098, raw_x=898.0, raw_y=1762.0, x=898.0, y=1762.0)
    values.update(changes)
    return SimpleNamespace(**values)


class OwnedWindowTouchObserverTests(unittest.TestCase):
    def test_same_event_once_and_exact_true_false_return(self):
        for value in (True, False):
            with self.subTest(value=value):
                trace = MemoryTrace(); trace.result = value; trace.begin(); received = event()
                self.assertIs(trace.touch(received), value)
                self.assertEqual(len(trace.calls), 1)
                self.assertIs(trace.calls[0][1], received)
                self.assertEqual(trace.taps[0].records[0].metadata[0], id(received))

    def test_original_throwable_identity_survives_both_records(self):
        trace = MemoryTrace(); trace.begin(); error = RuntimeError('original'); trace.error = error
        with self.assertRaises(RuntimeError) as caught:
            trace.touch(event())
        self.assertIs(caught.exception, error)
        self.assertEqual(len(trace.calls), 1)
        self.assertEqual(trace.taps[0].records[0].outcome, 'throw')

    def test_before_record_fault_cannot_prevent_original_dispatch(self):
        def failed(): raise LookupError('record')
        trace = MemoryTrace(state=failed); trace.begin(); received = event()
        self.assertTrue(trace.touch(received))
        self.assertIs(trace.calls[0][1], received)
        self.assertEqual(trace.unknown, 'before-record-failed')
        self.assertEqual(trace.taps[0].records, [])

    def test_after_record_fault_preserves_result_and_partial_record(self):
        values = iter(['before', LookupError('record')])
        def state():
            value = next(values)
            if isinstance(value, BaseException): raise value
            return value
        trace = MemoryTrace(state=state); trace.begin(); trace.result = False
        self.assertIs(trace.touch(event()), False)
        self.assertEqual(trace.unknown, 'after-record-failed')
        self.assertEqual(trace.taps[0].records[0].outcome, 'false')
        self.assertEqual(trace.taps[0].records[0].after, 'UNKNOWN')

    def test_record_faults_do_not_replace_original_exception(self):
        for failing_stage in ('before', 'after'):
            with self.subTest(stage=failing_stage):
                calls = []
                def state():
                    calls.append('record')
                    if failing_stage == 'before' or len(calls) == 2: raise LookupError('observer')
                    return 'before'
                trace = MemoryTrace(state=state); trace.begin(); trace.error = error = RuntimeError('original')
                with self.assertRaises(RuntimeError) as caught: trace.touch(event())
                self.assertIs(caught.exception, error)
                self.assertEqual(len(trace.calls), 1)

    def test_focus_same_boolean_once_and_exact_exception(self):
        for value in (True, False):
            trace = MemoryTrace(); trace.begin()
            self.assertIsNone(trace.focus(value))
            self.assertEqual(trace.calls, [('focus', value)])
            self.assertIs(trace.calls[0][1], value)
            self.assertEqual(trace.taps[0].records[0].metadata, value)
        trace.error = error = RuntimeError('focus-original')
        with self.assertRaises(RuntimeError) as caught: trace.focus(False)
        self.assertIs(caught.exception, error)

    def test_focus_drop_and_return_during_same_touch_interval_are_retained(self):
        trace = MemoryTrace(); trace.begin()
        trace.touch(event()); trace.focus(False); trace.focus(True); trace.touch(event(action=1, time=180))
        self.assertEqual([row.kind for row in trace.taps[0].records], ['touch', 'focus', 'focus', 'touch'])
        self.assertEqual([row.metadata for row in trace.taps[0].records if row.kind == 'focus'], [False, True])
        self.assertEqual([call[0] for call in trace.calls], ['touch', 'focus', 'focus', 'touch'])
        self.assertIsNone(trace.unknown)

    def test_outside_interval_and_unfiltered_event_forward_without_records(self):
        trace = MemoryTrace(); received = event()
        self.assertTrue(trace.touch(received))
        self.assertIsNone(trace.begin('Confirm'))
        trace.begin(); move = event(action=2)
        self.assertTrue(trace.touch(move))
        self.assertEqual(trace.taps[0].records, [])
        self.assertEqual(trace.calls, [('touch', received), ('touch', move)])

    def test_all_metadata_departures_are_unknown_never_delivery_proof(self):
        for change in (dict(down=99), dict(time=99), dict(pointers=2), dict(source=1), dict(raw_x=899.0), dict(raw_y=1763.0)):
            with self.subTest(change=change):
                trace = MemoryTrace(); trace.begin(); received = event(**change)
                self.assertTrue(trace.touch(received))
                self.assertEqual(trace.classification(), 'UNKNOWN')
                self.assertIs(trace.calls[0][1], received)
        trace = MemoryTrace(); trace.begin(expected_window=10); trace.touch(event())
        self.assertEqual(trace.unknown, 'window-mismatch')

    def test_global_raw_coordinates_match_with_nonzero_window_origin(self):
        trace = MemoryTrace(); trace.begin()
        received = event(x=898.0 - 24, y=1762.0 - 80)
        self.assertTrue(trace.touch(received))
        self.assertIsNone(trace.unknown)
        self.assertNotEqual(received.x, received.raw_x)
        self.assertNotEqual(received.y, received.raw_y)
        source = HELPER.read_text()
        predicate = source.split('        val matches = ', 1)[1].split('\n        if (!matches)', 1)[0]
        self.assertIn('event.rawX == tap.x && event.rawY == tap.y', predicate)
        self.assertNotRegex(predicate, r'event\.[xy]\b')

    def test_record_overflow_stays_bounded_and_still_forwards_every_event(self):
        trace = MemoryTrace(); trace.begin()
        for _ in range(6): trace.touch(event())
        self.assertEqual(len(trace.taps[0].records), 4)
        self.assertEqual(len(trace.calls), 6)
        self.assertEqual(trace.unknown, 'record-overflow')
        self.assertTrue(trace.taps[0].overflow)
        self.assertEqual(trace.classification(), 'UNKNOWN')

    def test_token_overflow_nested_and_wrong_end_cannot_invent_coverage(self):
        trace = MemoryTrace()
        for _ in range(8): trace.end(trace.begin())
        self.assertEqual(len(trace.taps), 8)
        self.assertIsNone(trace.begin())
        self.assertEqual(trace.unknown, 'tap-overflow')
        self.assertTrue(trace.token_overflow)
        nested = MemoryTrace(); nested.begin()
        self.assertIsNone(nested.begin())
        self.assertEqual(nested.unknown, 'nested-tap')
        fresh = MemoryTrace(); token = fresh.begin(); fresh.end(token + 1)
        self.assertFalse(fresh.pending.ended)
        self.assertEqual(fresh.classification(), 'UNKNOWN')

    def test_close_restores_only_exact_wrapper_and_preserves_changed_callback(self):
        trace = MemoryTrace(); trace.close()
        self.assertIs(trace.window.callback, trace.original)
        self.assertTrue(trace.restored)
        trace = MemoryTrace(); replacement = object(); trace.window.callback = replacement
        with self.assertRaisesRegex(RuntimeError, 'UNKNOWN'): trace.close()
        self.assertIs(trace.window.callback, replacement)
        self.assertEqual(trace.classification(), 'UNKNOWN')

    def test_source_binding_refuses_changed_delegate_argument_or_missing_catch(self):
        source = HELPER.read_text()
        for mutated in (source.replace('original.dispatchTouchEvent(event)', 'original.dispatchTouchEvent(other)'),
                        source.replace('throw error', 'throw RuntimeException()'),
                        source.replace('uncertain("after-record-failed")', 'error("after-record-failed")')):
            with self.subTest(mutated=mutated[-80:]), self.assertRaises(AssertionError): bound_source(mutated)

    def test_dispatch_source_has_no_tree_binder_wait_mutation_or_product_hook(self):
        source = HELPER.read_text()
        dispatch = source.split('    private fun touch(', 1)[1].split('    fun install()', 1)[0]
        for denied in ('decorView', 'uiAutomation', 'rootInActiveWindow', 'getBounds', 'performAction', 'model.draft',
                       'injectInputEvent', 'waitForIdle', 'sleep(', 'delay(', 'recycle(', 'setAction(', 'offsetLocation('):
            self.assertNotIn(denied, dispatch)
        fixture = FIXTURE.read_text()
        state = fixture.split('    private fun touchModelState(', 1)[1].split('    private fun smoke(', 1)[0]
        self.assertIn('QuestionPolicies.savedDraft(model.drafts, target, it)', state)
        self.assertIn('check(model.drafts.size <= 8)', state)
        for denied in ('model.draft(', 'model.edit(', 'model.send(', 'uiAutomation', 'getWindowVisibleDisplayFrame'):
            self.assertNotIn(denied, state)
        self.assertIn('scope=window-consumption-only reviewGuard=UNKNOWN', source)
        evidence = source.split('    fun evidence()', 1)[1].split('    fun close()', 1)[0]
        self.assertNotIn('window.', evidence)
        self.assertIn('coverage=recorded-window-events-only', evidence)
        self.assertIn('tokenOverflow=$tokenOverflow', evidence)
        self.assertIn('overflow=${tap.overflow}', evidence)

    def test_fixture_cleanup_attempts_restore_and_back_finish_independently(self):
        source = FIXTURE.read_text()
        cleanup = source.split('            var observerCleanup: Throwable? = null', 1)[1]
        self.assertLess(cleanup.index('touchTrace?.close()'), cleanup.index('model.back(); activity.finish()'))
        self.assertIn('val original = primaryFailure ?: observerCleanup', cleanup)
        self.assertIn('if (primaryFailure == null) observerCleanup?.let { throw it }', cleanup)
        for primary_present in (False, True):
            trace = MemoryTrace(); trace.window.callback = object()
            original = RuntimeError('primary') if primary_present else None
            failures = []; attempts = []
            try:
                attempts.append('restore'); trace.close()
            except BaseException as error: failures.append(error)
            try:
                attempts.append('back-finish'); raise LookupError('cleanup')
            except BaseException as error: failures.append(error)
            retained = original if original is not None else failures[0]
            self.assertIs(retained, original if primary_present else failures[0])
            self.assertEqual(attempts, ['restore', 'back-finish'])
            self.assertEqual(len(failures), 2)

    def test_original_physical_injection_body_and_deadlines_remain_exact(self):
        source = FIXTURE.read_text()
        tap = source.split('    private fun tap(', 1)[1].split('    private fun reveal(', 1)[0]
        self.assertIn('for (type in listOf(MotionEvent.ACTION_DOWN, MotionEvent.ACTION_UP)) {', tap)
        self.assertEqual(tap.count('uiAutomation.injectInputEvent(event, true)'), 1)
        self.assertEqual(tap.count('event.recycle()'), 1)
        self.assertIn('MotionEvent.obtain(down, SystemClock.uptimeMillis(), type, bounds.exactCenterX(), bounds.exactCenterY(), 0)', tap)
        self.assertIn('SystemClock.elapsedRealtime() + 10_000', source)
        self.assertIn('elapsed < 3_000', source)
        self.assertNotIn('waitForIdle', source)

    def test_success_and_failure_retain_trace_without_changing_seven_case_parser(self):
        source = FIXTURE.read_text()
        outer = source.split('    override fun onStart()', 1)[1].split('    private fun <T> main(', 1)[0]
        self.assertIn('synthetic receipts only. ${touchResultEvidence().replace("\\n", " | ")}\\n', outer)
        self.assertIn('$failureEvidence\\n${touchResultEvidence()}\\n', outer)
        self.assertIn('catch (_: Throwable) { "owned-touch UNKNOWN result-capture-failed" }', outer)
        spec = importlib.util.spec_from_file_location('touch_trace_result_parser', ROOT / 'scripts/ci/android-project-ui.py')
        parser = importlib.util.module_from_spec(spec); spec.loader.exec_module(parser)
        parser.select_suite(['--approval-choices'])
        trace = 'owned-touch status=observed restored=true scope=window-consumption-only\nowned-touch token=1 kind=touch outcome=true'
        prefix = f'INSTRUMENTATION_RESULT: approval_cases=7\nINSTRUMENTATION_RESULT: approval_case_ids={parser.CASES}\n'
        stream = f'Approval choices smoke passed: {parser.CASES}; synthetic receipts only. ' + trace.replace('\n', ' | ')
        result = prefix + 'INSTRUMENTATION_RESULT: stream=' + stream + '\n\nINSTRUMENTATION_CODE: -1\n'
        self.assertEqual(parser.instrumentation_result(result)['stream'], stream)
        for invalid in (result.replace('approval_cases=7', 'approval_cases=6'), result.replace(' | ', '\n'),
                        result.replace('INSTRUMENTATION_CODE: -1', 'INSTRUMENTATION_CODE: 0')):
            with self.subTest(invalid=invalid[-80:]), self.assertRaises(AssertionError): parser.instrumentation_result(invalid)


if __name__ == '__main__':
    unittest.main(verbosity=2)
