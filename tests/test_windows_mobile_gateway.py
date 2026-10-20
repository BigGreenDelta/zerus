"""Isolated Windows gateway: real connector, hgs/ConPTY and throwaway HTTPS.

No phone, owner profile, installed application or external relay is used.
Requires HGS_TEST_CONNECTOR, HGS_TEST_BIN, HGS_TEST_FAKE_AGENT and openssl.
Run outside a job that forbids breakaway, like the other Windows host tests.
The relay here is a protocol fixture; production relay contracts run separately.
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import ctypes
import json
import os
from pathlib import Path
import shutil
import ssl
import subprocess
import threading
import time
import unittest
import uuid

from test_windows_host import FAKE_AGENT, HGS, HIDDEN, LIMIT
from test_windows_lifecycle import LifecycleCase, read_json, read_events
from windows_gateway_custody import ConnectorCustody, guard_root_cleanup, termination_adapter

CONNECTOR = Path(os.environ.get("HGS_TEST_CONNECTOR", Path(__file__).resolve().parents[1] /
                               "services/mobile/connector/target/release/zerus-mobile-connector.exe"))
TLS = """[req]
distinguished_name=name
x509_extensions=ca
prompt=no
[name]
CN=synthetic-gateway-ca
[ca]
basicConstraints=critical,CA:TRUE
keyUsage=critical,keyCertSign,cRLSign
[leaf]
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=serverAuth
subjectAltName=IP:127.0.0.1
"""


class Gateway(LifecycleCase):
    def setUp(self):
        super().setUp()
        self.assertTrue(CONNECTOR.is_file(), "build the Windows Rust connector first")
        self.assertTrue(shutil.which("openssl"), "openssl is required for the throwaway TLS fixture")
        self.bundle = self.root / "gateway-bundle"
        self.bundle.mkdir()
        self.connector = self.bundle / "zerus-mobile-connector.exe"
        shutil.copy2(CONNECTOR, self.connector)
        shutil.copy2(HGS, self.bundle / "hgs.exe")
        # Keep the configuration root LifecycleCase enabled for local sessions.
        self.config = self.home / ".config" / "hgs" / "mobile" / "connector.json"
        self.pending, self.results, self.snapshots = [], {}, []
        self.mutex = threading.Lock()
        self.custody = None
        # Replace only this case's registered TemporaryDirectory cleanup. Its
        # GC finalizer must not bypass custody after an uncertain detached start.
        guard_root_cleanup(self, self.temp, lambda: self.custody is None or self.custody.may_remove_root)
        tls = self.root / "tls"
        tls.mkdir()
        config = tls / "openssl.cnf"
        config.write_text(TLS)
        def openssl(*args):
            subprocess.run(["openssl", *map(str, args)], capture_output=True, check=True,
                           timeout=20, creationflags=HIDDEN)
        openssl("req", "-x509", "-config", config, "-newkey", "ec", "-pkeyopt",
                "ec_paramgen_curve:prime256v1", "-nodes", "-keyout", tls / "ca.key",
                "-out", tls / "ca.pem", "-days", "2")
        openssl("req", "-new", "-config", config, "-newkey", "ec", "-pkeyopt",
                "ec_paramgen_curve:prime256v1", "-nodes", "-keyout", tls / "leaf.key",
                "-out", tls / "leaf.csr", "-subj", "/CN=127.0.0.1")
        openssl("x509", "-req", "-in", tls / "leaf.csr", "-CA", tls / "ca.pem", "-CAkey", tls / "ca.key",
                "-CAcreateserial", "-out", tls / "leaf.pem", "-days", "2", "-extfile", config, "-extensions", "leaf")
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(tls / "leaf.pem", tls / "leaf.key")
        self.env.update(SSL_CERT_FILE=str(tls / "ca.pem"), CURL_CA_BUNDLE=str(tls / "ca.pem"))
        fixture = self
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass
            def answer(self, body):
                data = json.dumps(body).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
            def do_GET(self):
                fixture.assertEqual(self.headers.get("Authorization"), "Bearer synthetic-windows-node-token")
                if self.path.startswith("/v1/node/requests?"):
                    with fixture.mutex:
                        batch, fixture.pending = fixture.pending, []
                    self.answer({"requests": batch})
                elif self.path == "/v1/node/devices":
                    self.answer({"workspace_id": "synthetic-workspace", "workspace_name": "Synthetic workspace", "devices": []})
                else:
                    self.send_error(404)
            def do_POST(self):
                fixture.assertEqual(self.headers.get("Authorization"), "Bearer synthetic-windows-node-token")
                body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", "0"))))
                with fixture.mutex:
                    if self.path == "/v1/node/heartbeat":
                        fixture.snapshots.append(body["snapshot"])
                    elif self.path.endswith("/result"):
                        fixture.results.setdefault(self.path.split("/")[-2], []).append(body)
                if self.path == "/v1/node/invitations":
                    self.answer({"workspace_id": "synthetic-workspace", "pair_code": "synthetic-one-use-invitation", "expires_at": time.time()+60})
                else:
                    self.answer({"ok": True})
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.addCleanup(self.close_server)
        self.addCleanup(self.close_connector)
        self.relay = f"https://127.0.0.1:{self.server.server_port}"
        self.manage("save", {"revision": "", "server_url": self.relay, "node_token": "synthetic-windows-node-token"})

    def close_server(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(5)

    def manage(self, action, payload=None):
        result = subprocess.run([str(self.connector), "--manage", action, "--config", str(self.config)],
                                input=json.dumps(payload or {}), capture_output=True, text=True, env=self.env,
                                timeout=15, creationflags=HIDDEN)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        return json.loads(result.stdout)

    def pin_connector(self, control):
        """Pin only the private control's PID executing this unique copied image.
        Custody rereads the same revision/generation/control after opening it.
        """
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.QueryFullProcessImageNameW.argtypes = (wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                                     ctypes.POINTER(wintypes.DWORD))
        kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
        handle = kernel.OpenProcess(0x00100000 | 0x1000 | 0x0001, False, control["pid"])
        if not handle:
            raise RuntimeError("private connector handle could not be opened")
        buffer, size = ctypes.create_unicode_buffer(32768), wintypes.DWORD(32768)
        valid = kernel.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size))
        if not valid or os.path.normcase(os.path.realpath(buffer.value)) != os.path.normcase(os.path.realpath(self.connector)):
            kernel.CloseHandle(handle)
            raise RuntimeError("connector process image did not match the owned fixture")
        return handle

    def start_connector(self, state):
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
        kernel.WaitForSingleObject.restype = wintypes.DWORD
        kernel.TerminateProcess.argtypes = (wintypes.HANDLE, wintypes.UINT)
        kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
        control_path = Path(state["state_dir"]) / "connector-control.json"
        def read_control():
            # Missing is unknown once launch was attempted; malformed data also
            # raises and preserves custody rather than authorizing deletion.
            if not control_path.exists():
                return None
            return json.loads(control_path.read_text())
        def stop(control):
            self.manage("stop", {"revision": control["revision"], "generation": control["generation"]})
        self.custody = ConnectorCustody(state["revision"], read_control, self.pin_connector,
            kernel.WaitForSingleObject, termination_adapter(kernel), kernel.CloseHandle, stop)
        self.custody.begin()  # Before Start, so failure/timeout cannot lose custody.
        running = self.manage("start", {"revision": state["revision"]})
        self.custody.recover()
        self.assertEqual(running["generation"], self.custody.control["generation"])
        return running

    def close_connector(self):
        if self.custody is None:
            return
        try:
            forced = self.custody.cleanup()
        except Exception:
            # This contains synthetic identity only, never a credential. Keep
            # the handle and root on unknown final exit for manual diagnosis.
            (self.root / "connector-custody-unknown.json").write_text(json.dumps({
                "revision": self.custody.revision, "control": self.custody.control,
                "image": str(self.connector), "exit_confirmed": self.custody.exited}))
            raise
        self.assertFalse(forced, "graceful connector stop failed; owned handle termination was required")

    def submit(self, operation, session, payload, request=None):
        request = request or {"request_id": str(uuid.uuid4()), "operation": operation,
                              "session": session, "payload": payload, "expires_at": time.time()+5}
        with self.mutex:
            before = len(self.results.get(request["request_id"], []))
            self.pending.append(request)
        def receipt():
            with self.mutex:
                rows = self.results.get(request["request_id"], [])
                return rows[-1] if len(rows) > before else None
        return request, self.wait_for("the gateway receipt", receipt)

    def test_direct_gateway_control_duplicate_receipts_and_native_host_survival(self):
        home = self.root / "codex-home"
        home.mkdir()
        shutil.copy2(FAKE_AGENT, self.bin / "codex.exe")
        ready = self.root / "codex.ready"
        record = self.root / "codex.jsonl"
        env = dict(self.env, CODEX_HOME=str(home), FAKE_AGENT_PROBE_TEXT="codex-cli 0.162.0",
                   FAKE_AGENT_READY=str(ready), FAKE_AGENT_RECORD=str(record), FAKE_AGENT_PROMPT="codex> ",
                   FAKE_AGENT_TRANSCRIPT_DIR=str(self.root / "transcripts"))
        self.hgs("codex", str(self.project), "--new", "-n", "gateway", "-d", env=env)
        self.wait_for("the synthetic Codex session", lambda: read_json(ready))
        name = "codex/project/gateway"
        detail = self.wait_for("native identity", lambda: (lambda v: v if v.get("conversation_id") else None)(
            json.loads(self.hgs("inspect", name).stdout)))
        host = self.status()
        state = self.manage("status")
        running = self.start_connector(state)
        self.assertTrue(running["running"])
        again = self.manage("start", {"revision": running["revision"]})
        self.assertEqual(again["generation"], running["generation"])
        self.wait_for("the direct Windows heartbeat", lambda: any(
            any(row["name"] == name for row in snapshot["sessions"]) for snapshot in self.snapshots))
        caps = self.snapshots[-1]["mobile_capabilities"]
        self.assertIn("terminal_snapshot", caps["operations"])
        for operation in ("voice_submit", "speech_submit", "worktree_create", "file_read", "launch"):
            self.assertNotIn(operation, caps["operations"])
        identity = {"expected_run_id": detail["run_id"], "expected_conversation_id": detail["conversation_id"]}
        request_id = str(uuid.uuid4())
        request, receipt = self.submit("terminal_snapshot", name, {**identity, "request_id": request_id},
                                      {"request_id": request_id, "operation": "terminal_snapshot", "session": name,
                                       "payload": {**identity, "request_id": request_id}})
        self.assertEqual(receipt["state"], "completed", receipt)
        snapshot = receipt["result"]
        self.assertEqual(snapshot["status"], "snapshot")
        self.assertTrue(snapshot["input_supported"])
        input_id = str(uuid.uuid4())
        command = {"request_id": input_id, "operation": "terminal_input", "session": name,
                   "payload": {**identity, "request_id": input_id,
                               "terminal_binding_id": snapshot["terminal_binding_id"],
                               "text": "synthetic gateway input", "enter": False}, "expires_at": time.time()+5}
        _, submitted = self.submit("terminal_input", name, {}, command)
        self.assertEqual(submitted["state"], "completed", submitted)
        _, replayed = self.submit("terminal_input", name, {}, command)
        self.assertEqual(replayed, submitted)
        def received_text():
            return "".join(row.get("text", "") for row in read_events(record) if row.get("kind") in ("text", "paste"))
        self.wait_for("the literal terminal input", lambda: "synthetic gateway input" in received_text())
        self.assertEqual(received_text().count("synthetic gateway input"), 1)
        phones = self.hgs("mobile", "phones", "--json")
        self.assertTrue(json.loads(phones.stdout)["configured"])
        invitation = self.hgs("mobile", "invite", "--workspace", "synthetic-workspace", "--server", self.relay, "--json")
        self.assertTrue(json.loads(invitation.stdout)["pair_url"].startswith("zerus://pair?"))
        # The management process exited after Start; the independent connector
        # and session host both remain. Stop must leave the same host/session.
        self.manage("stop", {"revision": running["revision"], "generation": running["generation"]})
        self.wait_for("graceful connector stop", lambda: not self.manage("status")["running"])
        after_host = self.status()
        self.assertEqual(after_host["host"], host["host"])
        current = json.loads(self.hgs("inspect", name).stdout)
        self.assertEqual((current["run_id"], current["conversation_id"]),
                         (detail["run_id"], detail["conversation_id"]))
        self.assertTrue(self.pane(name))


if __name__ == "__main__":
    unittest.main()
