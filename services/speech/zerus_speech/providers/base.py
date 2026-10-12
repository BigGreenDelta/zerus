from typing import Callable, Protocol


class Provider(Protocol):
    def available(self, profile: dict) -> tuple[bool, str]: ...
    def transcribe(self, profile: dict, pcm: bytes, cancelled: Callable[[], bool]) -> str: ...


class Cancelled(Exception):
    pass


class ProviderError(Exception):
    """Only a reviewed error code crosses the service boundary."""
    def __init__(self, code: str):
        self.code = code
        super().__init__(code)
