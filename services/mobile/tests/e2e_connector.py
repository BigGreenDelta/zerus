"""The outbound connector of an end-to-end suite, in either implementation.

`python` runs this tree's connector in-process, as these suites always did.
`rust` runs the command in ZERUS_CONNECTOR_E2E_COMMAND (a built Rust
connector) as its own process against the same relay, with a private
configuration file and only the variables a deployed unit would have. Without
the variable only Python runs; ZERUS_REQUIRE_RELAY_E2E=1, which CI sets, makes
that a failure.
"""
import asyncio
import contextlib
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
from unittest.mock import patch

from zerus_mobile.connector import Connector

COMMAND = "ZERUS_CONNECTOR_E2E_COMMAND"
REQUIRED = "ZERUS_REQUIRE_RELAY_E2E"


def implementations() -> list[str]:
    if os.environ.get(COMMAND):
        return ["python", "rust"]
    if os.environ.get(REQUIRED) == "1":
        raise AssertionError(f"{REQUIRED}=1: the end-to-end suites need {COMMAND} naming a built Rust connector")
    return ["python"]


class Running:
    """A started connector; `check()` raises once it has stopped on its own."""

    def __init__(self, task=None, process=None, log: Path | None = None):
        self.task, self.process, self.log = task, process, log

    def check(self) -> None:
        if self.task is not None and self.task.done():
            self.task.result()
            raise AssertionError("the Python connector stopped")
        if self.process is not None and self.process.poll() is not None:
            output = self.log.read_text(errors="replace")[-3000:] if self.log else ""
            raise AssertionError(f"the Rust connector exited with {self.process.returncode}:\n{output}")


@contextlib.asynccontextmanager
async def running(implementation: str, config: dict, root: Path, environment: dict | None = None):
    """Run a connector with `config` (plus loopback HTTP) until the block ends."""
    environment = environment or {}
    if implementation == "python":
        connector = Connector(config, allow_insecure_localhost=True)
        with patch.dict(os.environ, environment):
            task = asyncio.create_task(connector.run())
            try:
                yield Running(task=task)
            finally:
                task.cancel()
                await asyncio.gather(task, return_exceptions=True)
                connector.journal.close()
        return
    home = root / "connector-home"
    home.mkdir(mode=0o700, exist_ok=True)
    path = root / "connector.json"
    path.write_text(json.dumps(config))
    path.chmod(0o600)
    log = root / "connector.log"
    env = {"HOME": str(home), "PATH": "/usr/bin:/bin", "TMPDIR": str(home), **environment}
    with open(log, "wb") as output:
        process = subprocess.Popen([*shlex.split(os.environ[COMMAND]), "--config", str(path), "--allow-insecure-localhost"],
                                   cwd=root, env=env, stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT,
                                   start_new_session=True)
    try:
        yield Running(process=process, log=log)
    finally:
        with contextlib.suppress(ProcessLookupError, PermissionError):
            os.killpg(process.pid, signal.SIGKILL)
        await asyncio.to_thread(process.wait)
