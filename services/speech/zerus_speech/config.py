import json
import os
from pathlib import Path
import re
import sqlite3
import stat
from contextlib import closing
from urllib.parse import quote


def default_config() -> Path:
    return Path(os.environ.get("HGS_CONFIG_DIR", str(Path.home() / ".config/hgs"))) / "speech.json"


def load(path: Path) -> dict:
    if path.exists() and path.stat().st_size > 65536:
        raise ValueError("Speech configuration is too large")
    config = json.loads(path.read_text()) if path.exists() else {"schema": 1, "profiles": []}
    if not isinstance(config, dict) or config.get("schema") != 1:
        raise ValueError("Speech configuration requires schema 1")
    state = config.get("state_dir", str(Path(os.environ.get("XDG_STATE_HOME", str(Path.home() / ".local/state"))) / "hgs/speech"))
    config["state_dir"] = Path(state)
    config["socket_path"] = Path(config.get("socket_path", str(config["state_dir"] / "service.sock")))
    if not config["state_dir"].is_absolute() or not config["socket_path"].is_absolute():
        raise ValueError("Speech state and socket paths must be absolute")
    profiles = config.get("profiles", [])
    if not isinstance(profiles, list) or len(profiles) > 32:
        raise ValueError("Configure up to 32 speech profiles")
    ids = set()
    for profile in profiles:
        if (not isinstance(profile, dict) or not isinstance(profile.get("id"), str)
                or not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", profile["id"]) or profile["id"] in ids):
            raise ValueError("Speech profile IDs must be unique ASCII names")
        for field in ("name", "adapter", "model", "language"):
            minimum = 0 if field == "language" else 1
            if not isinstance(profile.get(field), str) or not minimum <= len(profile[field]) <= 128:
                raise ValueError("Speech profile requires bounded name, adapter, model and language")
        if profile.get("device", "cpu") not in {"cpu", "cuda", "cuda:0", "cuda:1", "mps"}:
            raise ValueError("Unsupported speech inference device")
        if "download_root" in profile and not isinstance(profile["download_root"], str):
            raise ValueError("Model cache path must be a string")
        ids.add(profile["id"])
    config["profiles"] = profiles
    config["managed_store"] = path.parent / "speech-profiles" / "profiles.sqlite3"
    if type(config.get("analytics_enabled", True)) is not bool:
        raise ValueError("analytics_enabled must be a boolean")
    config["analytics_enabled"] = config.get("analytics_enabled", True)
    for key, default, minimum, maximum in (("max_queue", 4, 1, 16), ("max_receipts", 100000, 1, 1000000),
                                           ("result_ttl_seconds", 86400, 1, 604800)):
        value = config.get(key, default)
        if type(value) is not int or not minimum <= value <= maximum:
            raise ValueError("Invalid speech service limit: " + key)
        config[key] = value
    return config


def profiles(config: dict) -> dict:
    """Read a committed host configuration, never network/model work on discovery."""
    path = config.get("managed_store")
    if path is None or not path.exists() and not path.is_symlink():
        return {p["id"]: dict(p) for p in config["profiles"]}
    info = path.lstat()
    parent = path.parent.lstat()
    if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or info.st_mode & 0o077
            or info.st_nlink != 1 or not stat.S_ISDIR(parent.st_mode)
            or parent.st_uid != os.geteuid() or parent.st_mode & 0o077):
        raise ValueError("Speech profile store must be private")
    # Do not open/close a raw descriptor: it can release SQLite's process locks.
    uri = "file:" + quote(str(path.parent.resolve() / path.name)) + "?mode=ro"
    with closing(sqlite3.connect(uri, uri=True, timeout=5)) as db:
        rows = db.execute("SELECT id,revision,profile,secret FROM profiles WHERE deleted=0 ORDER BY id LIMIT 33").fetchall()
    if len(rows) > 32:
        raise ValueError("Too many speech profiles")
    result = {}
    for identity, revision, encoded, secret in rows:
        profile = json.loads(encoded)
        profile["revision"] = revision
        if secret is not None:
            profile["api_key"] = secret
        result[identity] = profile
    return result
