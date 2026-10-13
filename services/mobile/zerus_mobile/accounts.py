"""Bounded public account projections and a single read-only background refresh."""
from __future__ import annotations

import asyncio
import copy
from datetime import datetime
import json
import math
import re
import time

REFRESH_SECONDS = 300
CATALOG_SECONDS = 30
MAX_ACCOUNTS = 100
MAX_CATALOG_BYTES = 128 * 1024
PROVIDERS = {"codex", "claude", "kimi", "dsh", "opencode"}
STATUSES = {"loading", "ok", "unavailable", "offline", "configured", "expired", "signed_out",
            "credentials_locked", "desktop_session_unavailable", "credentials_unavailable"}


def text(value, maximum=256):
    if not isinstance(value, str):
        return None
    return "".join(c for c in value if ord(c) >= 32 and ord(c) != 127)[:maximum]


def number(value):
    return value if type(value) in (int, float) and 0 <= value <= 2**53 - 1 and math.isfinite(value) else None


def iso_reset(value):
    if not isinstance(value, str) or not re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{1,9})?(?:Z|[+-]\d{2}:\d{2})", value):
        return False
    try:
        datetime.fromisoformat(value.replace("Z", "+00:00"))
        return True
    except ValueError:
        return False


def member(value, choices):
    return isinstance(value, str) and value in choices


def model_options(profile):
    """Project configured IDs and labels; never provider URLs or credentials."""
    rows = profile.get("model_options")
    result, seen = [], set()
    for row in (rows if isinstance(rows, list) else [])[:256]:
        if not isinstance(row, dict):
            continue
        value = row.get("value")
        if (not isinstance(value, str) or len(value.encode("utf-8")) > 256
                or any(c.isspace() or ord(c) < 32 or ord(c) == 127 for c in value)):
            continue
        provider, separator, model = value.partition("/")
        if (not separator or not model or not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]{0,79}", provider)
                or value in seen):
            continue
        seen.add(value)
        result.append({"value": value, "label": text(row.get("label"), 160) or value,
                       "provider": provider})
    return result


def usage(raw):
    """Never forward native errors, paths, auth fingerprints or arbitrary fields."""
    raw = raw if isinstance(raw, dict) else {}
    result = {"status": raw.get("status") if member(raw.get("status"), STATUSES) else "unavailable",
              "identity": {}, "windows": [], "refreshing": False, "refresh_error": False}
    for field in ("name", "email", "organization", "plan", "auth_method", "account_id"):
        value = text(raw.get("identity", {}).get(field)) if isinstance(raw.get("identity"), dict) else None
        if value:
            result["identity"][field] = value
    if member(raw.get("auth_status"), {"signed_in", "signed_out", "unknown"}):
        result["auth_status"] = raw["auth_status"]
    if member(raw.get("source"), {"Codex App Server", "Claude Code", "Kimi Code", "DeepSeek Harness", "OpenCode"}):
        result["source"] = raw["source"]
    checked = number(raw.get("checked_at"))
    if checked is not None:
        result["checked_at"] = checked
    if type(raw.get("identity_cached")) is bool:
        result["identity_cached"] = raw["identity_cached"]
    for window in (raw.get("windows") if isinstance(raw.get("windows"), list) else [])[:16]:
        if not isinstance(window, dict) or number(window.get("used_percent")) is None:
            continue
        row = {"used_percent": window["used_percent"]}
        for key in ("id", "label"):
            value = text(window.get(key), 160)
            if value is not None:
                row[key] = value
        minutes = number(window.get("window_minutes"))
        if minutes is not None:
            row["window_minutes"] = minutes
        reset = window.get("resets_at")
        if number(reset) is not None:
            row["resets_at"] = reset
        elif iso_reset(reset):
            row["resets_at"] = reset
        result["windows"].append(row)
    credits = raw.get("credits")
    if isinstance(credits, dict):
        projected = {}
        balance = text(credits.get("balance"), 80)
        if balance is not None and re.fullmatch(r"-?\d+(?:\.\d+)?", balance):
            projected["balance"] = balance
        if type(credits.get("unlimited")) is bool:
            projected["unlimited"] = credits["unlimited"]
        if projected:
            result["credits"] = projected
    balances = []
    for balance in (raw.get("balances") if isinstance(raw.get("balances"), list) else [])[:16]:
        if (isinstance(balance, dict) and member(balance.get("currency"), {"USD", "CNY"})
                and isinstance(balance.get("balance"), str)
                and re.fullmatch(r"-?\d{1,40}(?:\.\d{1,20})?", balance["balance"])):
            balances.append({"balance": balance["balance"], "currency": balance["currency"],
                             "kind": text(balance.get("kind"), 80) or "Wallet"})
    if balances:
        result["balances"] = balances
    return result


