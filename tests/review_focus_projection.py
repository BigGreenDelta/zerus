"""Exact reviewed test-only focus-wait projection; never advances the physical or probe baseline."""
import hashlib
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = "mobile/android/app/src/androidTest/java/app/zerus/mobile/ApprovalChoicesSmokeInstrumentation.kt"
PROBE = "mobile/android/app/src/main/java/app/zerus/mobile/QuestionUiProbe.kt"
PATCH = ROOT / "tests/fixtures/android-early-review-focus-wait.patch"
PATCH_SHA = "36aba7369aff6650774184d1283ed80f47af9d32aa79ea836f76fc81475720d4"
BASELINES = {
    FIXTURE: "73cee450f08e33c007ad6f3188a6a69135674e2503f80ea48582ba8aa394a9f7",
    PROBE: "beb6a3caf6ae07ca3a062f4f30286aad3b19b71427eed7c9e8d662f7d5ae85be",
}


def reverse_focus_wait(path, source):
    """Reverse only exact frozen postimage hunks; missing, changed or duplicated additions refuse."""
    if path not in BASELINES:
        raise AssertionError("unexpected focus-wait projection path")
    raw = PATCH.read_bytes()
    if hashlib.sha256(raw).hexdigest() != PATCH_SHA:
        raise AssertionError("reviewed focus-wait patch changed")
    files = {}
    for section in raw.decode().split("diff --git ")[1:]:
        header, body = section.split("\n", 1)
        before, after = header.split()
        name = before.removeprefix("a/")
        if name not in BASELINES or after != "b/" + name or name in files:
            raise AssertionError("unexpected focus-wait patch path")
        hunks = []
        for hunk in re.split(r"(?m)^@@[^\n]*\n", body)[1:]:
            lines = hunk.splitlines(keepends=True)
            if not lines or any(line[:1] not in (" ", "+", "-") for line in lines):
                raise AssertionError("unsupported focus-wait hunk")
            old = "".join(line[1:] for line in lines if line[0] in " -")
            new = "".join(line[1:] for line in lines if line[0] in " +")
            # A zero-context pure addition has no old text. Only its exact
            # SHA-sealed, unique postimage may be removed below.
            if not new or old == new:
                raise AssertionError("empty focus-wait hunk")
            hunks.append((old, new))
        if not hunks:
            raise AssertionError("missing focus-wait hunks")
        files[name] = hunks
    if set(files) != set(BASELINES):
        raise AssertionError("incomplete focus-wait patch")
    for old, new in files[path]:
        if source.count(new) != 1:
            raise AssertionError("focus-wait postimage missing, changed or duplicated")
        source = source.replace(new, old, 1)
    if hashlib.sha256(source.encode()).hexdigest() != BASELINES[path]:
        raise AssertionError("original focus-wait baseline changed")
    return source


def original_review_fixture():
    return reverse_focus_wait(FIXTURE, (ROOT / FIXTURE).read_text())
