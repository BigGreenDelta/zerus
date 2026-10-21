"""Generate Android library notices from a synthetic resolved classpath and Gradle cache."""
import json
import os
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/android-notices.py"
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
        self.components = []
        self.publish("androidx.compose:compose-bom:2025.06.01", category="platform")
        self.publish("example.lib:core:1.1", APACHE, {"META-INF/NOTICE": "Example core notice."})
        self.publish("example.lib:extra:2.0", APACHE, {"Extra.class": "class"})
        texts = "Kotlin text.\nApache text."
        index = {"Kotlin": {"start": 0, "length": 12}, "Guava": {"start": 13, "length": 12},
                 "Dagger": {"start": 13, "length": 12}}
        self.publish("example.google:services:3.0", SDK,
                     {"third_party_licenses.json": json.dumps(index), "third_party_licenses.txt": texts}, ".aar")
        # A multiplatform module whose Android variant lives in another component.
        self.publish("example.kmp:umbrella:4.0", APACHE, redirects=["example.kmp:umbrella-android:4.0"])
        self.publish("example.kmp:umbrella-android:4.0", APACHE, {"Umbrella.class": "class"})
        # A compatibility module whose metadata publishes no file at all.
        self.publish("example.kmp:common:4.0", APACHE, module={"variants": [{"name": "common", "files": []}]})
        self.write_resolved()

    def tearDown(self):
        self.temporary.cleanup()

    def folder(self, identifier):
        group, artifact, version = identifier.split(":")
        return self.cache / group / artifact / version / "0123abcd"

    def publish(self, identifier, licenses="", entries=None, suffix=".jar", category="library", redirects=(),
                module=None):
        group, artifact, version = identifier.split(":")
        folder = self.folder(identifier)
        folder.mkdir(parents=True)
        (folder / f"{artifact}-{version}.pom").write_text(pom(licenses))
        if module is not None:
            (folder / f"{artifact}-{version}.module").write_text(json.dumps(module))
        files = []
        if entries is not None:
            archive = folder / f"{artifact}-{version}{suffix}"
            with zipfile.ZipFile(archive, "w") as bundle:
                for name, content in entries.items():
                    bundle.writestr(name, content)
            files.append(str(archive))
        self.components.append({"id": identifier, "categories": [category], "redirects": list(redirects),
                                "files": files})

    def write_resolved(self, **changes):
        resolved = {"configuration": "releaseRuntimeClasspath", "projects": ["project :camera-video-fixed"],
                    "components": self.components, **changes}
        (self.directory / "resolved.json").write_text(json.dumps(resolved))

    def run_script(self, *arguments):
        environment = {**os.environ, "GRADLE_USER_HOME": str(self.directory / "gradle")}
        return subprocess.run([sys.executable, "-I", str(SCRIPT), str(self.directory / "resolved.json"), "--offline",
                               "--assets", str(self.assets), *arguments],
                              capture_output=True, text=True, env=environment)

    def assets_now(self):
        return {path.name: path.read_bytes() for path in sorted(self.assets.iterdir())}

    def assert_refused(self, message):
        """Generation stops with the message and leaves every earlier asset as it was."""
        before = self.assets_now()
        result = self.run_script()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn(message, result.stderr)
        self.assertEqual(self.assets_now(), before)

    def component(self, identifier):
        return next(item for item in self.components if item["id"] == identifier)

    def test_generates_inventory_notices_and_embedded_google_notices(self):
        result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("6 libraries and 1 platforms", result.stdout)
        self.assertEqual((self.assets / "libraries.tsv").read_text().splitlines()[1:], [
            "androidx.compose:compose-bom:2025.06.01\tplatform",
            "example.google:services:3.0\tLicenseRef-Android-SDK",
            "example.kmp:common:4.0\tApache-2.0",
            "example.kmp:umbrella:4.0\tApache-2.0",
            "example.kmp:umbrella-android:4.0\tApache-2.0",
            "example.lib:core:1.1\tApache-2.0",
            "example.lib:extra:2.0\tApache-2.0",
        ])
        # Identical embedded texts share one section that names every component.
        self.assertEqual((self.assets / "google-notices.txt").read_text(),
                         "### Dagger, Guava\n\nApache text.\n\n### Kotlin\n\nKotlin text.\n\n")
        self.assertEqual((self.assets / "library-notices.txt").read_text(),
                         "### example.lib:core (META-INF/NOTICE)\n\nExample core notice.\n\n")
        self.assertEqual(sorted(self.assets_now()), ["google-notices.txt", "libraries.tsv", "library-notices.txt"])
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

    def test_check_reports_a_removed_transitive_library(self):
        self.assertEqual(self.run_script().returncode, 0)
        self.components.remove(self.component("example.lib:extra:2.0"))
        self.write_resolved()
        result = self.run_script("--check")
        self.assertEqual(result.returncode, 1)
        self.assertIn("out of date: libraries.tsv", result.stderr)

    def test_a_shipped_library_without_its_archive_stops_generation(self):
        # Every archive gone while the POM files remain: nothing may be written.
        self.assertEqual(self.run_script().returncode, 0)
        removed = [path for path in self.cache.rglob("*") if path.suffix in (".jar", ".aar")]
        self.assertEqual(len(removed), 4)
        for path in removed:
            path.unlink()
        self.assert_refused("No artifact")
        check = self.run_script("--check")
        self.assertEqual(check.returncode, 1)
        self.assertIn("No artifact", check.stderr)

    def test_a_library_must_ship_files_redirect_or_publish_no_file(self):
        self.assertEqual(self.run_script().returncode, 0)
        self.component("example.lib:extra:2.0")["files"] = []
        self.write_resolved()
        self.assert_refused("No artifact of example.lib:extra:2.0: it neither ships files nor redirects")
        self.component("example.lib:extra:2.0")["redirects"] = ["example.lib:absent:2.0"]
        self.write_resolved()
        self.assert_refused("No artifact of example.lib:extra:2.0")
        # A redirect target must itself carry the files.
        self.component("example.lib:extra:2.0")["redirects"] = ["example.kmp:umbrella:4.0"]
        self.write_resolved()
        self.assert_refused("No artifact of example.lib:extra:2.0")
        # Metadata that publishes files elsewhere is not a metadata-only module.
        (self.folder("example.lib:extra:2.0") / "extra-2.0.module").write_text(
            json.dumps({"variants": [{"name": "jvm", "files": [{"name": "extra-2.0.jar"}]}]}))
        self.component("example.lib:extra:2.0")["redirects"] = []
        self.write_resolved()
        self.assert_refused("No artifact of example.lib:extra:2.0")

    def test_inconsistent_resolution_is_refused(self):
        self.assertEqual(self.run_script().returncode, 0)
        for changes, message in (
                ({"configuration": "debugRuntimeClasspath"}, "Resolve releaseRuntimeClasspath"),
                ({"projects": ["project :camera-video-fixed", "project :other"]}, "local modules ['project :other']"),
                ({"components": []}, "lists no components"),
                ({"components": self.components + [self.components[1]]}, "Malformed or repeated"),
                ({"components": self.components + [{"id": "a:b", "categories": ["library"]}]}, "Malformed"),
                ({"components": [{**self.components[0], "files": self.components[1]["files"]}]}, "ships files"),
                ({"components": [{**self.components[1], "categories": ["library", "platform"]}]}, "neither only")):
            with self.subTest(message=message):
                self.write_resolved(**changes)
                self.assert_refused(message)
        (self.directory / "resolved.json").write_text("{")
        self.assert_refused("Read the resolved release classpath first")

    def test_staging_failure_preserves_existing_assets(self):
        self.assertEqual(self.run_script().returncode, 0)
        before = self.assets_now()
        self.components.remove(self.component("example.lib:core:1.1"))
        self.write_resolved()
        # Two assets change, but no staged file can be created next to them.
        self.assets.chmod(0o555)
        try:
            result = self.run_script()
        finally:
            self.assets.chmod(0o755)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.assets_now(), before)
        result = self.run_script()
        self.assertIn("updated libraries.tsv, library-notices.txt", result.stdout)
        self.assertEqual(sorted(self.assets_now()), sorted(before))

    def test_failed_staged_write_removes_its_temporary_file(self):
        replace = runpy.run_path(str(SCRIPT))["replace"]
        (self.assets / "libraries.tsv").write_text("old")
        before = self.assets_now()
        fdopen = os.fdopen

        class FailedWrite:
            def __init__(self, stream):
                self.stream = stream

            def __enter__(self):
                return self

            def __exit__(self, *_):
                self.stream.close()

            def write(self, _):
                raise OSError("injected staged write failure")

        with mock.patch.object(os, "fdopen", side_effect=lambda *a, **k: FailedWrite(fdopen(*a, **k))):
            with self.assertRaisesRegex(OSError, "staged write failure"):
                replace(self.assets, {"libraries.tsv": "new"})
        self.assertEqual(self.assets_now(), before)

    def test_replacement_failure_is_reported_and_cleans_staging(self):
        replace = runpy.run_path(str(SCRIPT))["replace"]
        outputs = {"libraries.tsv": "new list", "google-notices.txt": "new notice"}
        for name in outputs:
            (self.assets / name).write_text("old")
        rename = os.replace

        def fail_second(source, destination):
            if Path(destination).name == "google-notices.txt":
                raise OSError("injected second replacement failure")
            return rename(source, destination)

        with mock.patch.object(os, "replace", side_effect=fail_second):
            with self.assertRaisesRegex(OSError, "second replacement failure"):
                replace(self.assets, outputs)
        self.assertEqual(self.assets_now(), {"google-notices.txt": b"old", "libraries.tsv": b"new list"})

    def test_unreviewed_license_or_missing_text_stops_generation(self):
        mit = "<licenses><license><name>MIT License</name></license></licenses>"
        (self.folder("example.lib:extra:2.0") / "extra-2.0.pom").write_text(pom(mit))
        self.assert_refused("Add the reviewed MIT text of example.lib:extra to PROJECT_TEXTS")
        unknown = "<licenses><license><name>Custom Terms</name></license></licenses>"
        (self.folder("example.lib:extra:2.0") / "extra-2.0.pom").write_text(pom(unknown))
        self.assert_refused("Review the license 'Custom Terms' of example.lib:extra:2.0")
        self.assertFalse((self.assets / "libraries.tsv").exists())

    def test_offline_generation_needs_every_pom(self):
        (self.folder("example.lib:extra:2.0") / "extra-2.0.pom").unlink()
        self.assert_refused("No POM for example.lib:extra:2.0 in the Gradle cache.")

    def test_a_notice_line_that_would_split_a_section_is_refused(self):
        with zipfile.ZipFile(self.component("example.lib:core:1.1")["files"][0], "w") as archive:
            archive.writestr("META-INF/NOTICE", "Intro\n### Not a section\n")
        self.assert_refused("starts a section")

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
        self.assertFalse([path.name for path in assets.iterdir() if path.name.startswith(".")])


if __name__ == "__main__":
    unittest.main()
