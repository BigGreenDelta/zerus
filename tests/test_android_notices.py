"""Generate Android library notices from a synthetic Gradle report and cache."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/android-notices.py"
REPORT = """
releaseRuntimeClasspath - Runtime classpath of '/release'.
+--- androidx.compose:compose-bom:2025.06.01
|    \\--- androidx.compose.ui:ui:1.8.3 (c)
+--- example.lib:core:1.0 -> 1.1
|    \\--- example.lib:extra:2.0
+--- example.google:services:3.0
|    \\--- example.lib:core:1.1 (*)
\\--- androidx.camera:camera-video:1.6.2 -> project :camera-video-fixed
"""
APACHE = ("<licenses><license><name>The Apache Software License, Version 2.0</name>"
          "<url>http://www.apache.org/licenses/LICENSE-2.0.txt</url></license></licenses>")
SDK = "<licenses><license><name>Android Software Development Kit License</name></license></licenses>"


def pom(licenses):
    return f'<project xmlns="http://maven.apache.org/POM/4.0.0">{licenses}</project>'


class AndroidNoticesTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="zerus-android-notices-")
        self.directory = Path(self.temporary.name)
        self.cache = self.directory / "gradle/caches/modules-2/files-2.1"
        self.assets = self.directory / "assets"
        self.assets.mkdir()
        self.publish("androidx.compose", "compose-bom", "2025.06.01")
        self.publish("example.lib", "core", "1.1", APACHE, {"META-INF/NOTICE": "Example core notice."})
        self.publish("example.lib", "extra", "2.0", APACHE)
        texts = "Kotlin text.\nApache text."
        index = {"Kotlin": {"start": 0, "length": 12}, "Guava": {"start": 13, "length": 12},
                 "Dagger": {"start": 13, "length": 12}}
        self.publish("example.google", "services", "3.0", SDK,
                     {"third_party_licenses.json": json.dumps(index), "third_party_licenses.txt": texts}, ".aar")
        (self.directory / "deps.txt").write_text(REPORT)

    def tearDown(self):
        self.temporary.cleanup()

    def publish(self, group, artifact, version, licenses="", entries=None, suffix=".jar"):
        folder = self.cache / group / artifact / version / "0123abcd"
        folder.mkdir(parents=True)
        (folder / f"{artifact}-{version}.pom").write_text(pom(licenses))
        if entries is not None or licenses:
            with zipfile.ZipFile(folder / f"{artifact}-{version}{suffix}", "w") as archive:
                for name, content in (entries or {"Example.class": "class"}).items():
                    archive.writestr(name, content)

    def run_script(self, *arguments):
        environment = {**os.environ, "GRADLE_USER_HOME": str(self.directory / "gradle")}
        return subprocess.run([sys.executable, "-I", str(SCRIPT), str(self.directory / "deps.txt"), "--offline",
                               "--assets", str(self.assets), *arguments],
                              capture_output=True, text=True, env=environment)

    def test_generates_inventory_notices_and_embedded_google_notices(self):
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("3 libraries and 1 platforms", result.stdout)
        self.assertEqual((self.assets / "libraries.tsv").read_text().splitlines()[1:], [
            "androidx.compose:compose-bom:2025.06.01\tplatform",
            "example.google:services:3.0\tLicenseRef-Android-SDK",
            "example.lib:core:1.1\tApache-2.0",
            "example.lib:extra:2.0\tApache-2.0",
        ])
        # Identical embedded texts share one section that names every component.
        self.assertEqual((self.assets / "google-notices.txt").read_text(),
                         "### Dagger, Guava\n\nApache text.\n\n### Kotlin\n\nKotlin text.\n\n")
        self.assertEqual((self.assets / "library-notices.txt").read_text(),
                         "### example.lib:core (META-INF/NOTICE)\n\nExample core notice.\n\n")
        check = self.run_script("--check")
        self.assertEqual(check.returncode, 0, check.stderr)

    def test_check_reports_assets_that_no_longer_match(self):
        self.assertEqual(self.run_script().returncode, 0)
        with (self.assets / "libraries.tsv").open("a") as stream:
            stream.write("example.lib:removed:1.0\tApache-2.0\n")
        result = self.run_script("--check")
        self.assertEqual(result.returncode, 1)
        self.assertIn("out of date: libraries.tsv", result.stderr)
        self.assertNotIn("removed", self.run_script().stdout)
        self.assertNotIn("removed", (self.assets / "libraries.tsv").read_text())

    def test_unreviewed_license_or_missing_text_stops_generation(self):
        mit = "<licenses><license><name>MIT License</name></license></licenses>"
        (next((self.cache / "example.lib/extra/2.0").glob("*/extra-2.0.pom"))).write_text(pom(mit))
        result = self.run_script()
        self.assertEqual(result.returncode, 1)
        self.assertIn("Add the reviewed MIT text of example.lib:extra to PROJECT_TEXTS", result.stderr)
        unknown = "<licenses><license><name>Custom Terms</name></license></licenses>"
        (next((self.cache / "example.lib/extra/2.0").glob("*/extra-2.0.pom"))).write_text(pom(unknown))
        result = self.run_script()
        self.assertIn("Review the license 'Custom Terms' of example.lib:extra:2.0", result.stderr)
        self.assertFalse((self.assets / "libraries.tsv").exists())

    def test_a_report_of_an_unresolved_configuration_is_refused(self):
        (self.directory / "deps.txt").write_text(REPORT + "+--- example.lib:declared-only (n)\n")
        result = self.run_script()
        self.assertEqual(result.returncode, 1)
        self.assertIn("lists unresolved dependencies", result.stderr)

    def test_offline_generation_needs_every_pom(self):
        for path in (self.cache / "example.lib/extra/2.0").glob("*/*.pom"):
            path.unlink()
        result = self.run_script()
        self.assertEqual(result.returncode, 1)
        self.assertIn("No POM for example.lib:extra:2.0 in the Gradle cache.", result.stderr)

    def test_a_notice_line_that_would_split_a_section_is_refused(self):
        folder = next((self.cache / "example.lib/core/1.1").glob("*"))
        with zipfile.ZipFile(folder / "core-1.1.jar", "w") as archive:
            archive.writestr("META-INF/NOTICE", "Intro\n### Not a section\n")
        result = self.run_script()
        self.assertEqual(result.returncode, 1)
        self.assertIn("starts a section", result.stderr)

    def test_checked_in_assets_cover_every_reviewed_text(self):
        assets = ROOT / "mobile/android/app/src/main/assets/licenses"
        rows = [line.split("\t") for line in (assets / "libraries.tsv").read_text().splitlines()[1:]]
        self.assertGreater(len(rows), 100)
        for fields in rows:
            if len(fields) == 3:
                self.assertTrue((assets / fields[2]).is_file(), fields)
        ui = (ROOT / "mobile/android/app/src/main/java/app/zerus/mobile/ThirdPartyUi.kt").read_text()
        for reference in ('"libraries.tsv"', '"library-notices.txt"', '"google-notices.txt"', '"licenses/Apache-2.0.txt"'):
            self.assertIn(reference, ui)


if __name__ == "__main__":
    unittest.main()
