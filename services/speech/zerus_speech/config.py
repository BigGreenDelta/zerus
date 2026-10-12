import json
import os
from pathlib import Path
import re


def default_config() -> Path:
    return Path(os.environ.get("HGS_CONFIG_DIR", str(Path.home() / ".config/hgs"))) / "speech.json"


def load(path: Path) -> dict:
    if path.stat().st_size > 65536:
        raise ValueError("Speech configuration is too large")
    config = json.loads(path.read_text())
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
            if not isinstance(profile.get(field), str) or not 1 <= len(profile[field]) <= 128:
                raise ValueError("Speech profile requires bounded name, adapter, model and language")
        if profile.get("device", "cpu") not in {"cpu", "cuda", "cuda:0", "cuda:1", "mps"}:
            raise ValueError("Unsupported speech inference device")
        if "download_root" in profile and not isinstance(profile["download_root"], str):
            raise ValueError("Model cache path must be a string")
        ids.add(profile["id"])
    config["profiles"] = profiles
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
