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


if __name__ == '__main__':
    unittest.main()
