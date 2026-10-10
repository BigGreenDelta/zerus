"""One-hop inventory and guarded per-machine contexts owned by one connector."""
from __future__ import annotations

import asyncio
import json
import logging
import uuid

LOG = logging.getLogger("zerus.connector")
MAX_PEERS = 32
MAX_PEER_SNAPSHOT = 1024 * 1024
# Keep stable across upgrades: an alias rebound to another UUID gets a new route.
ROUTE_NAMESPACE = uuid.UUID("fab5cf3c-2c74-5dcc-9a8c-83c5f1f2d7de")


def canonical_uuid(value):
    try:
        return isinstance(value, str) and str(uuid.UUID(value)) == value
    except (ValueError, AttributeError):
        return False


def route_id(via: str, machine_id: str) -> str:
    return str(uuid.uuid5(ROUTE_NAMESPACE, via + "\0" + machine_id))


class Fleet:
    def __init__(self, connector, error):
        self.connector, self.error = connector, error
        self.supported = False
        self.probe_complete = False
        self.relay_supported = True
        self.local_id = None
        self.routes = {}
        self.contexts = {}
        self.published = set()
        self.discovery_lock = asyncio.Lock()
        self.native_slots = asyncio.Semaphore(4)

    @property
    def available(self):
        return self.supported and self.relay_supported and self.local_id is not None

    async def discover(self):
        """Only the native inventory decides aliases; relay data never supplies one."""
        async with self.discovery_lock:
            if not self.supported:
                return
            try:
                raw = await self.connector._native(["swarm", "mobile-peers", "--json"],
                                                   timeout=12, max_output_bytes=128 * 1024)
                local = raw.get("local") if isinstance(raw, dict) else None
                peers = raw.get("peers") if isinstance(raw, dict) else None
                if (raw.get("schema") != 1 or not isinstance(local, dict)
                        or not canonical_uuid(local.get("machine_id"))
                        or not isinstance(peers, list) or len(peers) > MAX_PEERS):
                    raise self.error("native direct peer inventory has invalid format")
                routes, aliases = {}, set()
                for row in peers:
                    if not isinstance(row, dict):
                        raise self.error("native direct peer inventory has invalid format")
                    via, machine = row.get("via"), row.get("machine_id")
                    if (not isinstance(via, str) or not via or len(via) > 512
                            or any(ord(c) < 32 or ord(c) == 127 for c in via)
                            or via in aliases or type(row.get("online")) is not bool):
                        raise self.error("native direct peer inventory has invalid format")
                    aliases.add(via)
                    # An unreachable peer without a verified identity cannot be advertised.
                    if machine is None and row["online"] is False:
                        continue
                    if not canonical_uuid(machine):
                        raise self.error("native direct peer inventory has invalid format")
                    if machine == local["machine_id"]:
                        continue
                    identity = route_id(via, machine)
                    name = row.get("name")
                    name = "".join(c for c in name if ord(c) >= 32 and ord(c) != 127)[:128] if isinstance(name, str) else "Peer computer"
                    routes[identity] = {"route_id": identity, "machine_id": machine,
                                        "via": via, "name": name or "Peer computer", "online": row["online"]}
                self.local_id = local["machine_id"]
            except (self.error, ValueError, TypeError, AttributeError):
                # Fail closed, rather than keep a stale alias usable after an outage.
                routes = {}
                LOG.warning("Direct peer discovery unavailable")
            self.routes = routes
            removed = [self.contexts.pop(identity) for identity in list(self.contexts) if identity not in routes]
            for identity, row in routes.items():
                if identity not in self.contexts:
                    self.contexts[identity] = self.connector.peer_context(row["machine_id"], self.callback(identity, row["machine_id"]))
            await asyncio.gather(*(context.accounts.close() for context in removed))

    def callback(self, identity, machine):
        async def native(argv, payload=None, *, timeout=None, json_output=True, failure_json=False):
            budget = self.connector.timeout if timeout is None else timeout
            async def invoke():
                async with self.native_slots:
                    row = self.routes.get(identity)
                    if not row or not row["online"] or row["machine_id"] != machine:
                        raise self.error("direct peer route is unavailable or changed")
                    envelope = {"schema": 1, "via": row["via"], "target_machine_id": machine,
                                "argv": argv, "payload": payload}
                    return await self.connector._native(["swarm", "mobile-peer", "--json"], envelope,
                        timeout=budget, json_output=json_output, failure_json=failure_json,
                        max_output_bytes=MAX_PEER_SNAPSHOT if argv == ["ls", "--json", "--local"] else None)
            try:
                return await asyncio.wait_for(invoke(), budget)
            except asyncio.TimeoutError:
                raise self.error("native peer command timed out") from None
        return native

    async def resolve(self, envelope, *, present):
        if not present:
            return self.connector, None
        if (not isinstance(envelope, dict) or set(envelope) != {"schema", "route_id", "computer_id", "machine_id"}
                or type(envelope.get("schema")) is not int or envelope["schema"] != 1
                or not canonical_uuid(envelope.get("computer_id")) or not canonical_uuid(envelope.get("machine_id"))
                or (envelope.get("route_id") is not None and not canonical_uuid(envelope["route_id"]))):
            raise self.error("invalid immutable gateway route")
        if not self.available:
            raise self.error("guarded native routing is unavailable")
        identity, machine = envelope["route_id"], envelope["machine_id"]
        if identity is None:
            if machine != self.local_id:
                raise self.error("local native machine identity changed")
            return self.connector, machine
        # Inventory is refreshed independently. Every guarded native dispatch
        # rechecks the current configured edge and exact target UUID; resolving a
        # claim must not scan all other peers or wait for their network deadlines.
        row = self.routes.get(identity)
        if not row or not row["online"] or row["machine_id"] != machine:
            raise self.error("direct peer route is unavailable or changed")
        context = self.contexts[identity]
        if not context.snapshot_ready.is_set():
            await context.snapshot()
        return context, None

    def heartbeat(self, snapshot):
        body = {"snapshot": snapshot}
        if self.available:
            body.update(machine_id=self.local_id, peers=[{key: row[key] for key in
                ("route_id", "machine_id", "name", "online")} for _, row in sorted(self.routes.items())])
        return body

    def confirm_manifest(self, body):
        self.published = {(row["route_id"], row["machine_id"]) for row in body.get("peers", []) if row["online"]}

    async def publish(self, client):
        async def peer(identity, row):
            if not row["online"] or (identity, row["machine_id"]) not in self.published:
                return
            try:
                snapshot = await self.contexts[identity].snapshot()
                body = {"machine_id": row["machine_id"], "snapshot": snapshot}
                if len(json.dumps(body, ensure_ascii=True).encode()) > MAX_PEER_SNAPSHOT:
                    raise self.error("peer snapshot exceeds relay size limit")
                current = self.routes.get(identity)
                if current != row or (identity, row["machine_id"]) not in self.published:
                    return
                await self.connector.http(client, "POST", f"/v1/node/peers/{identity}/heartbeat", body)
            except self.error as error:
                # Missing routes are ordinary withdrawal/revocation races. Only
                # an unsupported POST method identifies endpoint incompatibility;
                # one unavailable peer must never downgrade other guarded routes.
                if getattr(error, "status", None) == 405:
                    self.relay_supported = False
                    self.published.clear()
                LOG.warning("Peer heartbeat unavailable")
            except KeyError:
                LOG.warning("Peer route was withdrawn during observation")
        await asyncio.gather(*(peer(identity, dict(row)) for identity, row in list(self.routes.items())))

    async def run(self, client):
        while True:
            if self.supported and self.relay_supported:
                await self.discover()
                await self.publish(client)
            await asyncio.sleep(self.connector.interval)

    async def close(self):
        await asyncio.gather(*(context.accounts.close() for context in self.contexts.values()))
