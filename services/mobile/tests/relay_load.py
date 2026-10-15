"""Disposable loopback relay load probe; never accepts a live URL or credentials.

Run with PYTHONPATH=services/mobile python services/mobile/tests/relay_load.py.
The HTTP client streams fixture files, and memory samples cover only the child
relay, not the generator. Linux /proc high-water RSS catches peaks between polls.
"""
from __future__ import annotations

import argparse
import asyncio
import base64
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import uuid

import aiohttp


def write_fixture(path: Path, node_id: str, kind: str) -> str:
    """Build maximum protocol fixtures without holding their bytes in memory."""
    from zerus_mobile.attachments import MAX_FILE_BYTES, MAX_REQUEST_BYTES

    request_id = str(uuid.uuid4())
    prefix = dict(request_id=request_id, computer_id=node_id, operation="send",
                  session="synthetic-load", payload=dict(request_id=request_id,
                  expected_run_id="synthetic-run", expected_conversation_id="synthetic-conversation",
                  text="Synthetic load"))
    with path.open("wb") as stream:
        encoded = json.dumps(prefix, separators=(",", ":")).encode()
        stream.write(encoded[:-2] + b',"attachments":[')
        if kind in ("attachments", "attachments_unicode"):
            # Multiples of three allow independently encoded chunks to concatenate.
            chunk_bytes = 3 * 16384
            encoded_chunk = base64.b64encode(b"x" * chunk_bytes)
            for index in range(2):
                if index:
                    stream.write(b",")
                name = f"synthetic-{index}.txt" if kind != "attachments_unicode" else f"synthetic-{index}-\U0001f600.txt"
                stream.write(json.dumps(dict(name=name, mime="text/plain"),
                                        separators=(",", ":"), ensure_ascii=False).encode()[:-1])
                stream.write(b',"data_base64":"')
                full, rest = divmod(MAX_FILE_BYTES, chunk_bytes)
                for _ in range(full):
                    stream.write(encoded_chunk)
                stream.write(base64.b64encode(b"x" * rest))
                stream.write(b'"}')
            stream.write(b"]}}")
        elif kind in ("unicode", "unicode_escaped", "unicode_mixed", "unicode_all"):
            if kind == "unicode_mixed":
                # Each token is below the individual mixed-width cap; aggregate
                # projected width must stop materialization of all 27 strings.
                for index in range(27):
                    if index: stream.write(b",")
                    stream.write(b'"' + b"x" * 1000000 + "\U0001f600".encode() + b'"')
                stream.write(b"]}}")
            else:
                stream.write(b'{"name":"synthetic.txt","mime":"text/plain","data_base64":"')
                chunk = b"x" * 65536 if kind != "unicode_all" else "\u0800".encode() * 21845
                for _ in range(420): stream.write(chunk)
                stream.write(b"\\ud83d\\ude00" if kind == "unicode_escaped" else "\U0001f600".encode())
                stream.write(b'"}]}}')
        elif kind == "dense":
            # The wire fits the protocol limit, but generic json.loads would build
            # millions of Python objects before the attachment validator sees it.
            chunk = b"0," * 32768
            remaining = MAX_REQUEST_BYTES - stream.tell() - 4
            while remaining >= len(chunk):
                stream.write(chunk)
                remaining -= len(chunk)
            stream.write(b"0," * (remaining // 2))
            stream.write(b"0]}}")
        elif kind == "malformed":
            stream.write(b"[" * 20000 + b"0" + b"]" * 19999 + b"]}}")
        else:
            raise ValueError("unknown fixture kind")
    return request_id


def memory_kib(pid: int) -> tuple[int, int]:
    fields = dict(line.split(":", 1) for line in Path(f"/proc/{pid}/status").read_text().splitlines()
                  if ":" in line)
    return tuple(int(fields[key].split()[0]) for key in ("VmRSS", "VmHWM"))


async def run_probe(kind="attachments", concurrency=2, memory_limit_mib=256,
                    config=None, repeats=1) -> dict:
    """Spawn a private SQLite relay, stream bounded requests and sample its RSS."""
    from zerus_mobile.store import Store

    if not 1 <= concurrency <= 8 or not 1 <= repeats <= 20:
        raise ValueError("concurrency must be 1..8 and repeats 1..20")
    if not Path("/proc/self/status").exists():
        raise RuntimeError("RSS validation requires Linux /proc")
    with tempfile.TemporaryDirectory(prefix="zerus-relay-load-") as directory:
        root = Path(directory)
        database = root / "relay.sqlite3"
        store = Store(database)
        try:
            workspace = store.workspace("Synthetic load")
            node = store.node(workspace, "Synthetic computer")
            phone = store.pair(store.invite(workspace)["pair_code"], "Synthetic phone")
        finally:
            store.close()
        fixtures = []
        for index in range(concurrency):
            path = root / f"payload-{index}.json"
            request_id = write_fixture(path, node["node_id"], kind)
            fixtures.append((path, request_id))
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
        listener.listen(128)
        command = [sys.executable, str(Path(__file__).resolve()), "--worker",
                   "--store", str(database), "--socket-fd", str(listener.fileno()),
                   "--config", json.dumps(config or {})]
        with (root / "relay.log").open("wb") as log:
            process = subprocess.Popen(command, pass_fds=(listener.fileno(),),
                                       stdout=log, stderr=log)
            listener.close()
            try:
                origin = f"http://127.0.0.1:{port}"
                timeout = aiohttp.ClientTimeout(total=90)
                connector = aiohttp.TCPConnector(limit=concurrency + 4)
                async with aiohttp.ClientSession(timeout=timeout, connector=connector) as client:
                    for _ in range(200):
                        if process.poll() is not None:
                            raise RuntimeError("disposable relay exited: " + (root / "relay.log").read_text()[-2000:])
                        try:
                            async with client.get(origin + "/healthz") as response:
                                if response.status == 200:
                                    await response.read()
                                    break
                        except aiohttp.ClientError:
                            pass
                        await asyncio.sleep(0.025)
                    else:
                        raise RuntimeError("disposable relay failed readiness")
                    baseline = memory_kib(process.pid)[0]
                    samples, health_samples = [], []
                    finished = asyncio.Event()

                    async def monitor():
                        while not finished.is_set():
                            samples.append(memory_kib(process.pid))
                            start = time.perf_counter()
                            async with client.get(origin + "/healthz") as response:
                                await response.read()
                                health_samples.append((response.status, time.perf_counter() - start))
                            await asyncio.sleep(0.01)

                    async def upload(path, request_id):
                        async def chunks():
                            with path.open("rb") as stream:
                                while piece := stream.read(65536):
                                    yield piece
                                    await asyncio.sleep(0)
                        start = time.perf_counter()
                        headers = {"Authorization": "Bearer " + phone["device_token"],
                                   "Content-Type": "application/json", "Content-Length": str(path.stat().st_size)}
                        async with client.post(origin + "/v1/requests", data=chunks(), headers=headers) as response:
                            body = await response.json()
                            if response.status >= 400 and not isinstance(body.get("error"), str):
                                raise AssertionError(f"uncontrolled response: {response.status}")
                            return dict(status=response.status, elapsed_seconds=time.perf_counter() - start,
                                        request_id=request_id)

                    observer = asyncio.create_task(monitor())
                    egress = []
                    try:
                        responses = []
                        for repetition in range(repeats):
                            if repetition:
                                # Reuse bounded fixture files with fresh UUIDs;
                                # retain one worker and claim only after every
                                # upload to expose warm allocator/input leaks.
                                fixtures = [(path, write_fixture(path, node["node_id"], kind)) for path, _ in fixtures]
                            responses.extend(await asyncio.gather(*(upload(*fixture) for fixture in fixtures)))
                        if kind in ("attachments", "attachments_unicode"):
                            admitted = {response["request_id"] for response in responses if response["status"] == 202}
                            node_headers = {"Authorization": "Bearer " + node["node_token"]}
                            for _ in range(len(admitted)):
                                start = time.perf_counter()
                                async with client.get(origin + "/v1/node/requests", headers=node_headers) as response:
                                    value = await response.json()
                                    if response.status != 200 or len(value.get("requests", [])) != 1:
                                        raise AssertionError("admitted upload could not be claimed")
                                    command = value["requests"][0]
                                    if command["request_id"] not in admitted or len(command["payload"]["attachments"]) != 2:
                                        raise AssertionError("claim changed attachment identity")
                                    admitted.remove(command["request_id"])
                                    request_id = command["request_id"]
                                    egress.append(dict(status=response.status, elapsed_seconds=time.perf_counter() - start))
                                del value, command
                                async with client.post(origin + f"/v1/node/requests/{request_id}/result", headers=node_headers,
                                     json=dict(state="completed", result={"status": "submitted"}, error=None)) as response:
                                    await response.read()
                                    if response.status != 200:
                                        raise AssertionError("delivery acknowledgement was rejected")
                                async with client.get(origin + f"/v1/requests/{request_id}",
                                     headers={"Authorization": "Bearer " + phone["device_token"]}) as response:
                                    receipt = await response.json()
                                    if response.status != 200 or receipt["state"] != "completed":
                                        raise AssertionError("delivery receipt did not survive the maximum upload")
                            async with client.get(origin + "/v1/node/requests", headers=node_headers) as response:
                                if (await response.json()).get("requests") != []:
                                    raise AssertionError("completed uploads were replayed")
                    finally:
                        finished.set()
                        await observer
                    samples.append(memory_kib(process.pid))
                    peak_mib = max(sample[1] for sample in samples) / 1024
                    statuses = [response["status"] for response in responses]
                    allowed = {202, 429, 503} if kind in ("attachments", "attachments_unicode") else {400, 413, 429, 503}
                    result = dict(kind=kind, concurrency=concurrency, repeats=repeats,
                                  body_bytes=fixtures[0][0].stat().st_size,
                                  baseline_rss_mib=baseline / 1024, peak_rss_mib=peak_mib,
                                  memory_limit_mib=memory_limit_mib, responses=responses, claims=egress,
                                  health_max_seconds=max((sample[1] for sample in health_samples), default=0),
                                  healthy=bool(health_samples) and all(sample[0] == 200 for sample in health_samples))
                    result["passed"] = (peak_mib <= memory_limit_mib and result["healthy"]
                                        and all(status in allowed for status in statuses)
                                        and (kind not in ("attachments", "attachments_unicode") or 202 in statuses))
                    return result
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)


def worker(args):
    from aiohttp import web
    from zerus_mobile.server import Config, create_app
    from zerus_mobile.store import Store

    store = Store(Path(args.store))
    app = create_app(store, Config(background=False, **json.loads(args.config)))
    try:
        web.run_app(app, sock=socket.socket(fileno=args.socket_fd), print=None, access_log=None,
                    handler_cancellation=True)
    finally:
        store.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kind", choices=("attachments", "attachments_unicode", "unicode", "unicode_escaped", "unicode_mixed", "unicode_all", "dense", "malformed", "all"), default="all")
    parser.add_argument("--concurrency", type=int, default=2)
    parser.add_argument("--repeats", type=int, default=1, help="Reuse worker; submit fresh UUIDs each round before claiming")
    parser.add_argument("--memory-limit-mib", type=float, default=256)
    parser.add_argument("--config", default="{}", help="JSON Config overrides for the disposable relay")
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--store", help=argparse.SUPPRESS)
    parser.add_argument("--socket-fd", type=int, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.worker:
        worker(args)
        return
    async def probes():
        kinds = ("attachments", "dense", "malformed") if args.kind == "all" else (args.kind,)
        return [await run_probe(kind, args.concurrency, args.memory_limit_mib,
                                config=json.loads(args.config), repeats=args.repeats) for kind in kinds]
    results = asyncio.run(probes())
    print(json.dumps(dict(python=sys.version.split()[0], probes=results), indent=2))
    raise SystemExit(0 if all(result["passed"] for result in results) else 1)


if __name__ == "__main__":
    main()
