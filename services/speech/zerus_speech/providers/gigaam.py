"""GigaAM adapter. Import and model allocation occur only on accepted jobs."""
import importlib.util
from pathlib import Path
import tempfile
import wave

from ..audio import SAMPLE_RATE, join_text, segments
from .base import Cancelled

MODELS = {"v3_e2e_rnnt", "v3_e2e_ctc", "v3_rnnt", "v3_ctc",
          "multilingual_ctc", "multilingual_large_ctc"}


class GigaAM:
    def __init__(self, state_dir: Path):
        self.state_dir = state_dir
        self._key = None
        self._model = None

    def available(self, profile: dict) -> tuple[bool, str]:
        if profile.get("model") not in MODELS:
            return False, "Unsupported GigaAM model"
        if importlib.util.find_spec("gigaam") is None or importlib.util.find_spec("torch") is None:
            return False, "Install the optional GigaAM runtime on this computer"
        root = Path(profile.get("download_root", "~/.cache/gigaam")).expanduser()
        names = [profile["model"] + ".ckpt"]
        if "e2e" in profile["model"]:
            names.append(profile["model"] + "_tokenizer.model")
        if any(not (root / name).is_file() for name in names):
            return False, "Prepare this model on the speech computer before recording"
        return True, ""

    def transcribe(self, profile: dict, pcm: bytes, cancelled) -> str:
        boundaries = segments(pcm)
        if not boundaries:
            return ""
        if cancelled():
            raise Cancelled()
        ready, reason = self.available(profile)
        if not ready:
            raise ValueError(reason)
        import gigaam
        key = (profile["model"], profile.get("device", "cpu"),
               str(Path(profile.get("download_root", "~/.cache/gigaam")).expanduser()))
        if key != self._key:
            # Only one resident model, including when the host advertises alternatives.
            self._model = None
            self._key = None
            import gc
            import torch
            gc.collect()
            if torch.cuda.is_available():
                torch.cuda.empty_cache()
            self._model = gigaam.load_model(key[0], device=key[1], download_root=key[2], use_flash=False)
            self._key = key
        text = ""
        previous_end = 0
        for start, end in boundaries:
            if cancelled():
                raise Cancelled()
            with tempfile.NamedTemporaryFile(suffix=".wav", dir=self.state_dir) as output:
                with wave.open(output.name, "wb") as wav:
                    wav.setnchannels(1)
                    wav.setsampwidth(2)
                    wav.setframerate(SAMPLE_RATE)
                    wav.writeframes(pcm[start * 2:end * 2])
                result = self._model.transcribe(output.name)
            # Reviewed upstream returns TranscriptionResult; older installations
            # used strings. Both carry the same plain transcript semantics.
            segment_text = result if isinstance(result, str) else result.text
            text = join_text(text, segment_text, overlap=start < previous_end)
            previous_end = end
        if cancelled():
            raise Cancelled()
        return text
