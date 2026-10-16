"""Bounded original MP4 admission; recognition always receives derived audio."""
import base64
import binascii

FEATURE = "voice_video"
HELP_MARKER = "voice-video-v1"
MAX_BYTES = 10 * 1024 * 1024
MAX_REQUEST_BYTES = 19 * 1024 * 1024


def _decode(value, maximum):
    if not isinstance(value, str) or len(value) > (maximum + 2) // 3 * 4:
        raise ValueError("video data exceeds its bound")
    try:
        result = base64.b64decode(value, validate=True)
    except (ValueError, binascii.Error):
        raise ValueError("invalid video encoding") from None
    if len(result) > maximum:
        raise ValueError("video data exceeds its bound")
    return result


def validate(value, pcm_bytes):
    required = {"format", "data_base64", "duration_ms", "width", "height"}
    if (not isinstance(value, dict) or not required <= set(value)
            or set(value) - required - {"poster_base64"} or value["format"] != "mp4"):
        raise ValueError("invalid video recording fields")
    duration = value["duration_ms"]
    if type(duration) is not int or not 100 <= duration <= 60_000:
        raise ValueError("video recording exceeds 60 seconds")
    if not 3_200 <= pcm_bytes <= 1_920_000 or abs(duration - pcm_bytes * 1000 // 32000) > 1000:
        raise ValueError("video and speech audio durations do not match")
    if any(type(value[field]) is not int or not 16 <= value[field] <= 1920 for field in ("width", "height")):
        raise ValueError("invalid video recording dimensions")
    data = _decode(value["data_base64"], MAX_BYTES)
    offset = 0
    boxes = 0
    found = set()
    while offset < len(data):
        boxes += 1
        if boxes > 4096 or len(data) - offset < 8:
            raise ValueError("invalid MP4 recording")
        size = int.from_bytes(data[offset:offset + 4], "big")
        kind = data[offset + 4:offset + 8]
        header = 8
        if size == 1:
            if len(data) - offset < 16:
                raise ValueError("invalid MP4 recording")
            header = 16
            size = int.from_bytes(data[offset + 8:offset + 16], "big")
        elif size == 0:
            size = len(data) - offset
        if size < header or size > len(data) - offset:
            raise ValueError("invalid MP4 recording")
        if kind == b"ftyp" and (offset != 0 or not 16 <= size <= 4096):
            raise ValueError("invalid MP4 recording")
        if kind != b"mdat" or size > header:
            found.add(kind)
        offset += size
    if not {b"ftyp", b"moov", b"mdat"} <= found:
        raise ValueError("invalid MP4 recording")
    if "poster_base64" in value:
        poster = _decode(value["poster_base64"], 8192)
        if not poster.startswith(b"\xff\xd8") or not poster.endswith(b"\xff\xd9"):
            raise ValueError("invalid video poster")
