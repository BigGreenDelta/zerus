from typing import Callable, Protocol


class Provider(Protocol):
    def available(self, profile: dict) -> tuple[bool, str]: ...
    def transcribe(self, profile: dict, pcm: bytes, cancelled: Callable[[], bool]) -> str: ...


class Cancelled(Exception):
    pass
