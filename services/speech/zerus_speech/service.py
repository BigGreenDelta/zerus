import json
import os
from pathlib import Path
import socket
import socketserver
import stat
import threading

from .jobs import AdmissionError, Jobs, canonical_uuid, private_directory

MAX_REQUEST_BYTES = 5_130_000


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        self.request.settimeout(10)
        request = None
        try:
            raw = self.rfile.readline(MAX_REQUEST_BYTES + 1)
            if len(raw) > MAX_REQUEST_BYTES or not raw.endswith(b"\n"):
                raise ValueError("Speech request is too large or incomplete")
            request = json.loads(raw)
            result = self.server.jobs.handle(request)
        except AdmissionError as error:
            result = {"schema": 1, "state": "failed", "error_code": error.code, "error": str(error)}
        except (ValueError, TypeError, KeyError, RecursionError) as error:
            result = {"schema": 1, "state": "failed", "error_code": "invalid_request", "error": str(error)[:256]}
        except (OSError, sqlite_error):
            result = {"schema": 1, "state": "failed", "error_code": "service_failure", "error": "Speech service request failed"}
        if "job_id" not in result and isinstance(request, dict):
            try:
                result["job_id"] = canonical_uuid(request.get("request_id", request.get("job_id")))
            except (ValueError, TypeError):
                pass
        try:
            self.wfile.write(json.dumps(result, ensure_ascii=False, separators=(",", ":")).encode() + b"\n")
        except OSError:
            pass


# Database error text can contain local paths; never return it to remote clients.
from sqlite3 import Error as sqlite_error


class Server(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
    daemon_threads = True
    request_queue_size = 4

    def __init__(self, config, *, test_mode=False):
        self.jobs = Jobs(config, test_mode=test_mode)
        self.capacity = threading.BoundedSemaphore(4)
        self.path = config["socket_path"]
        try:
            private_directory(self.path.parent)
            if self.path.exists() or self.path.is_symlink():
                info = self.path.lstat()
                if not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.geteuid():
                    raise ValueError("Speech socket path is occupied by another file")
                with socket.socket(socket.AF_UNIX) as probe:
                    probe.settimeout(1)
                    try:
                        probe.connect(str(self.path))
                    except (ConnectionRefusedError, FileNotFoundError):
                        pass
                    else:
                        raise ValueError("Speech socket is already in use")
                self.path.unlink()
            super().__init__(str(self.path), Handler)
            os.chmod(self.path, 0o600)
        except BaseException:
            self.jobs.close()
            raise

    def process_request(self, request, address):
        if not self.capacity.acquire(blocking=False):
            request.close()
            return
        try:
            super().process_request(request, address)
        except BaseException:
            self.capacity.release()
            raise

    def process_request_thread(self, request, address):
        try:
            super().process_request_thread(request, address)
        finally:
            self.capacity.release()

    def server_close(self):
        super().server_close()
        self.jobs.close()
        self.path.unlink(missing_ok=True)
