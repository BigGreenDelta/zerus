"""The relay fixture seeds through the relay's own interfaces and removes its database."""
import os
import unittest

from aiohttp import ClientSession

from relay_fixture import DATABASE_URL, UNFINISHED, start_relay


class RelayFixtureTests(unittest.IsolatedAsyncioTestCase):
    async def test_seeded_credentials_authenticate_until_revoked(self):
        relay = await start_relay()
        self.addAsyncCleanup(relay.cleanup)
        phone = await relay.pair('Example phone', relay.invitation['pair_code'])
        other = await relay.add_node('Other computer')
        async with ClientSession(base_url=relay.url) as client:
            async def status(path, token):
                async with client.get(path, headers={'Authorization': 'Bearer ' + token}) as response:
                    return response.status

            self.assertEqual(await status('/v1/computers', phone['device_token']), 200)
            self.assertEqual(await status('/v1/node/requests', relay.node['node_token']), 200)
            self.assertEqual(await status('/v1/node/requests', other['node_token']), 200)
            await relay.revoke('device', phone['device_id'])
            await relay.revoke('node', other['node_id'])
            self.assertEqual(await status('/v1/computers', phone['device_token']), 401)
            self.assertEqual(await status('/v1/node/requests', other['node_token']), 401)
            self.assertEqual(await status('/v1/node/requests', relay.node['node_token']), 200)

    async def test_rust_relay_owns_one_private_database_until_cleanup(self):
        import asyncpg

        relay = await start_relay(rust_only=True)
        self.addAsyncCleanup(relay.cleanup)
        server = await asyncpg.connect(os.environ[DATABASE_URL])
        self.addAsyncCleanup(server.close)

        async def databases():
            return await server.fetchval('SELECT count(*) FROM pg_database WHERE datname=$1', relay.database)

        self.assertEqual(relay.config.stat().st_mode & 0o777, 0o600)
        self.assertEqual(await databases(), 1)
        # Until cleanup, the exit hook would drop it after an interruption.
        self.assertIn(relay, UNFINISHED)
        await relay.cleanup()
        self.assertIsNotNone(relay.process.returncode)
        self.assertEqual(await databases(), 0)
        self.assertNotIn(relay, UNFINISHED)


if __name__ == '__main__':
    unittest.main()
