"""Finite source-bound stand-ins, not Kotlin, Compose, Android or device qualification."""
import ast
import hashlib
import importlib.util
from pathlib import Path
import re
from types import SimpleNamespace as NS
import unittest

ROOT = Path(__file__).resolve().parents[1]
_focus_spec = importlib.util.spec_from_file_location("review_focus_projection", ROOT / "tests/review_focus_projection.py")
_focus_projection = importlib.util.module_from_spec(_focus_spec)
_focus_spec.loader.exec_module(_focus_projection)
FIXTURE = _focus_projection.FIXTURE
PROBE = _focus_projection.PROBE
BASELINES = _focus_projection.BASELINES
reverse_focus_wait = _focus_projection.reverse_focus_wait
reverse_disclosure_reveal = _focus_projection.reverse_disclosure_reveal
reverse_confirmation_order = _focus_projection.reverse_confirmation_order


def disclosure_preparation_memory(source, visibility, *, header=True, ready=True, capture=True):
    """Finite source-bound accessibility stand-in; no Kotlin or device qualification."""
    body = source.split('    private fun reveal(', 1)[1].split('    private data class DisclosureScroll', 1)[0]
    ordinary = re.search(r'if \((visible\(label\) != null(?: && !captureDisclosure)?)\) true else', body)[1]
    capturing = 'captureDisclosure: Boolean = false' in body
    assert ordinary == ('visible(label) != null && !captureDisclosure' if capturing else 'visible(label) != null')
    for exact in ('await("option $label")',
                  'disclosureScroll ?: bindDisclosureScroll(disclosureLabel).also { disclosureScroll = it }',
                  'check(disclosureScrollReady(disclosure))',
                  'disclosure.node.performAction(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD)'):
        assert exact in body
    assert ('if (captureDisclosure && visible(label) != null) true else' in body) == capturing
    assert ('return disclosureScroll' in body) == capturing
    awaited = source.split('    private fun await(', 1)[1].split('    private fun find(', 1)[0]
    assert 'SystemClock.elapsedRealtime() + 10_000' in awaited and 'SystemClock.sleep(30)' in awaited
    lifecycle = source.split('            boundary("begin", "lifecycle_no_submit")', 1)[1].split(
        '            tap("Approve for this session"); reviewed()', 1)[0]
    if capturing:
        assert 'val lifecycleDisclosure = checkNotNull(reveal("Approve for this session", "Review synthetic background", captureDisclosure = true))' in lifecycle
        assert 'bindDisclosureScroll' not in lifecycle
    else:
        assert 'val lifecycleDisclosure = bindDisclosureScroll("Approve for this session")' in lifecycle
    observations = iter(visibility)
    state = {'last': False, 'binds': [], 'scrolls': 0, 'now': 0}
    owned = object()
    def visible():
        state['last'] = next(observations, state['last'])
        return state['last']
    def bind(label):
        state['binds'].append(label)
        assert header if label == 'header' else visible(), 'Current disclosure anchor is not visible'
        assert ready, 'Bound disclosure is not in the active window'
        return owned
    disclosure = None
    while True:
        # Evaluate the actual short-circuit and the sealed added capture branch.
        if visible() and (not capture if capturing else True):
            break
        disclosure = disclosure or bind('header')
        assert ready, 'Bound disclosure is not in the active window'
        if capturing and capture and visible():
            break
        state['scrolls'] += 1
        assert state['now'] < 10_000, 'original preparation deadline'
        state['now'] += 30
    if capture and not capturing:
        disclosure = bind('option')  # Actual old lifecycle's second, unguarded lookup.
    return disclosure, owned, state


