"""Optional OpenAI-compatible HTTP transport over the private speech queue.

The default CLI owns a standalone speech service; --socket attaches to an
existing one. Neither mode invokes hgs or requires the desktop application.
"""
import argparse
import asyncio
import base64
import contextlib
import hmac
import io
import json
import logging
import os
from pathlib import Path
import re
import stat
import threading
import uuid
import wave

from aiohttp import BodyPartReader, web

from .audio import MAX_AUDIO_BYTES

MAX_UPLOAD = 25 * 1024 * 1024
MAX_RESPONSE = 1024 * 1024
MAX_REQUEST = 5_130_000
MODEL_ID = re.compile(r"[A-Za-z0-9_.:-]{1,128}")


class APIError(Exception):
    def __init__(self, status, code, message, param=None):
        self.status, self.code, self.message, self.param = status, code, message, param


async def complete_exchange(coroutine):
    """Settle an owned, bounded IPC exchange before cancellation can overtake it.

    In particular, cancel(job) must not arrive before an interrupted submit(job).
    The task is always awaited, including on shutdown; it is never orphaned.
    """
    task = asyncio.create_task(coroutine)
    try:
        return await asyncio.shield(task)
    except asyncio.CancelledError:
        while not task.done():
            try:
                await asyncio.shield(task)
            except asyncio.CancelledError:
                continue
            except Exception:
                break
        with contextlib.suppress(Exception, asyncio.CancelledError):
            task.result()
        raise


class SocketBackend:
    def __init__(self, path, *, timeout=10):
        self.path = Path(path)
        self.timeout = timeout

    async def call(self, payload):
        return await complete_exchange(self._exchange(payload))

    async def _exchange(self, payload):
        writer = None
        try:
            info, parent = self.path.lstat(), self.path.parent.lstat()
            if (not self.path.is_absolute() or not stat.S_ISSOCK(info.st_mode)
                    or info.st_uid != os.geteuid() or info.st_mode & 0o077
                    or not stat.S_ISDIR(parent.st_mode) or parent.st_uid != os.geteuid()
                    or parent.st_mode & 0o077):
                raise ValueError()
            raw = json.dumps(payload, separators=(",", ":")).encode() + b"\n"
            if len(raw) > MAX_REQUEST:
                raise ValueError()
            async with asyncio.timeout(self.timeout):
                reader, writer = await asyncio.open_unix_connection(str(self.path), limit=MAX_RESPONSE + 1)
                writer.write(raw)
                await writer.drain()
                reply = await reader.readline()
                if len(reply) > MAX_RESPONSE or not reply.endswith(b"\n"):
                    raise ValueError()
                value = json.loads(reply)
                if not isinstance(value, dict) or value.get("schema") != 1:
                    raise ValueError()
                return value
        except (OSError, ValueError, TimeoutError, RecursionError):
            raise APIError(503, "service_unavailable", "The speech service is unavailable.") from None
        finally:
            if writer is not None:
                # A timed-out peer may never drain a large submit. Graceful
                # close/wait_closed would then wait outside the IPC deadline.
                writer.transport.abort()


async def normalize_audio(data):
    """Fast-path canonical WAV; decode other containers in a bounded FFmpeg child.

    Only pipe input is allowed, so playlists cannot fetch URLs or local files.
    No client filename becomes a filesystem path or subprocess argument.
    """
    try:
        with wave.open(io.BytesIO(data), "rb") as wav:
            if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getcomptype()) == (1, 2, 16000, "NONE"):
                if wav.getnframes() * 2 > MAX_AUDIO_BYTES:
                    raise APIError(400, "audio_too_long", "Audio must be at most 120 seconds.", "file")
                pcm = wav.readframes(wav.getnframes())
                if len(pcm) != wav.getnframes() * 2:
                    raise APIError(400, "invalid_audio", "Audio is truncated.", "file")
                if not pcm:
                    raise APIError(400, "invalid_audio", "Audio is empty.", "file")
                return pcm
    except (wave.Error, EOFError):
        pass
    process = None
    feeder = None
    try:
        process = await asyncio.create_subprocess_exec(
            "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
            "-protocol_whitelist", "pipe", "-i", "pipe:0", "-map", "0:a:0",
            "-vn", "-sn", "-dn", "-ac", "1", "-ar", "16000", "-t", "120.01",
            "-f", "s16le", "pipe:1", stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL)

        async def feed():
            try:
                process.stdin.write(data)
                await process.stdin.drain()
            except (BrokenPipeError, ConnectionResetError):
                pass
            finally:
                process.stdin.close()

        feeder = asyncio.create_task(feed())
        pcm = bytearray()
        async with asyncio.timeout(15):
            while part := await process.stdout.read(min(65536, MAX_AUDIO_BYTES + 1 - len(pcm))):
                pcm.extend(part)
                if len(pcm) > MAX_AUDIO_BYTES:
                    raise APIError(400, "audio_too_long", "Audio must be at most 120 seconds.", "file")
            await feeder
            if await process.wait() or not pcm or len(pcm) % 2:
                raise APIError(400, "invalid_audio", "Audio could not be decoded.", "file")
        return bytes(pcm)
    except FileNotFoundError:
        raise APIError(503, "decoder_unavailable", "Install FFmpeg on the speech server.") from None
    except TimeoutError:
        raise APIError(400, "invalid_audio", "Audio decoding exceeded its time limit.", "file") from None
    finally:
        if process is not None:
            if process.returncode is None:
                with contextlib.suppress(ProcessLookupError):
                    process.kill()
            await process.wait()
        if feeder is not None:
            feeder.cancel()
            with contextlib.suppress(asyncio.CancelledError, BrokenPipeError, ConnectionResetError):
                await feeder


