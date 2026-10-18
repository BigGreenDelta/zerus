"""Strict contracts for the Rust-only OpenCode launch-effort and queue-removal capabilities."""
import copy
import unittest
from unittest.mock import patch

import differential
from differential import canonical, differences

FEATURE = "opencode_launch_effort"
REMOVAL = "remove_queued"
# The Python operations the frozen lifecycle marker gates.
ACTIONS = ["archive", "forget", "fork", "pause", "rename", "restore", "resume", "terminate"]


def trace(shape, kind="heartbeat", count=1):
    return {"native": {}, "relay": {"SHAPE " + kind + " " + canonical(shape): count}, "journal": []}


def shapes(launch=True):
    python = {"body": ["presence", "snapshot"], "snapshot": ["mobile_capabilities", "sessions"],
              "operations": ["inspect", "launch"] if launch else ["inspect"], "features": ["inspect_after"]}
    rust = copy.deepcopy(python)
    if launch:
        rust["features"] = sorted([*rust["features"], FEATURE])
    return python, rust


class CapabilityProjectionTests(unittest.TestCase):
    def test_only_the_exact_declared_extension_matches_without_rewriting_traces(self):
        for kind in ("heartbeat", "peer heartbeat"):
            for launch in (False, True):
                with self.subTest(kind=kind, launch=launch):
                    python, rust = shapes(launch)
                    left, right = trace(python, kind), trace(rust, kind)
                    original = copy.deepcopy((left, right))
                    self.assertEqual(differences(left, right), ([], 0))
                    self.assertEqual((left, right), original)

    def test_missing_rust_feature_fails_even_when_both_raw_shapes_are_equal(self):
        python, _ = shapes()
        self.assertTrue(differences(trace(python), trace(python))[0])

    def test_python_never_advertises_the_feature_even_if_both_sides_do(self):
        _, rust = shapes()
        self.assertTrue(differences(trace(rust), trace(rust))[0])

    def test_no_launch_never_authorizes_the_feature_on_either_side(self):
        python, rust = shapes(False)
        for side in ("python", "rust", "both"):
            with self.subTest(side=side):
                left, right = copy.deepcopy(python), copy.deepcopy(rust)
                if side in ("python", "both"):
                    left["features"].append(FEATURE)
                if side in ("rust", "both"):
                    right["features"].append(FEATURE)
                self.assertTrue(differences(trace(left), trace(right))[0])

    def test_reverse_duplicate_or_malformed_features_are_not_projection(self):
        python, rust = shapes()
        for malformed in ([FEATURE, FEATURE], FEATURE, [False], None, [FEATURE, "inspect_after", "inspect_after"]):
            for side in ("python", "rust"):
                with self.subTest(side=side, malformed=malformed):
                    left, right = copy.deepcopy(python), copy.deepcopy(rust)
                    (left if side == "python" else right)["features"] = malformed
                    self.assertTrue(differences(trace(left), trace(right))[0])
        self.assertTrue(differences(trace(rust), trace(python))[0])

    def test_all_other_shape_fields_stay_exact(self):
        python, rust = shapes()
        for kind in ("heartbeat", "peer heartbeat"):
            for key in python:
                for side in ("python", "rust"):
                    with self.subTest(kind=kind, key=key, side=side):
                        left, right = copy.deepcopy(python), copy.deepcopy(rust)
                        target = left if side == "python" else right
                        target[key] = sorted([*target[key], "unrelated"])
                        self.assertTrue(differences(trace(left, kind), trace(right, kind))[0])
        right = copy.deepcopy(rust)
        right["operations"].remove("launch")
        self.assertTrue(differences(trace(python), trace(right))[0])

    def test_unexpected_keys_invalid_arrays_and_duplicate_shapes_fail_closed(self):
        python, rust = shapes()
        for side in ("python", "rust"):
            for key, value in (("unknown", []), ("body", "snapshot"), ("snapshot", [0]), ("operations", ["launch", "inspect"])):
                with self.subTest(side=side, key=key):
                    left, right = copy.deepcopy(python), copy.deepcopy(rust)
                    (left if side == "python" else right)[key] = value
                    self.assertTrue(differences(trace(left), trace(right))[0])
            left, right = trace(python), trace(rust)
            (left if side == "python" else right)["relay"][next(iter((left if side == "python" else right)["relay"]))] = 2
            self.assertTrue(differences(left, right)[0])
        for entry in ("SHAPE heartbeat malformed", 'SHAPE heartbeat {"features":[]}'):
            bad = {"native": {}, "relay": {entry: 1}, "journal": []}
            self.assertTrue(differences(bad, bad)[0])

    def test_native_work_and_journal_counts_are_not_relaxed(self):
        python, rust = shapes()
        for kind, entry in (("native", canonical({"periodic": False, "argv": ["send"]})), ("journal", "exact row")):
            with self.subTest(kind=kind):
                left, right = trace(python), trace(rust)
                left[kind], right[kind] = {entry: 1}, {entry: 2}
                found, _ = differences(left, right)
                self.assertEqual(len(found), 1)
                self.assertEqual((found[0]["kind"], found[0]["side"], found[0]["python"], found[0]["rust"]),
                                 (kind, "count", 1, 2))