def confirmation_read_memory(source, *, absent=True, absence_cost=2307, trace=None):
    """Evaluate the actual observation order over recorded-cost fakes, never Android evidence."""
    trace = [] if trace is None else trace
    body = source.split('                lifecycleCheckpoint(LifecycleCheckpoint.BODY_RESTORE_PASSED)', 1)[1].split(
        '            } finally {', 1)[0]
    statements = {
        'anchor': 'prepareEarlySendAnchor(lifecycleSend)',
        'disabled': 'check(count() == 4 && !button("Send answer").isEnabled)',
        'elapsed': 'check(elapsed >= 0 && elapsed < 3_000)',
        'absence': 'await("background retires confirmation") { visible("Confirm this option?") == null }',
    }
    assert all(body.count(statement) == 1 for statement in statements.values())
    ordered = sorted(statements, key=lambda step: body.index(statements[step]))
    assert 'val end = Math.addExact(start, 3_000)' in source
    assert 'SystemClock.elapsedRealtime() + 10_000' in source
    assert not any(term in body for term in ('.begin(', '.bindReview(', 'resumeEnd =', 'reviewStart ='))
    now = 415  # Recorded BODY_RESTORE_PASSED133085 minus original reviewStartedAt132670.
    for step in ordered:
        trace.append(step)
        if step == 'anchor':
            now += 269
            assert now < 3000, 'original early-review deadline'
        elif step == 'disabled':
            now += 9
            assert now < 3000, 'original early-review deadline'
        elif step == 'elapsed':
            assert 0 <= now < 3000, 'original early-review deadline'
        else:
            assert 0 <= absence_cost <= 10_000, 'original absence10s bound'
            now += absence_cost
            assert absent, 'mandatory native confirmation absence refused'
    assert set(trace) == set(statements)
    return trace, now


def evaluate(source, values):
    source = source.replace('owned.firstReviewSnapshot(bound.generation - 1)', 'recorded')
    for old, new in [('Long.MAX_VALUE', str(2**63 - 1)), ('Stage.CONTENT_COMMIT', "'CONTENT_COMMIT'"),
                     ('null', 'None'), ('&&', ' and '), ('||', ' or ')]:
        source = source.replace(old, new)
    source = re.sub(r'!(?!=)', 'not ', source)
    tree = ast.parse(' '.join(source.split()).strip(), mode='eval')
    allowed = (ast.Expression, ast.BoolOp, ast.And, ast.Or, ast.UnaryOp, ast.Not, ast.Compare,
               ast.Eq, ast.NotEq, ast.Lt, ast.LtE, ast.Gt, ast.GtE, ast.Name, ast.Load, ast.Constant,
               ast.Attribute, ast.BinOp, ast.Add, ast.Sub)
    assert all(isinstance(node, allowed) for node in ast.walk(tree))
    assert {node.id for node in ast.walk(tree) if isinstance(node, ast.Name)} <= values.keys()
    return eval(compile(tree, '<focus-review-scalar-source>', 'eval'), {'__builtins__': {}}, values)


def classification(source):
    body = source.split('@Synchronized fun classifyEarlyReview(', 1)[1].split(
        '    // END EARLY_REVIEW_FOCUS_WAIT classification', 1)[0]
    invalid = re.findall(r'if \(([\s\S]*?)\) return EarlyReviewState.INVALID', body)
    assert len(invalid) == 2
    for exact in ('val latest = checkNotNull(contentSnapshot(owned, question))',
                  'val first = bound.record.detail as? Committed ?: return EarlyReviewState.INVALID',
                  'val current = latest.record.detail as? Committed ?: return EarlyReviewState.INVALID',
                  'val start = first.reviewStartedAt ?: return EarlyReviewState.INVALID',
                  'val currentStart = current.reviewStartedAt',
                  'if (earlyReviewCurrent(owned, question, bound, elapsedNow, originalEnd)) return EarlyReviewState.CURRENT'):
        assert exact in body
    waiting = re.search(r'return if \(([^\n]+)\) EarlyReviewState.WAIT else EarlyReviewState.INVALID', body)[1]
    strict = source.split('@Synchronized fun earlyReviewCurrent(', 1)[1].split('    private fun fingerprint(', 1)[0]
    strict_return = re.search(r'return (bound.record.stage[\s\S]*?)\n    }', strict)[1]

    def classify(bound, latest, now, end, recorded=None):
        values = dict(bound=bound, latest=latest, recorded=bound if recorded is None else recorded,
                      elapsedNow=now, originalEnd=end)
        if evaluate(invalid[0], values): return 'INVALID'
        first, current = bound.record.detail, latest.record.detail
        if first is None or current is None or first.reviewStartedAt is None: return 'INVALID'
        values.update(first=first, current=current, start=first.reviewStartedAt, currentStart=current.reviewStartedAt)
        if evaluate(invalid[1], values): return 'INVALID'
        # This stand-in evaluates the actual unchanged Boolean return, not a newly invented action predicate.
        if current.reviewStartedAt is not None and evaluate(strict_return, values): return 'CURRENT'
        return 'WAIT' if evaluate(waiting, values) else 'INVALID'
    return classify


