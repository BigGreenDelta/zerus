"""Fixture-only phase evidence; never changes a gateway or cleanup deadline."""
from contextlib import contextmanager
import faulthandler
from functools import wraps
import sys


class DiagnosticFailure(RuntimeError):
    pass


class GatewayDiagnostics:
    # One dump per stalled phase, well before the unchanged 900-second wrapper.
    # A new phase rearms it; repeat=False also bounds output if custody is unknown.
    STACK_AFTER_SECONDS = 120

    def __init__(self, stream=None, *, dump_later=None, cancel_dump=None):
        self.stream = sys.stderr if stream is None else stream
        self.dump_later = faulthandler.dump_traceback_later if dump_later is None else dump_later
        self.cancel_dump = faulthandler.cancel_dump_traceback_later if cancel_dump is None else cancel_dump
        self.problems = []
        self.closed = False

    def mark(self, phase, event):
        try:
            self.stream.write(f"gateway diagnostic: phase={phase} event={event}\n")
            self.stream.flush()
        except Exception:
            self.problems.append("phase output failed")

    def _arm(self):
        try:
            self.dump_later(self.STACK_AFTER_SECONDS, repeat=False, file=self.stream, exit=False)
        except Exception:
            self.problems.append("stack retention failed")

    def start(self):
        self.mark("diagnostics", "arming")
        self._arm()
        if not self.problems:
            self.mark("diagnostics", "armed")
        if self.problems:
            raise DiagnosticFailure("gateway diagnostics could not be armed")

    @contextmanager
    def phase(self, name):
        self.mark(name, "enter")
        self._arm()
        try:
            yield
        except BaseException:
            self.mark(name, "error")
            raise
        else:
            self.mark(name, "exit")
        finally:
            # Diagnostic faults must not bypass a required owned cleanup.
            if not self.closed:
                self._arm()

    def close(self):
        # Registered first by setUp, therefore reached after ALL other cleanups.
        self.mark("diagnostics", "cancelling-after-cleanups")
        try:
            self.cancel_dump()
        except Exception:
            self.problems.append("stack cancellation failed")
            self.mark("diagnostics", "cancellation-error")
        else:
            self.mark("diagnostics", "disarmed-after-cleanups")
        self.closed = True
        if self.problems:
            raise DiagnosticFailure("gateway diagnostic retention failed")


def gateway_phase(name):
    """The label is a source literal, never argv, payload or exception text."""
    def decorate(method):
        @wraps(method)
        def traced(self, *args, **kwargs):
            with self.diagnostics.phase(name):
                return method(self, *args, **kwargs)
        return traced
    return decorate
