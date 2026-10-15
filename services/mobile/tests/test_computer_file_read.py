"""Read-only explicit file links; no owner files, relay or agents are used."""
import base64
import hashlib
import json
import os
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch
import uuid

from zerus_mobile import file_reads
from zerus_mobile.connector import Connector
from zerus_mobile.context import OPERATIONS, READ_OPERATIONS, READ_SQL
from zerus_mobile.attachments import MAX_READ_BYTES, MAX_FILE_BYTES

SESSION = "codex/example/files"
PATH = "/workspace/example/Сводная итог.xlsx"


def payload(**change):
    value = dict(request_id=str(uuid.uuid4()), expected_run_id="run", expected_conversation_id="conversation",
                 path=PATH, offset=0, length=MAX_READ_BYTES)
    value.update(change)
    return value


def native_file(data, request):
    return dict(request_id=request['request_id'], name=SESSION, run_id='run', conversation_id='conversation',
                requested_path=PATH, path=PATH, file_name=PATH.rsplit('/', 1)[-1], mime='application/octet-stream',
                bytes=len(data), sha256=hashlib.sha256(data).hexdigest(), data_base64=base64.b64encode(data).decode())


class Validation(unittest.TestCase):
    def test_read_class_and_strict_ranges(self):
        self.assertIn('file_read', OPERATIONS)
        self.assertIn('file_read', READ_OPERATIONS)
        self.assertNotIn('file_read', READ_SQL)
        good = payload()
        file_reads.validate(good, good['request_id'])
        for change in [dict(offset=1), dict(offset=True), dict(length=True), dict(offset=-1), dict(length=MAX_READ_BYTES+1),
                       dict(offset=MAX_FILE_BYTES-1, length=2), dict(path=''), dict(path='bad\nfile'), dict(path='bad\u0085file'),
                       dict(path='界'*2731), dict(command='cat file'), dict(sha256='x'*64), dict(archive_id=10),
                       dict(archive_id='0'*36), dict(agent_id='../child')]:
            bad = payload(**change)
            with self.subTest(change=change), self.assertRaises(ValueError):
                file_reads.validate(bad, bad['request_id'])

    def test_bytes_digest_and_original_identity_must_match(self):
        request = payload()
        good = native_file(b'\x00\xffPK', request)
        entry = file_reads.decode(good, SESSION, request)
        self.assertEqual(entry['data'], b'\x00\xffPK')
        for field, value in [('request_id', str(uuid.uuid4())), ('requested_path', '/other.xlsx'), ('run_id', 'other'),
                             ('sha256', 'a'*64), ('file_name', '../bad'), ('bytes', 3.2), ('data_base64', 'bad')]:
            with self.subTest(field=field), self.assertRaises(Exception):
                file_reads.decode({**good, field: value}, SESSION, request)