def catalog(raw):
    if not isinstance(raw, dict) or not isinstance(raw.get("profiles"), list) or len(raw["profiles"]) > MAX_ACCOUNTS:
        raise ValueError("native account catalog unavailable")
    entries = {}
    for profile in raw["profiles"]:
        if not isinstance(profile, dict):
            raise ValueError("invalid account row")
        identity = profile.get("id")
        if (not isinstance(identity, str) or not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_-]{0,79}", identity)
                or not member(profile.get("provider"), PROVIDERS) or identity in entries):
            raise ValueError("invalid account identity")
        public = {"id": identity, "provider": profile["provider"], "label": text(profile.get("label"), 160) or identity}
        for key in ("native", "installed", "is_default"):
            public[key] = profile.get(key) is True
        if profile["provider"] == "opencode":
            public["model_options"] = model_options(profile)
        # These values stay local and are discarded with this in-memory cache.
        seed = usage(profile.get("account_status"))
        auth_revision = profile.get("auth_revision")
        if not isinstance(auth_revision, str) or not auth_revision:
            # DSH omits auth_revision. Its signature-validated catalog identity
            # disappears on sign-out/replacement; never carry old usage across it.
            auth_revision = json.dumps({key: seed.get(key) for key in ("identity", "status", "auth_status")}, sort_keys=True)
        revision = (profile["provider"], auth_revision, profile.get("home"), profile.get("credential_file"))
        entries[identity] = {"public": public, "revision": revision,
                             "usage": seed, "attempted": None}
    if len(json.dumps([v["public"] | {"usage": v["usage"]} for v in entries.values()], ensure_ascii=True).encode()) > MAX_CATALOG_BYTES:
        raise ValueError("native account projection exceeds size limit")
    return entries


class AccountCache:
    def __init__(self, native, *, clock=time.monotonic, wall=time.time, errors=(ValueError, RuntimeError, UnicodeError)):
        self.native, self.clock, self.wall = native, clock, wall
        self.errors = errors
        self.entries = {}
        self.next_catalog = 0
        self.checked_at = None
        self.available = False
        self.stale = False
        self.cursor = 0
        self.task = None

    async def update(self):
        if self.clock() >= self.next_catalog:
            self.next_catalog = self.clock() + CATALOG_SECONDS
            try:
                fresh = catalog(await self.native(["account", "ls"], timeout=3))
                for identity, entry in fresh.items():
                    previous = self.entries.get(identity)
                    if previous and previous["revision"] == entry["revision"]:
                        previous["public"] = entry["public"]
                        fresh[identity] = previous
                self.entries = fresh
                self.available = True
                self.stale = False
                self.checked_at = self.wall()
            except self.errors:
                self.stale = True
        if self.available and not self.stale and (self.task is None or self.task.done()):
            installed = [(identity, entry) for identity, entry in self.entries.items() if entry["public"]["installed"]]
            for offset in range(len(installed)):
                index = (self.cursor + offset) % len(installed)
                identity, entry = installed[index]
                if entry["attempted"] is None or self.clock() - entry["attempted"] >= REFRESH_SECONDS:
                    self.cursor = (index + 1) % len(installed)
                    entry["attempted"] = self.clock()
                    entry["usage"] = {**entry["usage"], "refreshing": True}
                    self.task = asyncio.create_task(self.refresh(identity, entry))
                    break
        return self.view()

    async def refresh(self, identity, entry):
        try:
            raw = await self.native(["account", "inspect", identity], timeout=25)
            if not isinstance(raw, dict) or raw.get("id") != identity or raw.get("provider") != entry["public"]["provider"]:
                raise ValueError("account inspection identity changed")
            projected = usage(raw)
            # A provider outage preserves the last values, with explicit failure.
            if projected["status"] == "unavailable" and entry["usage"].get("checked_at") is not None:
                raise ValueError("account usage unavailable")
            if self.entries.get(identity) is entry:
                entry["usage"] = projected
        except self.errors:
            if self.entries.get(identity) is entry:
                entry["usage"] = {**entry["usage"], "refreshing": False, "refresh_error": True}

    def view(self):
        rows = []
        for entry in self.entries.values():
            data = copy.deepcopy(entry["usage"])
            observed = data.get("checked_at")
            data["stale"] = (self.stale or data["refresh_error"] or observed is None
                             or self.wall() - observed >= REFRESH_SECONDS or observed > self.wall() + 60)
            rows.append({**entry["public"], "usage": data})
        result = {"schema": 1, "available": self.available, "stale": self.stale,
                  "refresh_interval_seconds": REFRESH_SECONDS, "accounts": rows}
        if self.checked_at is not None:
            result["checked_at"] = self.checked_at
        if len(json.dumps(result, ensure_ascii=True).encode()) > MAX_CATALOG_BYTES:
            return {"schema": 1, "available": False, "stale": True, "refresh_interval_seconds": REFRESH_SECONDS, "accounts": []}
        return result

    async def close(self):
        if self.task is not None:
            self.task.cancel()
            await asyncio.gather(self.task, return_exceptions=True)