def wait_memory(source, states, *, now=1000, end=4000, card=True, generation=True, callback_cost=0):
    body = source.split('        private fun awaitCurrentReview(', 1)[1].split(
        '        // END EARLY_REVIEW_FOCUS_WAIT observation', 1)[0]
    for exact in ('val end = checkNotNull(resumeEnd)', 'while (true)',
                  'if (!currentCard() || bound.generation != checkNotNull(boundGeneration)) return false',
                  'val now = SystemClock.elapsedRealtime()', 'check(now < end)',
                  'when (reviewState(bound, now, end))', 'EarlyReviewState.INVALID -> return false',
                  'EarlyReviewState.CURRENT ->', 'check(SystemClock.elapsedRealtime() < end)',
                  'EarlyReviewState.WAIT ->', 'val remaining = end - SystemClock.elapsedRealtime()',
                  'check(remaining > 0)', 'SystemClock.sleep(minOf(30L, remaining))'):
        assert exact in body
    assert not re.search(r'\b(?:resumeEnd|reviewStart|boundGeneration|boundSnapshot)\s*=(?!=)', body)
    assert not any(term in body for term in ('node.', 'performAction', 'injectInputEvent', 'startActivity', '.begin(', '.bindReview('))
    values, calls, sleeps = iter(states), [], []
    while True:
        if not card or not generation: return False, now, calls, sleeps
        assert now < end, 'original deadline'
        calls.append((now, end)); state = next(values); now += callback_cost
        if state == 'INVALID': return False, now, calls, sleeps
        if state == 'CURRENT':
            assert now < end, 'late observation'
            return True, now, calls, sleeps
        assert state == 'WAIT'
        remaining = end - now
        assert remaining > 0, 'late ineligible observation'
        delay = min(30, remaining); sleeps.append(delay); now += delay


def snapshot(start=1000, *, generation=1, **changes):
    detail = NS(eligible=True, reviewStartedAt=start)
    record = NS(stage='CONTENT_COMMIT', card='card', questionObject=7, selection='selection', detail=detail)
    value = NS(generation=generation, record=record)
    for key, item in changes.items():
        target = detail if key in vars(detail) else record if key in vars(record) else value
        setattr(target, key, item)
    return value


