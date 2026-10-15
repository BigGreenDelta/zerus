"""Black-box startup: invalid command lines and configurations are refused."""
import unittest

from connector_harness import BlackBoxCase, STARTUP_FAILURE


class StartupRefusalTests(BlackBoxCase):
    async def refused(self, process, code=1):
        try:
            self.assertEqual(await process.wait_exit(10), code, process.output())
        finally:
            process.stop()
        return process.output()

    def assert_untouched(self, state_dir=None, exchanges=0, events=0):
        self.assertFalse((state_dir or self.state_dir).exists(), "a refused start created the journal directory")
        self.assertEqual(self.relay.exchanges[exchanges:], [], "a refused start reached the relay")
        self.assertEqual(self.hgs.events()[events:], [], "a refused start ran native hgs")

    async def test_invalid_configuration_is_refused_before_any_state_or_network(self):
        loopback = self.relay.url
        cases = {
            "not JSON": {"text": "{"},
            "not an object": {"text": "[]"},
            "plain HTTP off loopback": {"config": self.config(server_url="http://relay.example.invalid")},
            "credentials in the relay URL": {"config": self.config(server_url="https://user:secret@relay.example.invalid")},
            "query in the relay URL": {"config": self.config(server_url="https://relay.example.invalid/?token=x")},
            "fragment in the relay URL": {"config": self.config(server_url="https://relay.example.invalid/#x")},
            "control character in the relay URL": {"config": self.config(server_url="https://relay.example.invalid\n")},
            "non-HTTP scheme": {"config": self.config(server_url="ftp://relay.example.invalid")},
            "invalid port": {"config": self.config(server_url="https://relay.example.invalid:bad")},
            "missing relay URL": {"config": self.config(server_url=None)},
            "unrelated identity_url": {"config": self.config(identity_url=loopback + "/other")},
            "missing node token": {"config": self.config(node_token=None)},
            "empty node token": {"config": self.config(node_token="")},
            "node token with a space": {"config": self.config(node_token="bb token")},
            "node token that is not text": {"config": self.config(node_token=42)},
            "empty hgs_path": {"config": self.config(hgs_path="")},
            "hgs_path with NUL": {"config": self.config(hgs_path="hgs\0")},
            "poll interval below one second": {"config": self.config(poll_interval=0.5)},
            "poll interval that is not a number": {"config": self.config(poll_interval="5")},
            "state_dir that is not text": {"config": self.config(state_dir=7)},
        }
        for index, (name, case) in enumerate(cases.items()):
            with self.subTest(name):
                # A state directory of its own keeps one case from hiding another.
                state_dir = self.scratch / f"state-{index}"
                config = case.get("config")
                exchanges, events = len(self.relay.exchanges), len(self.hgs.events())
                if isinstance(config, dict) and config.get("state_dir") == str(self.state_dir):
                    config = {**config, "state_dir": str(state_dir)}
                output = await self.refused(await self.launch(config, text=case.get("text"), ready=False))
                self.assertIn(STARTUP_FAILURE, output)
                self.assertNotIn("Traceback", output)
                self.assert_untouched(state_dir, exchanges, events)

    async def test_loopback_http_requires_its_explicit_development_flag(self):
        output = await self.refused(await self.launch(args=(), ready=False))
        self.assertIn(STARTUP_FAILURE, output)
        self.assert_untouched()

    async def test_missing_configuration_file_is_refused(self):
        output = await self.refused(self.spawn(["--config", str(self.scratch / "absent.json"),
                                                "--allow-insecure-localhost"]))
        self.assertIn(STARTUP_FAILURE, output)
        self.assert_untouched()

    async def test_command_line_without_configuration_is_a_usage_error(self):
        await self.refused(self.spawn(["--allow-insecure-localhost"]), code=2)
        self.assert_untouched()


if __name__ == "__main__":
    unittest.main()
