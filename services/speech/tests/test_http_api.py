"""Real HTTP/Unix-socket contracts in private homes; no GPU or native agents."""
import asyncio
from contextlib import suppress
import io
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
import wave

from aiohttp import ClientSession, FormData, web

SERVICE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SERVICE))
from zerus_speech.config import load
from zerus_speech.http_api import APIError, SocketBackend, create_app, normalize_audio, read_key
from zerus_speech.providers.openai_http import OpenAIHTTP
from zerus_speech.service import Server

KEY = "synthetic-key-for-isolated-speech-contracts"


def wav_bytes(frames=1600, rate=16000, channels=1):
    output = io.BytesIO()
    with wave.open(output, "wb") as wav:
        wav.setnchannels(channels)
        wav.setsampwidth(2)
        wav.setframerate(rate)
        wav.writeframes(b"\x20\x01" * frames * channels)
    return output.getvalue()


def form(model="russian", audio=None, **fields):
    data = FormData()
    data.add_field("file", io.BytesIO(wav_bytes() if audio is None else audio),
                   filename="../../ignored.wav", content_type="audio/wav")
    data.add_field("model", model)
    for name, value in fields.items():
        data.add_field(name, value)
    return data


class SpeechHTTP(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="zsh-")
        self.root = Path(self.temp.name)
        self.path = self.root / "speech.json"
        self.path.write_text(json.dumps({"schema": 1, "state_dir": str(self.root / "state"),
            "profiles": [
                {"id": "ru", "name": "Russian", "adapter": "fixture", "model": "fixture", "language": "ru", "text": "Русская речь."},
                {"id": "multi", "name": "Multilingual", "adapter": "fixture", "model": "fixture", "language": "en", "text": "English speech."}]}))
        self.server = Server(load(self.path), test_mode=True)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.backend = SocketBackend(self.root / "state/service.sock")
        self.runners = []
        self.url = await self.start_app()
        self.client = ClientSession(headers={"Authorization": "Bearer " + KEY})

    async def asyncTearDown(self):
        await self.client.close()
        for runner in self.runners:
            await runner.cleanup()
        await asyncio.to_thread(self.server.shutdown)
        await asyncio.to_thread(self.server.server_close)
        self.thread.join()
        self.temp.cleanup()

    async def start_app(self, **kwargs):
        app = create_app(self.backend, {"russian": "ru", "multilingual": "multi"}, KEY,
                         adapters={"fixture"}, **kwargs)
        runner = web.AppRunner(app, handler_cancellation=True, auto_decompress=False,
                               access_log=None, shutdown_timeout=2)
        await runner.setup()
        self.runners.append(runner)
        site = web.TCPSite(runner, "127.0.0.1", 0)
        await site.start()
        port = site._server.sockets[0].getsockname()[1]
        return f"http://127.0.0.1:{port}"

    async def post(self, **kwargs):
        async with self.client.post(self.url + "/v1/audio/transcriptions", data=form(**kwargs)) as response:
            return response.status, await response.json()

    async def test_real_model_selection_json_text_and_redacted_discovery(self):
        async with self.client.get(self.url + "/v1/models") as response:
            self.assertEqual(response.status, 200)
            listing = await response.json()
        self.assertEqual([v["id"] for v in listing["data"]], ["russian", "multilingual"])
        self.assertNotIn(str(self.root), json.dumps(listing))
        self.assertNotIn(KEY, json.dumps(listing))
        self.assertEqual(await self.post(language="ru", response_format="json"), (200, {"text": "Русская речь."}))
        self.assertEqual(await self.post(model="multilingual", language="en"), (200, {"text": "English speech."}))
        async with self.client.post(self.url + "/v1/audio/transcriptions", data=form(response_format="text")) as response:
            self.assertEqual(response.status, 200)
            self.assertEqual(await response.text(), "Русская речь.")
        with self.server.jobs.lock:
            self.assertEqual(self.server.jobs.stats()["totals"]["submitted"], 3)
        self.assertFalse(list((self.root / "state").glob("*.pcm")))

    async def test_existing_zerus_http_client_works_without_any_protocol_changes(self):
        profile = {"base_url": self.url + "/v1", "model": "russian", "language": "ru",
                   "api_key": KEY, "allow_insecure_http": True}
        result = await asyncio.to_thread(OpenAIHTTP().transcribe, profile, b"\x20\x01" * 1600, lambda: False)
        self.assertEqual(result, "Русская речь.")

    async def test_authentication_precedes_parsing_and_all_routes_require_key(self):
        for path in ["/v1/models", "/v1/audio/transcriptions", "/unknown"]:
            async with ClientSession() as client:
                async with client.post(self.url + path, data=b"malformed") as response:
                    self.assertEqual(response.status, 401)
                    self.assertEqual((await response.json())["error"]["code"], "invalid_api_key")
                    self.assertEqual(response.headers["WWW-Authenticate"], "Bearer")
        with self.server.jobs.lock:
            self.assertEqual(self.server.jobs.stats()["totals"]["submitted"], 0)

    async def test_unknown_models_and_unsupported_parameters_do_not_run_inference(self):
        for args, expected in [({"model": "unknown"}, 404), ({"language": "en"}, 400),
                               ({"language": "../ru"}, 400), ({"stream": "true"}, 400),
                               ({"response_format": "verbose_json"}, 400), ({"temperature": "1"}, 400),
                               ({"prompt": "do not ignore"}, 400), ({"audio": b""}, 400)]:
            with self.subTest(args=args):
                status, body = await self.post(**args)
                self.assertEqual(status, expected, body)
                self.assertEqual(set(body["error"]), {"message", "type", "param", "code"})
        duplicate = form()
        duplicate.add_field("model", "multilingual")
        async with self.client.post(self.url + "/v1/audio/transcriptions", data=duplicate) as response:
            self.assertEqual(response.status, 400)
        with self.server.jobs.lock:
            self.assertEqual(self.server.jobs.stats()["totals"]["submitted"], 0)

    async def test_upload_and_audio_duration_bounds(self):
        limited = await self.start_app(max_upload=1024)
        async with self.client.post(limited + "/v1/audio/transcriptions", data=form()) as response:
            self.assertEqual(response.status, 413)
        status, body = await self.post(audio=wav_bytes(16000 * 120 + 1))
        self.assertEqual(status, 400, body)
        self.assertEqual(body["error"]["code"], "audio_too_long")
        truncated = wav_bytes()[:-2]
        self.assertEqual((await self.post(audio=truncated))[0], 400)
        with self.server.jobs.lock:
            self.assertEqual(self.server.jobs.stats()["totals"]["submitted"], 0)

    async def test_decoder_is_bounded_and_does_not_follow_playlist_file_references(self):
        if not shutil.which("ffmpeg"):
            with patch("asyncio.create_subprocess_exec", side_effect=FileNotFoundError):
                self.assertEqual((await self.post(audio=b"not audio"))[0], 503)
            return
        pcm = await normalize_audio(wav_bytes(frames=800, rate=8000, channels=2))
        self.assertEqual(len(pcm), 3200)
        secret = self.root / "not-an-upload.wav"
        secret.write_bytes(wav_bytes())
        playlist = f"ffconcat version 1.0\nfile '{secret}'\n".encode()
        self.assertEqual((await self.post(audio=playlist))[0], 400)

    async def wait_job(self, state):
        end = time.monotonic() + 3
        while time.monotonic() < end:
            with self.server.jobs.lock:
                row = self.server.jobs.db.execute("SELECT state FROM jobs ORDER BY created DESC LIMIT 1").fetchone()
            if row and row[0] == state:
                return
            await asyncio.sleep(0.01)
        self.fail(f"Job did not reach {state}")

    def delay(self):
        config = json.loads(self.path.read_text())
        config["profiles"][0]["delay"] = 2
        self.path.write_text(json.dumps(config))
        self.server.jobs.config["profiles"][0]["delay"] = 2

    async def test_timeout_cancels_the_exact_job_and_keeps_control_requests_responsive(self):
        self.delay()
        url = await self.start_app(timeout=0.2)
        async with self.client.post(url + "/v1/audio/transcriptions", data=form()) as response:
            self.assertEqual(response.status, 504)
            self.assertEqual((await response.json())["error"]["code"], "request_timeout")
        await self.wait_job("cancelled")
        async with self.client.get(self.url + "/v1/models") as response:
            self.assertEqual(response.status, 200)

    async def test_disconnect_cancels_accepted_work_without_publishing_a_transcript(self):
        self.delay()
        request = asyncio.create_task(self.client.post(self.url + "/v1/audio/transcriptions", data=form()))
        await self.wait_job("running")
        request.cancel()
        with suppress(asyncio.CancelledError):
            await request
        await self.wait_job("cancelled")
        with self.server.jobs.lock:
            self.assertIsNone(self.server.jobs.db.execute("SELECT text FROM jobs").fetchone()[0])

    async def test_disconnect_during_submit_settles_acceptance_before_cancellation(self):
        self.delay()
        started = asyncio.Event()
        exchange = self.backend._exchange

        async def delayed(payload):
            if payload["action"] == "submit":
                started.set()
                await asyncio.sleep(0.15)
            return await exchange(payload)

        with patch.object(self.backend, "_exchange", delayed):
            request = asyncio.create_task(self.client.post(self.url + "/v1/audio/transcriptions", data=form()))
            await asyncio.wait_for(started.wait(), timeout=3)
            request.cancel()
            with suppress(asyncio.CancelledError):
                await request
            await self.wait_job("cancelled")
        with self.server.jobs.lock:
            totals = self.server.jobs.stats()["totals"]
            self.assertEqual((totals["submitted"], totals["cancelled"], totals["completed"]), (1, 1, 0))

    async def test_concurrency_and_backend_queue_admission_are_bounded(self):
        self.delay()
        url = await self.start_app(max_requests=1)
        request = asyncio.create_task(self.client.post(url + "/v1/audio/transcriptions", data=form()))
        await self.wait_job("running")
        try:
            async with self.client.get(url + "/v1/models") as response:
                self.assertEqual(response.status, 429)
            self.server.jobs.config["max_queue"] = 1
            status, body = await self.post()
            self.assertEqual(status, 429, body)
            self.assertEqual(body["error"]["code"], "server_busy")
        finally:
            request.cancel()
            with suppress(asyncio.CancelledError):
                await request
        await self.wait_job("cancelled")

    async def test_backend_failures_are_sanitized_and_remote_profiles_cannot_be_exposed(self):
        app = create_app(self.backend, {"russian": "ru"}, KEY)  # fixture adapter is not allowed
        runner = web.AppRunner(app, access_log=None)
        await runner.setup()
        self.runners.append(runner)
        site = web.TCPSite(runner, "127.0.0.1", 0)
        await site.start()
        url = f"http://127.0.0.1:{site._server.sockets[0].getsockname()[1]}"
        async with self.client.post(url + "/v1/audio/transcriptions", data=form()) as response:
            self.assertEqual(response.status, 503)
        os.chmod(self.root / "state/service.sock", 0o666)
        async with self.client.get(self.url + "/v1/models") as response:
            self.assertEqual(response.status, 503)
            body = await response.text()
            self.assertNotIn(str(self.root), body)
            self.assertNotIn(KEY, body)

    async def test_stalled_socket_does_not_hold_large_submit_past_ipc_deadline(self):
        path = self.root / "stalled.sock"
        tasks = set()

        async def blackhole(reader, writer):
            tasks.add(asyncio.current_task())
            try:
                await asyncio.Event().wait()  # Deliberately never drain input.
            finally:
                writer.transport.abort()

        server = await asyncio.start_unix_server(blackhole, path=str(path))
        os.chmod(path, 0o600)
        try:
            start = time.monotonic()
            with self.assertRaises(APIError) as raised:
                await asyncio.wait_for(SocketBackend(path, timeout=0.05).call(
                    {"action": "submit", "data": "x" * 4_000_000}), timeout=1)
            self.assertEqual(raised.exception.code, "service_unavailable")
            self.assertLess(time.monotonic() - start, 1)
        finally:
            server.close()
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            await asyncio.wait_for(server.wait_closed(), timeout=1)


