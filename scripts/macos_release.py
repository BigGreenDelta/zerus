"""Homebrew cask rendering and macOS candidate metadata; no build or publication side effects."""
import hashlib
import json
from pathlib import Path
import re
import shutil

ROOT = Path(__file__).resolve().parents[1]
# One cask per update channel. Both install the same Zerus.app and conflict with each other.
CASKS = {"stable": "zerus", "nightly": "zerus@nightly"}
MINIMUM_SYSTEM = "14.0"
STABLE = re.compile(r"\d+\.\d+\.\d+")
NIGHTLY = re.compile(r"\d+\.\d+\.\d+\.r\d+\.g[0-9a-f]{7}\.n(\d+)")
NIGHTLY_TAG = re.compile(r"nightly-[1-9]\d*")
DIGEST = re.compile(r"[0-9a-f]{64}")


def _require(condition, message):
    if not condition:
        raise ValueError(message)


def macos_asset(version):
    return f"zerus-{version}-macos-arm64.zip"


def cask_for(channel):
    _require(channel in CASKS, "Unknown Homebrew channel.")
    return CASKS[channel]


def _channel_version(version, channel):
    pattern = NIGHTLY if cask_for(channel) == CASKS["nightly"] else STABLE
    _require(isinstance(version, str) and pattern.fullmatch(version), f"Cask version must be a {channel} version.")


def render_cask(version, digest, channel="stable", tag=None):
    _channel_version(version, channel)
    _require(DIGEST.fullmatch(digest), "Cask digest must be a lowercase SHA-256.")
    text = (ROOT / "packaging/homebrew" / f"{cask_for(channel)}.rb.in").read_text()
    if channel == "nightly":
        # Nightly prereleases are immutable; the tag holds the workflow run ID, not the version.
        _require(isinstance(tag, str) and NIGHTLY_TAG.fullmatch(tag), "Nightly cask needs its nightly release tag.")
        text = text.replace("@RELEASE_TAG@", tag)
    return text.replace("@VERSION@", version).replace("@SHA256@", digest)


def cask_fields(text):
    fields = {}
    for key in ("version", "sha256"):
        values = re.findall(rf'(?m)^  {key} "([^"]*)"$', text)
        _require(len(values) == 1, f"Cask must declare exactly one {key}.")
        fields[key] = values[0]
    return fields


def require_template_cask(text, version, digest, channel="stable", tag=None):
    _require(text == render_cask(version, digest, channel, tag), "Homebrew cask differs from the reviewed template.")


def cask_version_key(version, channel="stable"):
    try:
        _channel_version(version, channel)
    except ValueError:
        raise ValueError("Unexpected cask version; inspect the tap manually.") from None
    if channel == "nightly":
        # Build numbers of the Nightly workflow only grow; the product version may not.
        return (int(NIGHTLY.fullmatch(version).group(1)),)
    return tuple(int(part) for part in version.split("."))


def validate_macos_info(info, version, channel="stable"):
    """Check the provenance recorded by the signing job for a schema 2 candidate."""
    macos = info.get("macos")
    _require(isinstance(macos, dict) and set(macos) == {"asset", "qt_version", "team_id", "notarization_id",
                                                        "minimum_system_version"}, "Missing macOS provenance.")
    _require(macos["asset"] == macos_asset(version), "Unexpected macOS provenance: asset name.")
    _require(isinstance(macos["qt_version"], str) and STABLE.fullmatch(macos["qt_version"]),
             "Unexpected macOS provenance: Qt version.")
    _require(isinstance(macos["team_id"], str) and re.fullmatch(r"[A-Z0-9]{10}", macos["team_id"]),
             "Unexpected macOS provenance: Team ID.")
    _require(isinstance(macos["notarization_id"], str) and
             re.fullmatch(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}", macos["notarization_id"]),
             "Unexpected macOS provenance: notarization ID.")
    _require(macos["minimum_system_version"] == MINIMUM_SYSTEM, "Unexpected macOS provenance: minimum system version.")
    _require(info.get("homebrew_casks") == [cask_for(channel)], "Unexpected Homebrew cask family.")
    return macos


def add_macos(output, source, version, info, channel="stable"):
    """Copy the signed, notarized macOS asset into a candidate and render its cask.

    Everything is checked before the candidate changes, so a rejected artifact
    leaves no partial macOS files behind.
    """
    expected = {macos_asset(version), "macos-info.json"}
    entries = list(source.iterdir())
    _require({path.name for path in entries} == expected and
             all(path.is_file() and not path.is_symlink() for path in entries),
             "Unexpected signed macOS artifact contents.")
    metadata = json.loads((source / "macos-info.json").read_text())
    cask = cask_for(channel)
    validate_macos_info(dict(info, macos=metadata, homebrew_casks=[cask]), version, channel)
    tag = info.get("release_tag") if channel == "nightly" else None
    render_cask(version, "0" * 64, channel, tag)
    archive = output / macos_asset(version)
    shutil.copyfile(source / macos_asset(version), archive)
    with archive.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    (output / "homebrew").mkdir()
    (output / "homebrew" / f"{cask}.rb").write_text(render_cask(version, digest, channel, tag))
    info["macos"] = metadata
    return metadata
