"""Run the connector HTTP suites against a private relay.

With ZERUS_RELAY_BINARY, each fixture runs that Rust relay on its own
PostgreSQL database, relay_test_<uuid>, created on the disposable server that
ZERUS_RELAY_TEST_DATABASE_URL names and dropped afterwards. Credentials come
only from the zerus-relay command line and /v1/pair. Without the URL the suites
skip; ZERUS_REQUIRE_RELAY_E2E=1, which CI sets, makes that a failure. Without
the binary, the retained Python reference relay serves a private SQLite store
until that reference is deleted. No fixture touches a native agent, deployed
database, host configuration or non-fixture service, or prints the URL.
"""
import asyncio
import atexit
import contextlib
import json
import os
from pathlib import Path
import socket
import tempfile
import unittest
from urllib.parse import urlsplit
import uuid

from aiohttp import ClientSession

DATABASE_URL = 'ZERUS_RELAY_TEST_DATABASE_URL'
REQUIRED = 'ZERUS_REQUIRE_RELAY_E2E'
# Rust relays whose database still exists. Ctrl+C makes unittest skip cleanups,
# so the exit hook below stops and drops these; a killed interpreter leaves its
# databases to the disposable server.
UNFINISHED = set()


def unavailable(reason):
    """Skip the calling test, or fail it when these suites are required."""
    if os.environ.get(REQUIRED) == '1':
        raise AssertionError(f'{REQUIRED}=1: {reason}')
    raise unittest.SkipTest(reason)


async def start_relay(*, rust_only=False, workspace='Example workspace',
                      computer='Example computer', **settings):
    """A served relay with one workspace and one computer credential.

    Keyword settings are relay configuration fields; background maintenance
    is off unless a test enables it.
    """
    settings = {'background': False, **settings}
    binary = os.environ.get('ZERUS_RELAY_BINARY')
    if not binary:
        if rust_only:
            unavailable('this suite needs ZERUS_RELAY_BINARY naming the built zerus-relay')
        return await ReferenceRelay.start(workspace, computer, settings)
    server = os.environ.get(DATABASE_URL)
    if not server:
        unavailable(f'Rust relay suites need {DATABASE_URL} naming a disposable PostgreSQL '
                    'server whose role may create databases; see services/mobile/relay/README.md')
    return await RustRelay.start(Path(binary).resolve(), server, workspace, computer, settings)


async def admin(server, statement):
    import asyncpg  # Test-only, pinned in services/mobile/tests/requirements.txt.
    connection = await asyncpg.connect(server, timeout=10)
    try:
        await connection.execute(statement)
    finally:
        await connection.close()


def drop(database):
    # FORCE also closes connections a killed relay left behind.
    return f'DROP DATABASE IF EXISTS {database} WITH (FORCE)'


@atexit.register
def drop_unfinished():
    """Stop the Rust relays of an interrupted run and drop their databases."""
    for relay in list(UNFINISHED):
        if relay.process is not None:
            # Popen never signals a child that has already been reaped.
            with contextlib.suppress(Exception):
                relay.process.kill()
        with contextlib.suppress(Exception):
            asyncio.run(admin(relay.server, drop(relay.database)))


class Relay:
    """The served URL, its workspace, the computer credential and its invitation."""

    url = workspace = node = invitation = None

    def __init__(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='zerus-relay-fixture-')
        self.root = Path(self.temporary.name)

    async def pair(self, device_name, code=None):
        """Pair a phone over HTTP, with a new invitation unless one is given."""
        code = code or (await self.invite())['pair_code']
        async with ClientSession() as client:
            async with client.post(self.url + '/v1/pair',
                                   json={'code': code, 'device_name': device_name}) as response:
                if response.status != 200:
                    raise RuntimeError(f'fixture pairing failed with HTTP {response.status}')
                return await response.json()


