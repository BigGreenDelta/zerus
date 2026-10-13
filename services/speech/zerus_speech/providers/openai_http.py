"""Bounded OpenAI-compatible transcription, with no redirects or retry policy."""
import http.client
import io
import ipaddress
import json
import queue
import socket
import ssl
import threading
import time
from urllib.parse import urlsplit
import uuid
import wave

from .base import Cancelled, ProviderError

TIMEOUT_SECONDS = 60
_DNS_CAPACITY = threading.BoundedSemaphore(1)


def resolve(host, port, deadline, cancelled):
    """One bounded daemon resolver; timed-out DNS cannot accumulate workers."""
    if not _DNS_CAPACITY.acquire(blocking=False):
        raise ProviderError("http_connection")
    output = queue.Queue(maxsize=1)
    def lookup():
        try:
            output.put(socket.getaddrinfo(host, port, type=socket.SOCK_STREAM)[:32])
        except OSError:
            output.put(None)
        finally:
            _DNS_CAPACITY.release()
    threading.Thread(target=lookup, daemon=True, name="speech-http-dns").start()
    limit = min(deadline, time.monotonic() + 5)
    while True:
        if cancelled():
            raise Cancelled()
        if time.monotonic() >= limit:
            raise ProviderError("http_timeout")
        try:
            result = output.get(timeout=min(0.05, max(0.001, limit - time.monotonic())))
        except queue.Empty:
            continue
        if not result:
            raise ProviderError("http_connection")
        return result


def private_address(address):
    ip = ipaddress.ip_address(address.split("%", 1)[0])
    return (ip.is_private or ip.is_loopback) and not ip.is_unspecified and not ip.is_multicast