class EarlyReviewFocusWaitSource(unittest.TestCase):
    def setUp(self):
        self.probe = (ROOT / PROBE).read_text()
        self.fixture = (ROOT / FIXTURE).read_text()
        self.classify = classification(self.probe)

    def test_exact_reverse_keeps_original_physical_probe_and_boolean_definitions(self):
        for path in (FIXTURE, PROBE):
            original = reverse_focus_wait(path, (ROOT / path).read_text())
            self.assertEqual(hashlib.sha256(original.encode()).hexdigest(), BASELINES[path])
        helper = lambda source: source.split('    @Synchronized fun earlyReviewCurrent(', 1)[1].split('    private fun fingerprint(', 1)[0]
        self.assertEqual(helper(self.probe), helper(reverse_focus_wait(PROBE, self.probe)))
        old = reverse_focus_wait(FIXTURE, self.fixture)
        for begin, end in [('    private fun prepareEarlySendAnchor(', '    private fun tap('),
                           ('        fun earlyReview(', '        var primaryFailure:'),
                           ('            boundary("begin", "feedback_keyboard")', '            boundary("begin", "lifecycle_no_submit")')]:
            self.assertEqual(self.fixture.split(begin, 1)[1].split(end, 1)[0], old.split(begin, 1)[1].split(end, 1)[0])

    def test_projector_refuses_changed_added_code_and_still_protects_original_guards(self):
        for path, old, new in [(FIXTURE, 'minOf(30L, remaining)', '30L'),
                               (PROBE, 'elapsedNow >= originalEnd', 'false')]:
            text = (ROOT / path).read_text(); self.assertIn(old, text)
            with self.assertRaises(AssertionError): reverse_focus_wait(path, text.replace(old, new, 1))
        changed = self.fixture.replace('check(fresh.refresh())', 'check(true)', 1)
        with self.assertRaisesRegex(AssertionError, 'original focus-wait baseline changed'):
            reverse_focus_wait(FIXTURE, changed)

    def test_projector_refuses_missing_or_duplicated_sealed_additions(self):
        for path, source, indent, label in [
                (FIXTURE, self.fixture, '            ', 'current'),
                (FIXTURE, self.fixture, '        ', 'observation'),
                (PROBE, self.probe, '    ', 'classification')]:
            begin = indent + '// BEGIN EARLY_REVIEW_FOCUS_WAIT ' + label + '\n'
            end = indent + '// END EARLY_REVIEW_FOCUS_WAIT ' + label + '\n'
            addition = begin + source.split(begin, 1)[1].split(end, 1)[0] + end
            self.assertEqual(source.count(addition), 1)
            for changed in (source.replace(addition, '', 1), source + addition):
                with self.subTest(path=path, label=label), self.assertRaises(AssertionError):
                    reverse_focus_wait(path, changed)

    def test_same_card_focus_reset_never_becomes_actionable_until_current(self):
        bound = snapshot()
        observations = [snapshot(generation=2, eligible=False), snapshot(None, generation=3, eligible=False),
                        snapshot(None, generation=4), snapshot(2200, generation=5)]
        self.assertEqual([self.classify(bound, value, 2300, 4000) for value in observations], ['WAIT', 'WAIT', 'WAIT', 'CURRENT'])
        result, now, calls, sleeps = wait_memory(self.fixture, ['WAIT', 'WAIT', 'WAIT', 'CURRENT'])
        self.assertTrue(result); self.assertEqual(now, 1090); self.assertEqual(sleeps, [30, 30, 30])
        self.assertEqual({end for _, end in calls}, {4000})

    def test_expiry_refuses_even_a_forward_reset_or_current_late_observation(self):
        bound = snapshot()
        for now, end in [(4000, 4000), (4001, 4000), (2300, 5200), (2300, 3999)]:
            self.assertEqual(self.classify(bound, snapshot(2200, generation=2), now, end), 'INVALID')
            self.assertEqual(self.classify(bound, snapshot(None, generation=2, eligible=False), now, end), 'INVALID')
        for states, kwargs in [(['WAIT'] * 101, {}), (['CURRENT'], dict(now=4000)),
                               (['CURRENT'], dict(callback_cost=3000)), (['WAIT'], dict(callback_cost=3000))]:
            with self.subTest(kwargs=kwargs), self.assertRaises(AssertionError): wait_memory(self.fixture, states, **kwargs)
        self.assertEqual(wait_memory(self.fixture, ['WAIT', 'CURRENT'], now=3969)[3], [30])
        with self.assertRaises(AssertionError): wait_memory(self.fixture, ['WAIT', 'CURRENT'], now=3970)

    def test_changed_identity_selection_generation_or_unknown_clock_is_never_wait(self):
        bound = snapshot()
        for changed in [snapshot(generation=0), snapshot(card='other'), snapshot(questionObject=8), snapshot(selection='other'),
                        snapshot(stage='ENTRY'), snapshot(999), snapshot(2301)]:
            self.assertEqual(self.classify(bound, changed, 2300, 4000), 'INVALID')
        for changed in [snapshot(None), snapshot(-1), snapshot(2**63-1), snapshot(generation=0), snapshot(generation=9)]:
            self.assertEqual(self.classify(changed, snapshot(), 2300, 4000, recorded=bound), 'INVALID')
        self.assertEqual(self.classify(bound, snapshot(None, eligible=False), 999, 4000), 'INVALID')
        for kwargs in [dict(card=False), dict(generation=False)]:
            self.assertEqual(wait_memory(self.fixture, [], **kwargs)[2:], ([], []))
        refused = wait_memory(self.fixture, ['WAIT', 'INVALID', 'CURRENT'])
        self.assertFalse(refused[0]); self.assertEqual(len(refused[2]), 2)

    def test_observation_cannot_rebind_or_access_a_native_node(self):
        body = self.probe.split('// BEGIN EARLY_REVIEW_FOCUS_WAIT classification', 1)[1].split('// END EARLY_REVIEW_FOCUS_WAIT classification', 1)[0]
        for forbidden in ('SystemClock', 'append(', 'observe(', 'committed(', 'sleep(', 'launch(', 'delay(', 'performAction'):
            self.assertNotIn(forbidden, body)
        current = self.fixture.split('        fun current(): Boolean {', 1)[1].split('        private fun awaitCurrentReview(', 1)[0]
        self.assertLess(current.index('if (!currentCard()) return false'), current.index('awaitCurrentReview(bound)'))
        self.assertLess(current.index('if (reviewStart == null) return true'), current.index('awaitCurrentReview(bound)'))
        self.assertLess(current.index('awaitCurrentReview(bound)'), current.index('currentReview(bound,'))


