#!/usr/bin/env python3
"""Qualify public E2EE transport in an owned 256 MiB Docker cgroup.

Accepts an image, never a live URL/database/credential. All device keys, maximum
ciphertexts and client buffers are generated outside the relay cgroup. Its
PostgreSQL and disk-backed data/spool volumes are disposable and always removed.
"""
import argparse
import asyncio
from collections import Counter
import json
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid

import aiohttp

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests"))
from relay_load import memory_kib


def docker(*args):
    return subprocess.check_output(["docker", *args], text=True).strip()


async def qualify(image, repeats):
    name = "zerus-public-load-" + uuid.uuid4().hex[:12]
    print("owned qualification fixture: "+name,file=sys.stderr)
    network, database, data, spool, pgdata = (name + suffix for suffix in ("-net", "-pg", "-data", "-spool", "-pgdata"))
    started = False
    try:
        for volume in (data, spool, pgdata):
            docker("volume", "create", "--label", "zerus.public-qualification="+name, volume)
        docker("network", "create", "--label", "zerus.public-qualification="+name, network)
        docker("run", "--detach", "--name", database, "--network", network,
               "--label", "zerus.public-qualification="+name,
               "--network-alias", "database", "--memory", "256m", "--memory-swap", "256m",
               "-v", pgdata+":/var/lib/postgresql/data", "-e", "POSTGRES_HOST_AUTH_METHOD=trust",
               "postgres:17.11-alpine@sha256:b0f9560a2de083e2cc7382e75f808c7381a32852a7ec49117deedb300e552b24")
        for _ in range(200):
            result = subprocess.run(["docker", "exec", database, "pg_isready", "-U", "postgres"],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if result.returncode == 0:
                break
            await asyncio.sleep(.05)
        else:
            raise AssertionError("disposable PostgreSQL did not start")
        config = {"database_url": "postgresql://postgres@database/postgres", "public_enrollment": True,
                  "large_payload_slots": 2, "pool_min": 2, "pool_max": 10,
                  "max_spool_bytes":164*2**20,
                  "queue_ttl": 900, "claim_ttl": 900, "team_reserved_percent": 25}
        subprocess.run(["docker", "run", "--rm", "--network", "none", "-i", "-v", data + ":/data",
                        "--entrypoint", "/bin/sh", image, "-c", "umask 077; cat > /data/config.json"],
                       input=json.dumps(config), text=True, check=True)
        docker("run", "--detach", "--name", name, "--memory", "256m", "--memory-swap", "256m",
               "--label", "zerus.public-qualification="+name,
               "--pids-limit", "64", "--read-only", "--tmpfs", "/tmp:size=4m",
               "--cap-drop", "ALL", "--security-opt", "no-new-privileges", "--network", network,
               "-v", data + ":/data", "-v", spool + ":/spool", "-p", "127.0.0.1::8787", image,
               "--config", "/data/config.json", "serve", "--host", "0.0.0.0")
        started = True
        port = docker("port", name, "8787").rsplit(":", 1)[1]
        url = "http://127.0.0.1:" + port
        pid = int(docker("inspect", "--format", "{{.State.Pid}}", name))
        statuses, team_latencies, health = [], [], []
        async with aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=90)) as client:
            # Claim GETs must never be retried invisibly after a lost response.
            client._retry_connection=False
            for _ in range(200):
                try:
                    async with client.get(url + "/readyz") as response:
                        if response.status == 200:
                            break
                except aiohttp.ClientError:
                    pass
                await asyncio.sleep(.025)
            else:
                raise AssertionError("public relay did not become ready")
            with tempfile.TemporaryDirectory(prefix=name) as directory:
                environment = os.environ.copy()
                environment["ZERUS_RELAY_PUBLIC_FIXTURE_DIR"] = directory
                subprocess.run(["cargo", "+1.99.1", "test", "--manifest-path", str(ROOT / "services/mobile/relay/Cargo.toml"),
                                "--locked", "--test", "public_workspace", "generate_public_load_fixtures", "--",
                                "--ignored", "--exact"], env=environment, check=True, stdout=subprocess.DEVNULL)
                directory = Path(directory)
                metadata = json.loads((directory / "metadata.json").read_text())
                for lane in ("standard","team"):
                    path=directory / f"{lane}-malformed.json"
                    shutil.copyfile(directory / f"{lane}-request.json",path)
                    with path.open("ab") as stream:stream.write(b" trailing")

                async def small(path, body, secret=None):
                    headers = {"Authorization": "Bearer " + secret} if secret else {}
                    async with client.post(url + path, json=body, headers=headers) as response:
                        result = await response.json()
                        assert response.status in (200, 202), (response.status, result)
                        return result

                for lane in ("standard", "team"):
                    m = metadata[lane]
                    read = lambda kind: json.loads((directory / f"{lane}-{kind}.json").read_text())
                    await small("/v2/workspaces", read("create"))
                    await small("/v2/node/invitations", read("invite"), m["node_token"])
                    await small("/v2/enrollments", read("join"))
                    await small("/v2/node/enrollments/" + m["device_id"] + "/approve", read("approve"), m["node_token"])
                    await small("/v2/node/routes", read("routes"), m["node_token"])
                # Only this fixture's operator CLI may assign priority.
                subprocess.run(["docker", "exec", name, "zerus-relay", "--config", "/data/config.json",
                                "service-class", "--workspace", metadata["team"]["workspace_id"], "--class", "team"],
                               check=True, stdout=subprocess.DEVNULL)
                await asyncio.sleep(2.2)  # Documented bounded auth-cache convergence.
                done = asyncio.Event()

                async def observe():
                    while not done.is_set():
                        start = time.perf_counter()
                        async with client.get(url + "/healthz") as response:
                            await response.read()
                            assert response.status == 200
                        health.append((time.perf_counter() - start) * 1000)
                        await asyncio.sleep(.01)

                async def upload(lane, kind="request", slow=False):
                    path = directory / f"{lane}-{kind}.json"
                    async def chunks():
                        with path.open("rb") as stream:
                            while chunk := stream.read(65536):
                                yield chunk
                                await asyncio.sleep(.001 if slow else 0)
                    start = time.perf_counter()
                    async with client.post(url + "/v2/requests", data=chunks(), headers={
                            "Authorization": "Bearer " + metadata[lane]["device_token"],
                            "Content-Type": "application/json", "Content-Length": str(path.stat().st_size)}) as response:
                        body = await response.json()
                        assert response.status in (202, 400, 401, 429), (response.status, body)
                        statuses.append((lane, kind, response.status))
                        if lane == "team" and response.status == 202:
                            team_latencies.append((time.perf_counter() - start) * 1000)
                        return response.status

                observer = asyncio.create_task(observe())
                try:
                    # The standard max-sized upload owns its only ordinary
                    # heavy slot while team traffic takes the reserved one.
                    for _ in range(repeats):
                        standard = asyncio.create_task(upload("standard", slow=True))
                        await asyncio.sleep(.075)
                        await asyncio.gather(standard, upload("standard"), upload("standard"), upload("team"))
                    assert team_latencies, "team request must progress during ordinary overload"
                    assert any(lane == "standard" and status == 429 for lane, _, status in statuses)
                    for lane in ("standard", "team"):
                        assert await upload(lane) == 202
                        assert await upload(lane, "signature") == 401
                        assert await upload(lane, "malformed") == 400
                    # Claim/response spooling is also exercised at the maximum.
                    async def claim(lane):
                        async with client.get(url + "/v2/node/requests", headers={
                                "Authorization": "Bearer " + metadata[lane]["node_token"]}) as response:
                            assert response.status == 200
                            body = await response.read()
                        value = await asyncio.to_thread(json.loads, body)
                        assert len(value["requests"]) == 1
                        assert value["requests"][0]["header"]["request_id"] == metadata[lane]["request_id"]
                        del value, body
                        await small("/v2/node/requests/" + metadata[lane]["request_id"] + "/result",
                                    json.loads((directory / f"{lane}-result.json").read_text()), metadata[lane]["node_token"])
                    await asyncio.gather(claim("standard"), claim("team"))
                    for lane in ("standard", "team"):
                        async with client.get(url + "/v2/node/requests", headers={
                                "Authorization": "Bearer " + metadata[lane]["node_token"]}) as response:
                            assert (await response.json())["requests"] == []
                        assert await upload(lane) == 202  # Exact receipt, never a second claim.
                        async with client.get(url+"/v2/node/requests",headers={"Authorization":"Bearer "+metadata[lane]["node_token"]}) as response:
                            assert (await response.json())["requests"]==[]
                finally:
                    done.set()
                    await observer
        events = dict(line.split() for line in docker("exec", name, "cat", "/sys/fs/cgroup/memory.events").splitlines())
        peak = int(docker("exec", name, "cat", "/sys/fs/cgroup/memory.peak"))
        assert events["oom_kill"] == "0" and events["oom"] == "0", events
        assert peak <= 256 * 2**20
        await asyncio.sleep(.25)
        resting, hwm = memory_kib(pid)
        assert resting < 64 * 1024, "maximum ciphertext allocations must not accumulate"
        health.sort()
        return {"image_id": docker("image", "inspect", "--format", "{{.Id}}", image), "binary_sha256":docker("exec",name,"sha256sum","/usr/local/bin/zerus-relay").split()[0],"memory_limit_mib": 256,
                "cgroup_peak_mib": round(peak / 2**20, 2), "rss_high_water_mib": round(hwm / 1024, 2),
                "resting_rss_mib": round(resting / 1024, 2), "oom_kills": 0, "repeats": repeats,
                "heavy_slots": {"total": 2, "standard": 1, "team_reserved": 1},
                "database_pool_total": 10, "auth_control_pools": {"standard": 2, "team": 2},
                "declared_disk_spool_mib":164,
                "statuses": {str(key): count for key, count in Counter(statuses).items()},
                "health_samples": len(health), "health_p99_ms": round(health[int(len(health) * .99)], 2),
                "team_max_upload_ms": round(max(team_latencies), 2)}
    except BaseException:
        if started:
            print("owned public fixture:", docker("inspect", "--format", "{{.State.OOMKilled}} {{.State.ExitCode}}", name), file=sys.stderr)
            print(docker("logs", "--tail", "40", name),file=sys.stderr)
            print(docker("logs", "--tail", "40", database),file=sys.stderr)
            print(docker("exec",name,"cat","/sys/fs/cgroup/memory.events"),file=sys.stderr)
        raise
    finally:
        for container in (name, database):
            subprocess.run(["docker", "rm", "--force", container], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        for volume in (data, spool, pgdata):
            subprocess.run(["docker", "volume", "rm", volume], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        subprocess.run(["docker", "network", "rm", network], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", default="zerus-relay-contract")
    parser.add_argument("--repeats", type=int, choices=range(1, 11), default=5)
    arguments = parser.parse_args()
    print(json.dumps(asyncio.run(qualify(arguments.image, arguments.repeats)), indent=2))