class OpenAIHTTP:
    @staticmethod
    def endpoint(profile):
        url = profile.get("base_url", "")
        if (not isinstance(url, str) or not 1 <= len(url) <= 2048
                or any(ord(c) < 33 or ord(c) == 127 for c in url)
                or any(c in url for c in "@?#\\")):
            raise ValueError("Invalid HTTP speech base URL")
        parsed = urlsplit(url)
        if (not parsed.hostname or parsed.username or parsed.password or parsed.query or parsed.fragment
                or parsed.scheme not in {"http", "https"}
                or parsed.scheme == "http" and profile.get("allow_insecure_http") is not True):
            raise ValueError("HTTP speech requires HTTPS or explicit insecure transport opt-in")
        _ = parsed.port  # Validate ports before any network request.
        return parsed

    def available(self, profile):
        try:
            self.endpoint(profile)
            if not isinstance(profile.get("model"), str) or not 1 <= len(profile["model"]) <= 128:
                raise ValueError("Invalid HTTP speech model")
        except (ValueError, TypeError):
            return False, "The HTTP speech profile is incomplete or invalid"
        return True, ""

    def transcribe(self, profile, pcm, cancelled):
        endpoint = self.endpoint(profile)
        if cancelled():
            raise Cancelled()
        output = io.BytesIO()
        with wave.open(output, "wb") as wav:
            wav.setnchannels(1); wav.setsampwidth(2); wav.setframerate(16000); wav.writeframes(pcm)
        boundary = "zerus" + uuid.uuid4().hex
        body = bytearray()
        for name, value in (("model", profile["model"]), ("response_format", "json"), ("language", profile.get("language", ""))):
            if not value:
                continue
            body.extend((f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"\r\n\r\n{value}\r\n').encode())
        body.extend((f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="audio.wav"\r\nContent-Type: audio/wav\r\n\r\n').encode())
        body.extend(output.getvalue()); body.extend((f"\r\n--{boundary}--\r\n").encode())
        headers = {"Content-Type": "multipart/form-data; boundary=" + boundary, "Accept": "application/json"}
        if profile.get("api_key"):
            headers["Authorization"] = "Bearer " + profile["api_key"]
        deadline = time.monotonic() + TIMEOUT_SECONDS
        connection = (http.client.HTTPSConnection(endpoint.hostname, endpoint.port, timeout=5, context=ssl.create_default_context())
                      if endpoint.scheme == "https" else http.client.HTTPConnection(endpoint.hostname, endpoint.port, timeout=5))
        connection.auto_open = 0
        sockets, interrupted = [], []
        finished = threading.Event()
        response = None
        def watch():
            while not finished.wait(0.025):
                is_cancelled = cancelled()
                if is_cancelled or time.monotonic() >= deadline:
                    interrupted.append("cancelled" if is_cancelled else "timeout")
                    for owned in list(sockets):
                        try: owned.shutdown(socket.SHUT_RDWR)
                        except OSError: pass
                        try: owned.close()
                        except OSError: pass
                    return
        watcher = threading.Thread(target=watch, daemon=True, name="speech-http-deadline")
        watcher.start()
        try:
            # http.client does not use ambient proxies or follow redirects.
            addresses = resolve(endpoint.hostname, endpoint.port or (443 if endpoint.scheme == "https" else 80), deadline, cancelled)
            allowed = [item for item in addresses if endpoint.scheme == "https" or private_address(item[4][0])]
            if not allowed:
                raise ProviderError("http_address_blocked")
            # Dial the exact validated IP; HTTP Host and TLS still use the name.
            # No retry occurs after a POST, including on redirects or timeouts.
            connected = None
            for family, kind, protocol, _, address in allowed[:4]:
                if cancelled(): raise Cancelled()
                remaining = deadline - time.monotonic()
                if remaining <= 0: raise ProviderError("http_timeout")
                owned = socket.socket(family, kind, protocol); sockets.append(owned)
                owned.settimeout(min(5, remaining))
                try:
                    owned.connect(address); connected = owned; break
                except OSError:
                    owned.close()
            if connected is None:
                raise ProviderError("http_connection")
            if endpoint.scheme == "https":
                connected = ssl.create_default_context().wrap_socket(connected, server_hostname=endpoint.hostname, do_handshake_on_connect=False)
                sockets.append(connected)
                connected.settimeout(max(0.1, deadline - time.monotonic()))
                connected.do_handshake()
            connection.sock = connected
            if cancelled():
                raise Cancelled()
            connection.sock.settimeout(max(0.1, deadline - time.monotonic()))
            connection.request("POST", endpoint.path.rstrip("/") + "/audio/transcriptions", body=body, headers=headers)
            response = connection.getresponse()
            if response.status != 200:
                raise ProviderError("http_authentication" if response.status in {401, 403} else
                                    "http_rate_limited" if response.status == 429 else "http_status")
            content = bytearray()
            while True:
                if cancelled():
                    raise Cancelled()
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise ProviderError("http_timeout")
                if connection.sock:
                    connection.sock.settimeout(remaining)
                part = response.read1(min(8192, 65537 - len(content)))
                if cancelled():
                    raise Cancelled()
                if interrupted:
                    raise ProviderError("http_timeout")
                content.extend(part)
                if len(content) > 65536:
                    raise ProviderError("http_response_invalid")
                if not part:
                    break
            try:
                value = json.loads(content)
                text = value.get("text") if isinstance(value, dict) else None
                if not isinstance(text, str) or len(text.encode()) > 32000:
                    raise ValueError()
            except (ValueError, UnicodeError, RecursionError):
                raise ProviderError("http_response_invalid") from None
            if cancelled():
                raise Cancelled()
            return text
        except (socket.timeout, TimeoutError):
            if cancelled(): raise Cancelled() from None
            raise ProviderError("http_timeout") from None
        except (OSError, http.client.HTTPException):
            if cancelled(): raise Cancelled() from None
            if interrupted: raise ProviderError("http_timeout") from None
            raise ProviderError("http_connection") from None
        finally:
            finished.set()
            if response is not None:
                response.close()
            connection.close()
            for owned in sockets:
                owned.close()
            watcher.join(timeout=0.1)