class ConnectorRead(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.connector = Connector(dict(server_url='https://relay.example.invalid', node_token='fixture-token', state_dir=self.directory.name))
        self.connector.sessions = {SESSION}
        self.connector.file_read_supported = True
        self.connector.snapshot_seen_at = time.monotonic()

    async def asyncTearDown(self):
        self.connector.journal.close()
        self.directory.cleanup()

    async def execute(self, value):
        return await self.connector.execute(dict(request_id=value['request_id'], operation='file_read', session=SESSION, payload=value))

    async def test_unicode_binary_snapshot_is_chunked_once_without_rereading_or_shell(self):
        data = bytes(range(256)) * 5000
        calls = []
        async def native(argv, request, **options):
            calls.append((argv, request, options))
            self.assertEqual(argv, ['file-read', SESSION, '--json'])
            self.assertNotIn('offset', request)
            return native_file(data, request)
        received = b''
        sha = None
        with patch.object(self.connector, 'native', side_effect=native):
            while len(received) < len(data):
                request = payload(offset=len(received), length=min(MAX_READ_BYTES, len(data)-len(received)))
                if sha is not None: request['sha256'] = sha
                response = await self.execute(request)
                self.assertEqual(response['state'], 'completed', response)
                result = response['result']
                sha = result['sha256']
                self.assertEqual(result['requested_path'], PATH)
                received += base64.b64decode(result['data_base64'])
        self.assertEqual(received, data)
        self.assertEqual(len(calls), 1)

    async def test_new_explicit_open_reads_current_bytes_instead_of_a_stale_path_cache(self):
        async def native(argv, request, **options):
            return native_file(contents.pop(0), request)
        contents = [b'first', b'second']
        with patch.object(self.connector, 'native', side_effect=native) as call:
            first, second = await self.execute(payload()), await self.execute(payload())
        self.assertNotEqual(first['result']['sha256'], second['result']['sha256'])
        self.assertEqual(call.call_count, 2)

    async def test_changed_missing_and_unsupported_reads_fail_without_receipts_becoming_mutations(self):
        expected = hashlib.sha256(b'first').hexdigest()
        async def native(argv, request, **options):
            return native_file(b'changed', request)
        with patch.object(self.connector, 'native', side_effect=native):
            result = await self.execute(payload(offset=1, length=2, sha256=expected))
        self.assertEqual(result['state'], 'failed')
        self.assertIn('changed', result['error'])
        self.connector.file_read_supported = False
        with patch.object(self.connector, 'native') as call:
            self.assertEqual((await self.execute(payload()))['state'], 'failed')
            call.assert_not_called()


@unittest.skipUnless(os.environ.get('ZERUS_RELAY_BINARY') and os.environ.get('ZERUS_DIRECTORY_TEST_HGS'),
                     'Explicit isolated Rust relay and native CLI targets required')
class NativeFileHttp(unittest.IsolatedAsyncioTestCase):
    """Exercise the full byte limit through the real HTTP/connector/CLI boundary."""
    async def asyncSetUp(self):
        import test_rust_relay as fixture
        self.fixture = fixture
        await fixture.RustHttpContracts.asyncSetUp(self)

    async def asyncTearDown(self):
        await self.fixture.RustHttpContracts.asyncTearDown(self)

    def headers(self, node=False):
        return self.fixture.RustHttpContracts.headers(self, node)

    async def call(self, *args, **kwargs):
        return await self.fixture.RustHttpContracts.call(self, *args, **kwargs)

    async def test_ten_mib_original_snapshot_and_changed_evicted_copy(self):
        with tempfile.TemporaryDirectory(prefix='zerus-file-http-') as directory:
            root = Path(directory).resolve()
            state = root / 'state'; state.mkdir()
            document = root / 'Сводная итог.xlsx'
            data = bytes(range(256)) * (MAX_FILE_BYTES // 256)
            document.write_bytes(data)
            (state / (hashlib.sha256(SESSION.encode()).hexdigest() + '.json')).write_text(json.dumps(
                dict(version=1, name=SESSION, agent='codex', run_id='run', conversation_id='conversation',
                     cwd=str(root), subagents={})))
            environment = {k: v for k, v in os.environ.items() if not k.startswith(('HGS_', 'OPENCODE_'))}
            environment.update(HOME=str(root), HGS_STATE_DIR=str(state), HGS_CONFIG_DIR=str(root / 'config'))
            connector = Connector(dict(server_url='https://relay.example.invalid', node_token='fixture',
                state_dir=str(root / 'journal'), hgs_path=str(Path(os.environ['ZERUS_DIRECTORY_TEST_HGS']).resolve())))
            connector.sessions = {SESSION}; connector.snapshot_seen_at = time.monotonic()
            try:
                with patch.dict(os.environ, environment, clear=True):
                    await connector.probe_capabilities()
                    self.assertTrue(connector.file_read_supported)
                    await self.call('POST', '/v1/node/heartbeat', {'snapshot': {'sessions': [],
                        'mobile_capabilities': {'protocol_version': 1, 'operations': ['file_read']}}}, node=True)

                    async def read_range(offset, digest=None):
                        value = payload(path=str(document), offset=offset)
                        if digest is not None: value['sha256'] = digest
                        request = dict(request_id=value['request_id'], computer_id=self.node['node_id'],
                                       operation='file_read', session=SESSION, payload=value)
                        await self.call('POST', '/v1/requests', request, expected=202)
                        claims = (await self.call('GET', '/v1/node/requests', node=True))['requests']
                        self.assertEqual(len(claims), 1)
                        result = await connector.execute(claims[0])
                        await self.call('POST', '/v1/node/requests/' + value['request_id'] + '/result', result, node=True)
                        return await self.call('GET', '/v1/requests/' + value['request_id'])

                    received = bytearray()
                    digest = None
                    with patch.object(connector, '_native', wraps=connector._native) as native:
                        while len(received) < len(data):
                            receipt = await read_range(len(received), digest)
                            self.assertEqual(receipt['state'], 'completed', receipt)
                            result = receipt['result']
                            self.assertEqual(result['requested_path'], str(document))
                            self.assertEqual(result['bytes'], MAX_FILE_BYTES)
                            digest = result['sha256']
                            received.extend(base64.b64decode(result['data_base64'], validate=True))
                            document.write_bytes(b'changed after first snapshot')
                        self.assertEqual(native.await_count, 1)
                    self.assertEqual(received, data)
                    self.assertEqual(digest, hashlib.sha256(data).hexdigest())
                    connector.file_reads = file_reads.attachment_reads.AttachmentReads()
                    self.assertEqual((await read_range(MAX_READ_BYTES, digest))['state'], 'failed')
            finally:
                await connector.accounts.close()
                await connector.fleet.close()
                connector.journal.close()