class DisclosureRevealBindingSource(unittest.TestCase):
    def setUp(self):
        self.fixture = (ROOT / FIXTURE).read_text()
        self.before = reverse_disclosure_reveal(self.fixture)

    def test_later_exact_projection_preserves_prior_patch_and_every_other_fixture_statement(self):
        self.assertEqual(hashlib.sha256(self.before.encode()).hexdigest(),
                         '48e42224d7358f2f9bd98758a19bb1ba66d42b756aa8bc77ce7cfb5f9d8a3f2a')
        self.assertEqual(hashlib.sha256(_focus_projection.PATCH.read_bytes()).hexdigest(),
                         '36aba7369aff6650774184d1283ed80f47af9d32aa79ea836f76fc81475720d4')
        self.assertEqual(hashlib.sha256(reverse_focus_wait(FIXTURE, self.fixture).encode()).hexdigest(), BASELINES[FIXTURE])
        for start, end in [('    private fun bindDisclosureScroll(', '    private fun screenshot('),
                           ('    private fun await(', '    private fun find('),
                           ('            tap("Approve for this session"); reviewed()\n            proveBroaderDraft()',
                            '            boundary("complete", "lifecycle_no_submit")')]:
            self.assertEqual(reverse_confirmation_order(self.fixture).split(start, 1)[1].split(end, 1)[0], self.before.split(start, 1)[1].split(end, 1)[0])

    def test_transient_option_lookup_has_old_failure_and_retains_one_current_disclosure(self):
        with self.assertRaisesRegex(AssertionError, 'Current disclosure anchor is not visible'):
            disclosure_preparation_memory(self.before, [True, False])
        disclosure, owned, state = disclosure_preparation_memory(self.fixture, [True, False, True, True])
        self.assertIs(disclosure, owned)
        self.assertEqual(state['binds'], ['header'])
        self.assertEqual(state['scrolls'], 1)
        self.assertEqual(state['now'], 30)
        # Resume uses the same native node: no option/header acquisition is present.
        resume = self.fixture.split('await("same current card body restored")', 1)[1].split('prepareEarlySendAnchor(lifecycleSend)', 1)[0]
        self.assertIn('disclosureScrollReady(lifecycleDisclosure)', resume)
        self.assertNotIn('bindDisclosureScroll', resume)

    def test_ordinary_visible_option_keeps_offscreen_header_short_circuit(self):
        for source in (self.before, self.fixture):
            disclosure, _, state = disclosure_preparation_memory(source, [True], header=False, capture=False)
            self.assertIsNone(disclosure)
            self.assertEqual(state['binds'], [])
            self.assertEqual(state['scrolls'], 0)

    def test_capture_refuses_unknown_or_stale_ownership_and_keeps_original_bound(self):
        for facts in (dict(header=False), dict(ready=False)):
            with self.subTest(facts=facts), self.assertRaises(AssertionError):
                disclosure_preparation_memory(self.fixture, [True], **facts)
        with self.assertRaisesRegex(AssertionError, 'original preparation deadline'):
            disclosure_preparation_memory(self.fixture, [False])
        guards = self.fixture.split('    private fun bindDisclosureScroll(', 1)[1].split('    private fun screenshot(', 1)[0]
        for exact in ('anchor.refresh() && exactVisible(anchor, label)',
                      'anchor.packageName?.toString() == targetContext.packageName',
                      'check(window >= 0)', 'current.refresh() && current.windowId == window',
                      'current.packageName?.toString() == targetContext.packageName',
                      'check(parents++ < 16)', 'active.windowId != window',
                      'active.packageName?.toString() != targetContext.packageName'):
            self.assertIn(exact, guards)

    def test_changed_missing_or_duplicated_later_hunks_refuse(self):
        patch = _focus_projection.DISCLOSURE_PATCH.read_text()
        for hunk in re.split(r'(?m)^@@[^\n]*\n', patch)[1:]:
            new = ''.join(line[1:] for line in hunk.splitlines(keepends=True) if line[0] in ' +')
            self.assertEqual(self.fixture.count(new), 1)
            for changed in (new + '// changed reviewed preparation\n', '', new + new):
                with self.subTest(hunk=new[:80], replacement=changed[:80]), self.assertRaises(AssertionError):
                    reverse_focus_wait(FIXTURE, self.fixture.replace(new, changed, 1))


