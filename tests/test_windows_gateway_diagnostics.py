"""Portable diagnostics only; no Windows fixture, gateway or agent is run."""
import ast
import io
from pathlib import Path
import subprocess
import sys
import textwrap
import unittest

from windows_gateway_diagnostics import DiagnosticFailure, GatewayDiagnostics, gateway_phase


class FlushedLog(io.StringIO):
    def __init__(self):
        super().__init__()
        self.flushes = 0

    def flush(self):
        self.flushes += 1
        super().flush()


class Diagnostics(unittest.TestCase):
    def make_diagnostics(self):
        stream, events = FlushedLog(), []
        diagnostic = GatewayDiagnostics(stream,
            dump_later=lambda *args, **kwargs: events.append(("arm", args, kwargs)),
            cancel_dump=lambda: events.append(("cancel",)))
        return diagnostic, stream, events

    def test_fixed_flushed_phases_rearm_one_shot_without_changing_deadlines(self):
        diagnostic, stream, events = self.make_diagnostics()
        diagnostic.start()
        with diagnostic.phase("heartbeat"):
            self.assertEqual(events[-1][0], "arm")
        diagnostic.close()
        self.assertEqual(stream.getvalue().splitlines(), [
            "gateway diagnostic: phase=diagnostics event=arming",
            "gateway diagnostic: phase=diagnostics event=armed",
            "gateway diagnostic: phase=heartbeat event=enter",
            "gateway diagnostic: phase=heartbeat event=exit",
            "gateway diagnostic: phase=diagnostics event=cancelling-after-cleanups",
            "gateway diagnostic: phase=diagnostics event=disarmed-after-cleanups"])
        self.assertEqual(stream.flushes, 6)
        self.assertEqual(len(events), 4)
        for _, args, kwargs in events[:-1]:
            self.assertEqual(args, (120,))
            self.assertEqual(kwargs, {"repeat": False, "file": stream, "exit": False})
        self.assertEqual(events[-1], ("cancel",))

    def test_exception_identity_and_private_values_are_not_logged(self):
        diagnostic, stream, events = self.make_diagnostics()
        secret = "synthetic-secret-config-token-key-body"
        error = RuntimeError(secret)
        diagnostic.start()
        class Subject:
            diagnostics = diagnostic

            @gateway_phase("manage")
            def call(self, payload):
                raise error
        with self.assertRaises(RuntimeError) as caught:
            Subject().call({"token": secret})
        self.assertIs(caught.exception, error)
        self.assertIn("phase=manage event=error", stream.getvalue())
        self.assertNotIn(secret, stream.getvalue())
        self.assertNotIn("payload", stream.getvalue())
        self.assertNotEqual(events[-1], ("cancel",))
        diagnostic.close()

    def fixture_case(self, *, setup_error=False, cleanup_error=None):
        """Load exact fixture admission/cleanup definitions, not its native module.

        All inherited setup, host, server and connector callbacks are memory fakes.
        This exercises actual registration/order without a Windows API or profile.
        """
        diagnostic, stream, timer = self.make_diagnostics()
        order = []
        class Parent(unittest.TestCase):
            def setUp(self):
                order.append("inherited-setup")
                self.assertGreater(len(timer), 0)
                self.addCleanup(self.root_cleanup)
                self.addCleanup(self.stop_host)
                if setup_error:
                    raise RuntimeError("synthetic inherited setup failure")

            def root_cleanup(self):
                order.append("root-cleanup")
                self.assertFalse(diagnostic.closed)
                self.assertNotEqual(timer[-1], ("cancel",))
                if cleanup_error == "root":
                    raise RuntimeError("synthetic root cleanup failure")

            def stop_host(self):
                order.append("host-cleanup")
                self.assertFalse(diagnostic.closed)

            def runTest(self):
                order.append("body")

        filename = Path(__file__).with_name("test_windows_mobile_gateway.py")
        source = ast.parse(filename.read_text())
        fixture = next(node for node in source.body if isinstance(node, ast.ClassDef) and node.name == "Gateway")
        fixture.body = [node for node in fixture.body if isinstance(node, ast.FunctionDef)
                        and node.name in ("setUp", "doCleanups", "stop_host")]
        namespace = {"LifecycleCase": Parent, "GatewayDiagnostics": lambda: diagnostic,
                     "gateway_phase": gateway_phase}
        exec(compile(ast.Module(body=[fixture], type_ignores=[]), str(filename), "exec"), namespace)
        case_type = namespace["Gateway"]

        @gateway_phase("setup-gateway")
        def setup_gateway(case):
            order.append("gateway-setup")
            @gateway_phase("cleanup-server")
            def close_server(case):
                order.append("server-cleanup")
                case.assertFalse(diagnostic.closed)
            @gateway_phase("cleanup-connector")
            def close_connector(case):
                order.append("connector-cleanup")
                case.assertFalse(diagnostic.closed)
                if cleanup_error == "connector":
                    raise RuntimeError("synthetic connector cleanup failure")
            case.addCleanup(close_server, case)
            case.addCleanup(close_connector, case)
        case_type.set_up_gateway = setup_gateway
        return case_type(), diagnostic, stream, timer, order

    def test_actual_fixture_registers_cancellation_after_every_cleanup(self):
        case, diagnostic, stream, timer, order = self.fixture_case()
        result = unittest.TestResult()
        case.run(result)
        self.assertTrue(result.wasSuccessful(), result.errors)
        self.assertEqual(order, ["inherited-setup", "gateway-setup", "body",
                                "connector-cleanup", "server-cleanup", "host-cleanup", "root-cleanup"])
        self.assertTrue(diagnostic.closed)
        self.assertEqual(timer[-1], ("cancel",))
        self.assertLess(stream.getvalue().index("phase=cleanup-host event=exit"),
                        stream.getvalue().index("disarmed-after-cleanups"))
        self.assertIn("phase=cleanup-all event=enter", stream.getvalue())

    def test_partial_inherited_setup_keeps_watchdog_through_registered_cleanups(self):
        case, diagnostic, stream, timer, order = self.fixture_case(setup_error=True)
        result = unittest.TestResult()
        case.run(result)
        self.assertEqual(len(result.errors), 1)
        self.assertIn("synthetic inherited setup failure", result.errors[0][1])
        self.assertEqual(order, ["inherited-setup", "host-cleanup", "root-cleanup"])
        self.assertIn("phase=setup-inherited event=error", stream.getvalue())
        self.assertEqual(timer[-1], ("cancel",))
        self.assertTrue(diagnostic.closed)

    def test_cleanup_failure_keeps_other_callbacks_and_final_cancellation(self):
        for failing in ("connector", "root"):
            with self.subTest(failing=failing):
                case, diagnostic, stream, timer, order = self.fixture_case(cleanup_error=failing)
                result = unittest.TestResult()
                case.run(result)
                self.assertEqual(len(result.errors), 1)
                self.assertIn(f"synthetic {failing} cleanup failure", result.errors[0][1])
                self.assertEqual(order[-4:], ["connector-cleanup", "server-cleanup", "host-cleanup", "root-cleanup"])
                self.assertTrue(diagnostic.closed)
                self.assertEqual(timer[-1], ("cancel",))

    def test_initial_arm_failure_refuses_before_inherited_setup(self):
        case, diagnostic, stream, timer, order = self.fixture_case()
        def cannot_arm(*args, **kwargs):
            raise OSError("synthetic private timer failure")
        diagnostic.dump_later = cannot_arm
        result = unittest.TestResult()
        case.run(result)
        self.assertEqual(order, [])
        self.assertEqual(len(result.errors), 2)  # startup refusal + retained diagnostic failure
        self.assertIn("could not be armed", result.errors[0][1])
        self.assertNotIn("event=armed", stream.getvalue())
        self.assertEqual(timer[-1], ("cancel",))

    def test_later_diagnostic_failure_does_not_bypass_owned_cleanup(self):
        case, diagnostic, stream, timer, order = self.fixture_case()
        arm = diagnostic.dump_later
        def fail_after_setup(*args, **kwargs):
            if "gateway-setup" in order:
                raise OSError("synthetic private timer failure")
            return arm(*args, **kwargs)
        diagnostic.dump_later = fail_after_setup
        result = unittest.TestResult()
        case.run(result)
        self.assertEqual(order[-4:], ["connector-cleanup", "server-cleanup", "host-cleanup", "root-cleanup"])
        self.assertEqual(len(result.errors), 1)
        self.assertIn("diagnostic retention failed", result.errors[0][1])
        self.assertTrue(diagnostic.closed)
        self.assertEqual(timer[-1], ("cancel",))
        self.assertNotIn("synthetic private timer failure", stream.getvalue())

    def test_cancel_failure_is_a_failure_after_all_owned_cleanup(self):
        case, diagnostic, stream, timer, order = self.fixture_case()
        def cannot_cancel():
            raise OSError("synthetic private cancellation failure")
        diagnostic.cancel_dump = cannot_cancel
        result = unittest.TestResult()
        case.run(result)
        self.assertEqual(order[-1], "root-cleanup")
        self.assertEqual(len(result.errors), 1)
        self.assertIn("diagnostic retention failed", result.errors[0][1])
        self.assertNotIn("synthetic private cancellation failure", stream.getvalue())
        self.assertIn("event=cancellation-error", stream.getvalue())
        self.assertNotIn("event=disarmed-after-cleanups", stream.getvalue())

    def test_real_python_watchdog_retains_main_and_worker_stacks(self):
        # One disposable Python process, no native fixture import or resource.
        code = textwrap.dedent("""
            import sys, threading
            sys.path.insert(0, sys.argv[1])
            from windows_gateway_diagnostics import GatewayDiagnostics
            ready, release = threading.Event(), threading.Event()
            def diagnostic_worker():
                ready.set()
                release.wait(2)
            worker = threading.Thread(target=diagnostic_worker)
            worker.start()
            assert ready.wait(1)
            diagnostic = GatewayDiagnostics()
            diagnostic.STACK_AFTER_SECONDS = 0.05  # portable timer probe only
            diagnostic.start()
            diagnostic.mark('portable-timer-probe', 'enter')
            release.wait(0.5)
            diagnostic.close()
            release.set()
            worker.join(1)
            assert not worker.is_alive()
        """)
        result = subprocess.run([sys.executable, "-I", "-B", "-c", code, str(Path(__file__).parent.resolve())],
                                stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Timeout (", result.stderr)
        self.assertIn("diagnostic_worker", result.stderr)
        self.assertIn('File "<string>"', result.stderr)
        self.assertIn("phase=portable-timer-probe event=enter", result.stderr)
        self.assertIn("disarmed-after-cleanups", result.stderr)
        self.assertEqual(result.stderr.count("Timeout ("), 1)


if __name__ == "__main__":
    unittest.main()