class Standalone(unittest.TestCase):
    def test_key_permissions_and_standalone_cli_without_hgs_or_gui(self):
        with tempfile.TemporaryDirectory(prefix="zsh-cli-") as directory:
            root = Path(directory)
            key = root / "key"
            key.write_text(KEY + "\n")
            os.chmod(key, 0o600)
            self.assertEqual(read_key(key), KEY)
            os.chmod(key, 0o644)
            with self.assertRaises(ValueError):
                read_key(key)
            os.chmod(key, 0o600)
            path = root / "speech.json"
            path.write_text(json.dumps({"schema": 1, "state_dir": str(root / "state"),
                "profiles": [{"id": "ru", "name": "Fixture", "model": "fixture", "adapter": "fixture", "language": "ru"}]}))
            config = json.loads(path.read_text())
            config["profiles"][0]["language"] = ""
            path.write_text(json.dumps(config))
            self.assertEqual(load(path)["profiles"][0]["language"], "")
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", 0))
                port = probe.getsockname()[1]
            env = {k: v for k, v in os.environ.items() if not k.startswith("HGS_")}
            env.update(HOME=str(root), PATH=str(root),
                       PYTHONPATH=os.pathsep.join(filter(None, [str(SERVICE), os.environ.get("PYTHONPATH")])))
            process = subprocess.Popen([sys.executable, "-m", "zerus_speech.http_api", "--config", str(path),
                "--api-key-file", str(key), "--model", "russian=ru", "--port", str(port), "--test-mode"],
                env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                import urllib.request
                end = time.monotonic() + 5
                while True:
                    try:
                        request = urllib.request.Request(f"http://127.0.0.1:{port}/v1/models", headers={"Authorization": "Bearer " + KEY})
                        with urllib.request.urlopen(request, timeout=1) as response:
                            self.assertEqual(json.load(response)["data"][0]["id"], "russian")
                        break
                    except OSError:
                        if time.monotonic() >= end:
                            self.fail("Standalone service did not start")
                        time.sleep(0.02)
            finally:
                process.terminate()
                process.wait(timeout=15)
            self.assertEqual(process.returncode, 0)
            self.assertFalse((root / "state/service.sock").exists())


if __name__ == "__main__":
    unittest.main()
