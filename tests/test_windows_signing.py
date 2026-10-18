#!/usr/bin/env python3
"""The owner rule for signed Windows zips: only Zerus-built executables change."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import conpty_pair
import windows_portable
import windows_signing
from windows_portable_fixture import pinned_pair, portable_zip


class SignedZip(unittest.TestCase):
    def setUp(self):
        pin = patch.object(conpty_pair, "PIN", pinned_pair())
        pin.start()
        self.addCleanup(pin.stop)
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.unsigned = portable_zip(self.root / "unsigned.zip",
                                     entries=[(f"{windows_portable.TOP}/licenses/extra.txt", "optional notice")])

    def signed(self, replace=(), add=(), drop=()):
        """A copy of the unsigned zip with some entries replaced, added or left out."""
        replace = dict(replace)
        path = self.root / "signed.zip"
        path.unlink(missing_ok=True)
        with zipfile.ZipFile(self.unsigned) as source, zipfile.ZipFile(path, "w") as target:
            for entry in source.infolist():
                name = entry.filename[len(windows_portable.TOP) + 1:]
                if name in drop:
                    continue
                target.writestr(entry.filename, replace.get(name, source.read(entry)))
            for name, data in add:
                target.writestr(f"{windows_portable.TOP}/{name}", data)
        return path

    def test_signing_may_change_only_the_executables_built_here(self):
        signed = self.signed(replace={"hgs.exe": b"tested hgs.exe+signature",
                                      "hgs-tray.exe": b"tested hgs-tray.exe+signature"})
        self.assertEqual(windows_signing.changes(self.unsigned, signed), ["hgs-tray.exe", "hgs.exe"])
        self.assertEqual(windows_signing.changes(self.unsigned, self.unsigned), [])

    def test_a_changed_third_party_binary_is_refused(self):
        for name in ("vcruntime140.dll", "Qt6Core.dll", "avcodec-61.dll", "platforms/qwindows.dll", "LICENSE"):
            with self.subTest(name=name):
                with self.assertRaisesRegex(ValueError, f"not built from this repository: {name}"):
                    windows_signing.changes(self.unsigned, self.signed(replace={name: b"re-signed"}))

    def test_the_entry_set_cannot_change(self):
        with self.assertRaisesRegex(ValueError, "changed the zip's files: signature.p7s"):
            windows_signing.changes(self.unsigned, self.signed(add=[("signature.p7s", b"x")]))
        with self.assertRaisesRegex(ValueError, "changed the zip's files: licenses/extra.txt"):
            windows_signing.changes(self.unsigned, self.signed(drop={"licenses/extra.txt"}))
        # A required file must be there in the first place.
        with self.assertRaisesRegex(ValueError, "lacks: README.txt"):
            windows_signing.changes(self.unsigned, self.signed(drop={"README.txt"}))

    def test_a_changed_conpty_pair_fails_its_pin_first(self):
        with self.assertRaisesRegex(ValueError, "not the pinned"):
            windows_signing.changes(self.unsigned, self.signed(replace={"conpty.dll": b"re-signed"}))


if __name__ == "__main__":
    unittest.main()