class RustRelay(Relay):
    process = log = None
    created = False

    @classmethod
    async def start(cls, binary, server, workspace, computer, settings):
        relay = cls(binary, server)
        try:
            await relay._start(workspace, computer, settings)
        except BaseException:
            await relay.cleanup()
            raise
        return relay

    def __init__(self, binary, server):
        super().__init__()
        self.binary, self.server = binary, server
        self.database = 'relay_test_' + uuid.uuid4().hex
        self.config = self.root / 'relay.json'
        # HOME is private too, so no command can reach the default SQLite path.
        self.environment = {**os.environ, 'HOME': str(self.root)}

    async def _start(self, workspace, computer, settings):
        await admin(self.server, f'CREATE DATABASE {self.database}')
        self.created = True
        UNFINISHED.add(self)
        url = urlsplit(self.server)._replace(path='/' + self.database).geturl()
        descriptor = os.open(self.config, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(descriptor, 'w') as handle:
            json.dump({**settings, 'database_url': url}, handle)
        provisioned = await self.cli('provision', '--name', workspace, '--computer-name', computer)
        self.workspace = provisioned['workspace_id']
        self.node = {key: provisioned[key] for key in ('node_id', 'node_token')}
        self.invitation = {key: provisioned[key] for key in ('pair_code', 'expires_at')}
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port = reservation.getsockname()[1]
        self.log = (self.root / 'relay.log').open('wb')
        self.process = await asyncio.create_subprocess_exec(
            str(self.binary), '--config', str(self.config), 'serve', '--port', str(port),
            stdout=asyncio.subprocess.DEVNULL, stderr=self.log, env=self.environment)
        self.url = f'http://127.0.0.1:{port}'
        async with ClientSession() as client:
            for _ in range(400):
                if self.process.returncode is not None:
                    raise RuntimeError('Rust relay exited during fixture startup: ' + self.output())
                try:
                    async with client.get(self.url + '/readyz') as response:
                        if response.status == 200:
                            return
                except OSError:
                    pass
                await asyncio.sleep(0.025)
        raise RuntimeError('Rust relay fixture did not become ready: ' + self.output())

    def output(self):
        # The relay never logs SQL, bodies, tokens or URLs.
        return (self.root / 'relay.log').read_text(errors='replace')[-2000:].strip()

    async def cli(self, *arguments):
        process = await asyncio.create_subprocess_exec(
            str(self.binary), '--config', str(self.config), *arguments,
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE, env=self.environment)
        try:
            stdout, stderr = await asyncio.wait_for(process.communicate(), 60)
        except BaseException:
            if process.returncode is None:
                process.kill()
                await process.wait()
            raise
        if process.returncode:
            raise RuntimeError(f'zerus-relay {arguments[0]} exited {process.returncode}: '
                               + stderr.decode(errors='replace').strip())
        return json.loads(stdout)

    async def add_node(self, name):
        return await self.cli('node', '--workspace', self.workspace, '--name', name)

    async def invite(self):
        return await self.cli('invite', '--workspace', self.workspace)

    async def revoke(self, kind, identity):
        """Revoke a device, node or computer through the command line."""
        await self.cli('revoke-' + kind, '--id', identity)

    async def cleanup(self):
        try:
            if self.process is not None and self.process.returncode is None:
                self.process.terminate()
                try:
                    await asyncio.wait_for(self.process.wait(), 45)
                except asyncio.TimeoutError:
                    self.process.kill()
                    await self.process.wait()
        finally:
            try:
                if self.created:
                    await admin(self.server, drop(self.database))
                    self.created = False
                    UNFINISHED.discard(self)
            finally:
                if self.log is not None:
                    self.log.close()
                self.temporary.cleanup()


class ReferenceRelay(Relay):
    """The Python reference relay on a private SQLite store, until it is deleted."""

    runner = store = None

    @classmethod
    async def start(cls, workspace, computer, settings):
        from aiohttp import web
        from zerus_mobile.server import Config, create_app
        from zerus_mobile.store import Store
        relay = cls()
        try:
            relay.store = Store(relay.root / 'relay.sqlite3')
            relay.workspace = relay.store.workspace(workspace)
            relay.node = relay.store.node(relay.workspace, computer)
            relay.invitation = relay.store.invite(relay.workspace)
            relay.runner = web.AppRunner(create_app(relay.store, Config(**settings)))
            await relay.runner.setup()
            site = web.TCPSite(relay.runner, '127.0.0.1', 0)
            await site.start()
            relay.url = 'http://127.0.0.1:' + str(site._server.sockets[0].getsockname()[1])
        except BaseException:
            await relay.cleanup()
            raise
        return relay

    async def add_node(self, name):
        return self.store.node(self.workspace, name)

    async def invite(self):
        return self.store.invite(self.workspace)

    async def revoke(self, kind, identity):
        found = (self.store.revoke_computer(identity) if kind == 'computer'
                 else self.store.revoke(kind + 's', identity))
        if not found:
            raise RuntimeError(f'fixture {kind} was not found')

    async def cleanup(self):
        try:
            if self.runner is not None:
                await self.runner.cleanup()
        finally:
            if self.store is not None:
                self.store.close()
            self.temporary.cleanup()