class ConfirmationObservationOrderSource(unittest.TestCase):
    def setUp(self):
        self.fixture = (ROOT / FIXTURE).read_text()
        self.before = reverse_confirmation_order(self.fixture)

    def test_exact_permutation_preserves_all_old_oracles_and_establishes_owned_early_read_first(self):
        self.assertEqual(hashlib.sha256(self.before.encode()).hexdigest(),
                         '6d4ff021aad9da5cf4be6dee18eb3f25b6ed50f761d2999882a8110484687160')
        self.assertEqual(hashlib.sha256(reverse_focus_wait(FIXTURE, self.fixture).encode()).hexdigest(), BASELINES[FIXTURE])
        begin = '                // BEGIN LIFECYCLE_READ_DIAGNOSTIC confirm-retire-begin\n'
        end = '                // END LIFECYCLE_READ_DIAGNOSTIC confirm-retire-passed\n'
        moved = begin + self.before.split(begin, 1)[1].split(end, 1)[0] + end
        self.assertEqual(self.before.count(moved), 1); self.assertEqual(self.fixture.count(moved), 1)
        self.assertEqual(self.before.replace(moved, '', 1), self.fixture.replace(moved, '', 1))
        body = self.fixture.split('                lifecycleCheckpoint(LifecycleCheckpoint.BODY_RESTORE_PASSED)', 1)[1].split(
            '            } finally {', 1)[0]
        self.assertLess(body.index('prepareEarlySendAnchor(lifecycleSend)'), body.index('check(count() == 4 && !button("Send answer").isEnabled)'))
        self.assertLess(body.index('check(elapsed >= 0 && elapsed < 3_000)'), body.index('await("background retires confirmation")'))
        for forbidden in ('tap(', 'performAction(', 'injectInputEvent(', 'startActivity(', '.begin(', '.bindReview(', 'model.'):
            self.assertNotIn(forbidden, body)
        before_read = self.fixture.split('await("same Activity regained focus")', 1)[1].split('check(count() == 4 && !button("Send answer").isEnabled)', 1)[0]
        for exact in ('lifecycleActivity === activity && !activity.isFinishing && !activity.isDestroyed',
                      'activity.lifecycle.currentState == androidx.lifecycle.Lifecycle.State.RESUMED && activity.hasWindowFocus()',
                      'lifecycleSend.bindReview()', 'check(lifecycleSend.current())',
                      'check(broaderDraftMatches(false))', 'disclosureScrollReady(lifecycleDisclosure)',
                      'prepareEarlySendAnchor(lifecycleSend)'):
            self.assertIn(exact, before_read)
        prepare = self.fixture.split('    private fun prepareEarlySendAnchor(', 1)[1].split('    private fun cachedSendButton(', 1)[0]
        for exact in ('check(cached.current())', 'check(SystemClock.elapsedRealtime() < end)',
                      'fresh.windowId == cached.window', 'fresh.packageName?.toString() == targetContext.packageName',
                      'check(fresh.isVisibleToUser)', 'fresh.text?.toString() == "Send answer"'):
            self.assertIn(exact, prepare)

    def test_recorded_cost_has_old_deadline_failure_and_keeps_both_oracles_after_reorder(self):
        old_trace = []
        with self.assertRaisesRegex(AssertionError, 'original early-review deadline'):
            confirmation_read_memory(self.before, trace=old_trace)
        self.assertEqual(old_trace, ['absence', 'anchor', 'disabled'])
        trace, elapsed_after_reads = confirmation_read_memory(self.fixture)
        self.assertEqual(trace, ['anchor', 'disabled', 'elapsed', 'absence'])
        self.assertEqual(elapsed_after_reads, 3000)

    def test_still_present_confirmation_and_original_absence_bound_never_complete(self):
        trace = []
        with self.assertRaisesRegex(AssertionError, 'mandatory native confirmation absence refused'):
            confirmation_read_memory(self.fixture, absent=False, trace=trace)
        self.assertEqual(trace, ['anchor', 'disabled', 'elapsed', 'absence'])
        with self.assertRaisesRegex(AssertionError, 'original absence10s bound'):
            confirmation_read_memory(self.fixture, absence_cost=10_001)

    def test_reorder_projection_refuses_changed_missing_or_duplicated_exact_hunks(self):
        patch = _focus_projection.CONFIRMATION_ORDER_PATCH.read_text()
        for hunk in re.split(r'(?m)^@@[^\n]*\n', patch)[1:]:
            new = ''.join(line[1:] for line in hunk.splitlines(keepends=True) if line[0] in ' +')
            self.assertEqual(self.fixture.count(new), 1)
            for changed in (new + '// changed reviewed observation\n', '', new + new):
                with self.subTest(hunk=new[:80], replacement=changed[:80]), self.assertRaises(AssertionError):
                    reverse_focus_wait(FIXTURE, self.fixture.replace(new, changed, 1))


if __name__ == '__main__':
    unittest.main()