def action_shapes(actions=True):
    """Shapes with or without the session actions that gate queue removal."""
    python = {"body": ["presence", "snapshot"], "snapshot": ["mobile_capabilities", "sessions"],
              "operations": sorted([*ACTIONS, "inspect"]) if actions else ["inspect"], "features": ["inspect_after"]}
    rust = copy.deepcopy(python)
    if actions:
        rust["operations"] = sorted([*rust["operations"], REMOVAL])
    return python, rust


class QueueRemovalProjectionTests(unittest.TestCase):
    def test_rust_offers_removal_exactly_with_the_session_actions(self):
        for kind in ("heartbeat", "peer heartbeat"):
            for actions in (False, True):
                with self.subTest(kind=kind, actions=actions):
                    python, rust = action_shapes(actions)
                    left, right = trace(python, kind), trace(rust, kind)
                    original = copy.deepcopy((left, right))
                    self.assertEqual(differences(left, right), ([], 0))
                    self.assertEqual((left, right), original)

    def test_missing_rust_removal_fails_even_when_both_raw_shapes_are_equal(self):
        python, _ = action_shapes()
        self.assertTrue(differences(trace(python), trace(python))[0])

    def test_python_never_advertises_removal_even_if_both_sides_do(self):
        _, rust = action_shapes()
        self.assertTrue(differences(trace(rust), trace(rust))[0])

    def test_no_session_actions_never_authorize_removal_on_either_side(self):
        python, rust = action_shapes(False)
        for side in ("python", "rust", "both"):
            with self.subTest(side=side):
                left, right = copy.deepcopy(python), copy.deepcopy(rust)
                if side in ("python", "both"):
                    left["operations"] = sorted([*left["operations"], REMOVAL])
                if side in ("rust", "both"):
                    right["operations"] = sorted([*right["operations"], REMOVAL])
                self.assertTrue(differences(trace(left), trace(right))[0])

    def test_other_session_actions_stay_exact(self):
        python, rust = action_shapes()
        for action in ("rename", "forget"):
            with self.subTest(action=action):
                right = copy.deepcopy(rust)
                right["operations"].remove(action)
                self.assertTrue(differences(trace(python), trace(right))[0])

    def test_the_table_and_the_frozen_markers_drive_the_rule(self):
        self.assertEqual(differential.FROZEN_MARKERS["lifecycle"], ACTIONS)
        self.assertIn({"operation": REMOVAL, "marker": "lifecycle", "class": "mutation"}, differential.RUST_ONLY)

    def test_an_operation_behind_a_marker_python_does_not_know_is_left_to_the_suites(self):
        row = {"operation": "fixture_read", "marker": "fixture_marker", "class": "read"}
        python, _ = action_shapes(False)
        with patch.object(differential, "RUST_ONLY", [*differential.RUST_ONLY, row]):
            for listed in (False, True):
                with self.subTest(listed=listed):
                    right = copy.deepcopy(python)
                    if listed:
                        right["operations"] = sorted([*right["operations"], "fixture_read"])
                    self.assertEqual(differences(trace(python), trace(right)), ([], 0))
            left = copy.deepcopy(python)
            left["operations"] = sorted([*left["operations"], "fixture_read"])
            self.assertTrue(differences(trace(left), trace(left))[0])


if __name__ == "__main__":
    unittest.main()