async def read_form(request, max_upload):
    if request.content_type != "multipart/form-data" or request.headers.get("Content-Encoding", "identity") != "identity":
        raise APIError(400, "invalid_request", "Use an uncompressed multipart/form-data request.")
    if request.content_length is not None and request.content_length > max_upload + 65536:
        raise APIError(413, "request_too_large", "The upload exceeds the server limit.", "file")
    fields = {}
    try:
        reader = await request.multipart()
        while part := await reader.next():
            if not isinstance(part, BodyPartReader):
                raise ValueError()
            name = part.name
            if name not in {"file", "model", "language", "response_format", "stream", "temperature"} or name in fields:
                raise APIError(400, "unsupported_parameter", "Unknown or duplicate form parameter.")
            if part.headers.get("Content-Encoding") or part.headers.get("Content-Transfer-Encoding"):
                raise ValueError()
            limit = max_upload if name == "file" else 512
            data = bytearray()
            while chunk := await part.read_chunk(65536):
                data.extend(chunk)
                if len(data) > limit or request.content.total_bytes > max_upload + 65536:
                    raise APIError(413, "request_too_large", "The upload exceeds the server limit.", name)
            fields[name] = bytes(data) if name == "file" else data.decode("utf-8")
    except (ValueError, AssertionError, UnicodeError, LookupError):
        raise APIError(400, "invalid_request", "Malformed multipart request.") from None
    if not fields.get("file") or not fields.get("model"):
        raise APIError(400, "missing_parameter", "Both file and model are required.")
    if fields.get("response_format", "json") not in {"json", "text"}:
        raise APIError(400, "unsupported_parameter", "Supported response formats are json and text.", "response_format")
    if fields.get("stream", "false") != "false":
        raise APIError(400, "unsupported_parameter", "Streaming is not supported.", "stream")
    if fields.get("temperature", "0") not in {"0", "0.0"}:
        raise APIError(400, "unsupported_parameter", "Only temperature 0 is supported.", "temperature")
    if fields.get("language") and not re.fullmatch(r"[a-z]{2}", fields["language"]):
        raise APIError(400, "invalid_language", "Use a two-letter lowercase language code.", "language")
    return fields


