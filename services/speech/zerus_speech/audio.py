"""Bounded PCM validation and silence-aware segmentation without model downloads."""
from array import array
import base64
import binascii
import math
import sys

SAMPLE_RATE = 16000
MAX_DURATION_MS = 120000
MAX_AUDIO_BYTES = SAMPLE_RATE * 2 * MAX_DURATION_MS // 1000


def decode_audio(audio: object) -> bytes:
    if not isinstance(audio, dict) or set(audio) != {"format", "sample_rate", "channels", "data_base64"}:
        raise ValueError("Audio must contain format, sample_rate, channels and data_base64")
    if (audio["format"] != "pcm_s16le" or type(audio["sample_rate"]) is not int
            or audio["sample_rate"] != SAMPLE_RATE or type(audio["channels"]) is not int
            or audio["channels"] != 1):
        raise ValueError("Audio must be mono PCM s16le at 16000 Hz")
    encoded = audio["data_base64"]
    if not isinstance(encoded, str) or len(encoded) > (MAX_AUDIO_BYTES // 3 + 1) * 4:
        raise ValueError("Audio exceeds the 120 second limit")
    try:
        pcm = base64.b64decode(encoded, validate=True)
    except (ValueError, binascii.Error):
        raise ValueError("Audio is not valid base64") from None
    if not pcm or len(pcm) % 2 or len(pcm) > MAX_AUDIO_BYTES:
        raise ValueError("Audio must contain complete PCM samples, up to 120 seconds")
    return pcm


def segments(pcm: bytes) -> list[tuple[int, int]]:
    """Return sample boundaries under GigaAM's 25 s limit.

    A conservative amplitude gate omits digital silence, preserves 200 ms around
    speech and prefers quiet cuts after 18 s. Continuous speech uses 400 ms
    overlap; text stitching only removes exact matching boundary words. This is
    not a neural VAD and does not attempt to classify noise as speech.
    """
    samples = array("h", pcm)
    if sys.byteorder != "little":
        samples.byteswap()
    frame = SAMPLE_RATE // 50  # 20 ms
    levels = [math.sqrt(sum(v * v for v in samples[i:i + frame]) / len(samples[i:i + frame]))
              for i in range(0, len(samples), frame)]
    active = [i for i, level in enumerate(levels) if level > 35]
    if not active:
        return []
    start = max(0, (active[0] - 10) * frame)
    stop = min(len(samples), (active[-1] + 11) * frame)
    result = []
    while stop - start > 24 * SAMPLE_RATE:
        low = (start + 18 * SAMPLE_RATE) // frame
        high = (start + 24 * SAMPLE_RATE) // frame
        cut_frame = min(range(low, high), key=lambda i: levels[i])
        if levels[cut_frame] <= 35:
            end = cut_frame * frame
            result.append((start, end))
            start = end
        else:
            end = start + 24 * SAMPLE_RATE
            result.append((start, end))
            start = end - int(0.4 * SAMPLE_RATE)
    result.append((start, stop))
    return result


def join_text(previous: str, following: str, *, overlap: bool) -> str:
    if not previous:
        return following.strip()
    if not following.strip():
        return previous
    left, right = previous.split(), following.split()
    if overlap:
        normalized = lambda word: word.casefold().strip(".,!?;:—–-\"'«»()")
        for count in range(min(12, len(left), len(right)), 0, -1):
            if [normalized(w) for w in left[-count:]] == [normalized(w) for w in right[:count]]:
                right = right[count:]
                break
    return " ".join(left + right)
