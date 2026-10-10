"""The macOS bundle keeps its hgs-tray path while system surfaces show Zerus."""
import os
from pathlib import Path
import plistlib
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
PACKAGING = ROOT / "tray/packaging"
NAME = "Zerus"


def strings(path):
    return dict(re.findall(r'^"([^"]+)" = "([^"]*)";$', path.read_text(encoding="utf-8"), re.M))


def installed_folder():
    install = (ROOT / "tray/install.sh").read_text()
    return re.search(r'^APP_DEST="\$\{HOME\}/Applications/(.+)\.app"$', install, re.M).group(1)


class MacosBundleName(unittest.TestCase):
    def check_names(self, info_plist, localized_strings):
        info = plistlib.loads(info_plist.read_bytes())
        # Launch Services shows the localized name only while the unlocalized
        # display name equals the bundle folder name; otherwise it shows the folder.
        self.assertEqual(info["CFBundleDisplayName"], installed_folder())
        self.assertIs(info["LSHasLocalizedDisplayName"], True)
        self.assertEqual(info["CFBundleDevelopmentRegion"], "en")
        self.assertEqual(info["CFBundleName"], NAME)
        self.assertEqual(strings(localized_strings), {"CFBundleDisplayName": NAME, "CFBundleName": NAME})
        return info

    def test_sources_localize_the_compatible_bundle(self):
        info = self.check_names(PACKAGING / "Info.plist", PACKAGING / "en.lproj/InfoPlist.strings")
        # The executable must not depend on the bundle folder name.
        self.assertEqual(info["CFBundleExecutable"], "${MACOSX_BUNDLE_EXECUTABLE_NAME}")
        self.assertEqual(info["CFBundleIdentifier"], "com.hgdev.hgs-tray")

    @unittest.skipUnless(os.environ.get("ZERUS_MACOS_BUNDLE"), "needs a built macOS bundle")
    def test_built_bundle_carries_the_localized_name(self):
        contents = Path(os.environ["ZERUS_MACOS_BUNDLE"]) / "Contents"
        info = self.check_names(contents / "Info.plist", contents / "Resources/en.lproj/InfoPlist.strings")
        self.assertEqual(info["CFBundleExecutable"], "hgs-tray")
        self.assertTrue((contents / "MacOS/hgs-tray").is_file())


if __name__ == "__main__":
    unittest.main()