def create_app(backend, models, api_key, *, max_upload=MAX_UPLOAD, max_requests=4,
               timeout=55, adapters=frozenset({"gigaam"})):
    if (not models or len(models) > 32 or any(not MODEL_ID.fullmatch(k) or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", v) for k, v in models.items())
            or not 32 <= len(api_key) <= 4096 or not api_key.isascii() or any(c.isspace() or ord(c) < 33 or ord(c) == 127 for c in api_key)
            or not 1 <= max_requests <= 16 or not 1 <= max_upload <= MAX_UPLOAD or not 0 < timeout <= 300):
        raise ValueError("Invalid HTTP speech configuration")
    active = 0

    @web.middleware
    async def boundary(request, handler):
        nonlocal active
        admitted = False
        try:
            authorization = request.headers.getall("Authorization", [])
            if len(authorization) != 1 or not hmac.compare_digest(authorization[0].encode(), ("Bearer " + api_key).encode()):
                raise APIError(401, "invalid_api_key", "A valid Bearer API key is required.")
            if active >= max_requests:
                raise APIError(429, "server_busy", "The speech server is busy.")
            active += 1
            admitted = True
            async with asyncio.timeout(timeout):
                return await handler(request)
        except TimeoutError:
            error = APIError(504, "request_timeout", "Speech recognition exceeded its time limit.")
        except APIError as exc:
            error = exc
        except web.HTTPException as exc:
            error = APIError(exc.status, "invalid_request", "Unknown API route or method.")
        except (OSError, ConnectionError):
            error = APIError(503, "service_unavailable", "The speech service is unavailable.")
        except Exception as exc:
            logging.warning("Speech HTTP request failed (%s)", type(exc).__name__)
            error = APIError(500, "internal_error", "The speech request failed.")
        finally:
            if admitted:
                active -= 1
        response = web.json_response({"error": {"message": error.message,
            "type": "invalid_request_error" if error.status < 500 else "server_error",
            "param": error.param, "code": error.code}}, status=error.status)
        response.force_close()  # Never drain an untrusted oversized/unread body.
        if error.status == 401:
            response.headers["WWW-Authenticate"] = "Bearer"
        if error.status == 429:
            response.headers["Retry-After"] = "1"
        return response

    async def profiles():
        reply = await backend.call({"action": "providers"})
        return {p["id"]: p for p in reply.get("profiles", []) if p.get("adapter") in adapters}

    async def list_models(request):
        known = await profiles()
        return web.json_response({"object": "list", "data": [
            {"id": model, "object": "model", "created": 0, "owned_by": "zerus-speech"}
            for model, profile in models.items() if known.get(profile, {}).get("available")]})

    async def transcribe(request):
        job_id = None
        completed = False
        try:
            fields = await read_form(request, max_upload)
            model = fields["model"]
            if model not in models:
                raise APIError(404, "model_not_found", "The requested transcription model does not exist.", "model")
            profile = (await profiles()).get(models[model])
            if not profile or not profile.get("available"):
                raise APIError(503, "model_unavailable", "The requested model is unavailable.", "model")
            language = fields.get("language", "")
            if language and (profile.get("language") and language != profile["language"]
                             or not profile.get("language") and language not in {"ru", "en", "kk", "ky", "uz"}):
                raise APIError(400, "unsupported_language", "The selected model does not support that language.", "language")
            pcm = await normalize_audio(fields.pop("file"))
            job_id = str(uuid.uuid4())
            submit = {"action": "submit", "request_id": job_id, "profile_id": models[model],
                "audio": {"format": "pcm_s16le", "sample_rate": 16000, "channels": 1,
                          "data_base64": base64.b64encode(pcm).decode()}}
            if "profile_revision" in profile:
                submit["profile_revision"] = profile["profile_revision"]
            result = await backend.call(submit)
            while result.get("state") in {"queued", "running"}:
                if result.get("job_id") != job_id:
                    raise APIError(503, "invalid_result", "The speech result identity is invalid.")
                await asyncio.sleep(0.05)
                result = await backend.call({"action": "status", "job_id": job_id})
            if result.get("state") == "completed" and result.get("job_id") != job_id:
                raise APIError(503, "invalid_result", "The speech result identity is invalid.")
            if result.get("state") != "completed" or not isinstance(result.get("text"), str):
                code = result.get("error_code", "inference_failed")
                if code == "queue_full":
                    raise APIError(429, "server_busy", "The speech queue is full.")
                if code == "profile_revision_conflict":
                    raise APIError(409, "model_configuration_changed", "The model configuration changed; review it.", "model")
                raise APIError(503, "inference_failed", "The speech service could not complete recognition.")
            text = result["text"]
            if len(text.encode()) > 32000:
                raise APIError(503, "invalid_result", "The speech result exceeds the server limit.")
            completed = True
            return (web.Response(text=text, content_type="text/plain") if fields.get("response_format") == "text"
                    else web.json_response({"text": text}))
        finally:
            if job_id and not completed:
                with contextlib.suppress(APIError):
                    await backend.call({"action": "cancel", "job_id": job_id})

    app = web.Application(middlewares=[boundary], client_max_size=max_upload + 65536)
    app.router.add_get("/v1/models", list_models)
    app.router.add_post("/v1/audio/transcriptions", transcribe)
    return app


def read_key(path):
    info = path.lstat()
    if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid()
            or info.st_mode & 0o077 or info.st_nlink != 1 or info.st_size > 4097):
        raise ValueError("API key file must be private, regular and owned by this user")
    return path.read_text().removesuffix("\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--config", type=Path, help="Standalone service configuration (default: speech.json)")
    mode.add_argument("--socket", type=Path, help="Attach to an existing private speech service")
    parser.add_argument("--model", action="append", required=True, metavar="MODEL=PROFILE", help="Explicit public model to local profile mapping; repeatable")
    parser.add_argument("--api-key-file", required=True, type=Path)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--test-mode", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    os.umask(0o077)
    logging.basicConfig(level=logging.INFO)
    server = None
    thread = None
    try:
        models = {}
        for mapping in args.model:
            model, profile = mapping.split("=", 1)
            if model in models:
                raise ValueError("Duplicate public model")
            models[model] = profile
        key = read_key(args.api_key_file)
        socket_path = args.socket
        if socket_path is None:
            from .config import default_config, load
            from .service import Server
            config = load(args.config or default_config())
            server = Server(config, test_mode=args.test_mode)
            socket_path = config["socket_path"]
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
        app = create_app(SocketBackend(socket_path), models, key,
                         adapters={"gigaam", "fixture"} if args.test_mode else {"gigaam"})
        web.run_app(app, host=args.host, port=args.port, access_log=None,
                    handler_cancellation=True, auto_decompress=False,
                    shutdown_timeout=15, print=None)
    except (OSError, ValueError):
        parser.exit(1, "Could not start speech HTTP service; check configuration, key permissions and listener.\n")
    finally:
        if server is not None:
            server.shutdown()
            server.server_close()
            if thread is not None:
                thread.join(timeout=5)


if __name__ == "__main__":
    main()
