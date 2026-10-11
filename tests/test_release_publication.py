"""Credential-free contracts for release provenance and partial-publication retries."""
import copy
import importlib.util
import io
import json
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import release_common as common


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


fetch = load("fetch_candidate", "fetch-release-candidate.py")
publish = load("publish_candidate", "publish-release.py")
VERSION = "0.37.0"
COMMIT = "a" * 40


class PublicationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="zerus-release-contract-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.info = {"schema": 1, "version": VERSION, "source_commit": COMMIT,
                     "architecture": "x86_64", "distribution": "Arch Linux",
                     "publication": "candidate", "native_agents_bundled": False,
                     "aur_packages": list(common.PACKAGES),
                     "binary_dependencies": {name: "1.2.3" for name in common.BINARY_LIBRARIES}}

    def candidate(self):
        directory = self.directory / "candidate"
        directory.mkdir()
        for suffix in ("source", "arch-x86_64"):
            (directory / f"zerus-{VERSION}-{suffix}.tar.gz").write_bytes(suffix.encode())
        for package in ("zerus", "zerus-ade-bin"):
            (directory / f"{package}-{VERSION}-1-x86_64.pkg.tar.zst").write_bytes(package.encode())
        (directory / "release-info.json").write_text(json.dumps(self.info))
        for package in common.PACKAGES:
            recipe_dir = directory / "aur" / package
            recipe_dir.mkdir(parents=True)
            vcs = package == "zerus-git"
            if vcs:
                source = "zerus::git+https://github.com/ufna/zerus.git#branch=main"
                digest = "SKIP"
            else:
                suffix = "source" if package == "zerus" else "arch-x86_64"
                source = f"https://github.com/{common.REPOSITORY}/releases/download/v{VERSION}/zerus-{VERSION}-{suffix}.tar.gz"
                digest = common.sha256(directory / f"zerus-{VERSION}-{suffix}.tar.gz")
            (recipe_dir / "PKGBUILD").write_text(common.MAINTAINER + "\npkgname=" + package + "\n")
            (recipe_dir / ".SRCINFO").write_text(f"pkgbase = {package}\n\tpkgver = {VERSION}\n\tpkgrel = 1\n\tarch = x86_64\n\tsource = {source}\n\tsha256sums = {digest}\npkgname = {package}\n")
            if package == "zerus-ade-bin":
                with (recipe_dir / ".SRCINFO").open("a") as output:
                    output.write("".join(f"\tdepends = {name}>=1.2.3\n" for name in common.BINARY_LIBRARIES))
        common.finish_candidate(directory)
        return directory

    def test_sealed_candidate_validates_exact_version_and_commit(self):
        directory = self.candidate()
        info, hashes = common.validate_candidate(directory, VERSION, COMMIT)
        self.assertEqual(info["source_commit"], COMMIT)
        self.assertEqual(len(hashes), 6)
        with self.assertRaisesRegex(ValueError, "another commit"):
            common.validate_candidate(directory, VERSION, "b" * 40)

    def test_changed_binary_fails_before_publication(self):
        directory = self.candidate()
        (directory / f"zerus-{VERSION}-arch-x86_64.tar.gz").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "Checksum mismatch"):
            common.validate_candidate(directory, VERSION)

    def test_recipe_edits_must_match_sealed_aur_archive(self):
        directory = self.candidate()
        (directory / "aur/zerus/PKGBUILD").write_text("changed")
        with self.assertRaisesRegex(ValueError, "sealed archive"):
            common.validate_candidate(directory, VERSION)

    def test_extra_file_and_unsafe_checksum_are_rejected(self):
        directory = self.candidate()
        (directory / "unreviewed.txt").write_text("unsealed")
        with self.assertRaisesRegex(ValueError, "Unsealed"):
            common.validate_candidate(directory, VERSION)
        (directory / "unreviewed.txt").unlink()
        with (directory / "SHA256SUMS").open("a") as output:
            output.write("0" * 64 + "  ../outside\n")
        with self.assertRaisesRegex(ValueError, "Invalid checksum"):
            common.validate_candidate(directory, VERSION)

    def test_stable_candidate_has_no_windows_zip_even_when_sealed(self):
        # finish_candidate seals nightly zips; a stable release keeps its Arch-only family.
        directory = self.candidate()
        self.assertEqual(len(common.validate_candidate(directory, VERSION)[1]), 6)
        (directory / f"zerus-{VERSION}-windows-x86_64.zip").write_bytes(b"portable")
        common.finish_candidate(directory)
        self.assertIn(f"zerus-{VERSION}-windows-x86_64.zip", (directory / "SHA256SUMS").read_text())
        with self.assertRaisesRegex(ValueError, "Unexpected/missing release assets"):
            common.validate_candidate(directory, VERSION)

    def test_wrong_package_architecture_fails(self):
        directory = self.candidate()
        path = directory / "aur/zerus/.SRCINFO"
        path.write_text(path.read_text().replace("arch = x86_64", "arch = aarch64"))
        common.finish_candidate(directory)
        with self.assertRaisesRegex(ValueError, "Wrong package metadata"):
            common.validate_candidate(directory, VERSION)

    def test_vcs_version_only_updates_are_not_pushed(self):
        old = "pkgver=0.36.2.r8.gabcd\ndepends=('python')\n"
        new = old.replace("0.36.2.r8.gabcd", "0.37.0.r20.g1234")
        self.assertFalse(common.recipe_changed(old, new, vcs=True))
        self.assertTrue(common.recipe_changed(old, new, vcs=False))
        self.assertTrue(common.recipe_changed(old, new.replace("python", "python bash"), vcs=True))
        old = "\tpkgver = 0.36.2.r8.gabcd\n\tprovides = zerus=0.36.2.r8.gabcd\n"
        self.assertFalse(common.recipe_changed(old, old.replace("0.36.2.r8.gabcd", "0.37.0.r20.g1234"), vcs=True))

    def test_version_comparison_prevents_downgrades(self):
        def version(text, release="1"):
            return common.package_version({"pkgver": [text], "pkgrel": [release]})
        self.assertGreater(version("0.37.0"), version("0.36.2.r99.gabcd"))
        self.assertGreater(version("0.37.0.r20.gabcd"), version("0.37.0.r19.g1234"))
        self.assertGreater(version("0.37.0", "2"), version("0.37.0"))

    def test_aur_retry_preserves_refreshed_descriptions_without_writes(self):
        directory = self.candidate()
        previous, current = common.DESCRIPTION_UPDATES[0]
        for package in common.PACKAGES:
            source = directory / "aur" / package
            for name, line in (("PKGBUILD", f"pkgdesc='{previous}'\n"),
                               (".SRCINFO", f"\tpkgdesc = {previous}\n")):
                path = source / name
                path.write_text(path.read_text() + line)

        def clone(args, **kwargs):
            destination = Path(args[-1])
            destination.mkdir(parents=True)
            source = directory / "aur" / destination.name
            for name in ("PKGBUILD", ".SRCINFO"):
                text = (source / name).read_text().replace(previous, current)
                if destination.name == "zerus-git":
                    text = text.replace(VERSION, "0.36.2.r8.gabcd")
                (destination / name).write_text(text)

        def git(directory, *args, **kwargs):
            self.assertEqual(args, ("ls-files",), "A retry must not write AUR refs or files")
            return "PKGBUILD\n.SRCINFO"

        with patch.object(publish.subprocess, "check_output", return_value=" ".join(common.PACKAGES)), \
                patch.object(publish.subprocess, "run", side_effect=clone), \
                patch.object(publish, "git", side_effect=git), \
                patch.object(publish.urllib.request, "urlopen", return_value=io.StringIO('{"results": []}')):
            self.assertEqual(publish.prepare_aur(directory, self.directory / "retry", [], {}), [])
        for package in common.PACKAGES:
            self.assertIn(current, (self.directory / "retry" / package / "PKGBUILD").read_text())

    def test_description_retry_exception_does_not_hide_recipe_or_version_changes(self):
        directory = self.candidate()
        package = "zerus-ade-bin"
        source = directory / "aur" / package
        previous, current = common.DESCRIPTION_UPDATES[0]
        for name, line in (("PKGBUILD", f"pkgdesc='{previous}'\n"),
                           (".SRCINFO", f"\tpkgdesc = {previous}\n")):
            path = source / name
            path.write_text(path.read_text() + line)
        cases = (("PKGBUILD", "\nunreviewed_command\n"),
                 (".SRCINFO", "\tdepends = unreviewed-library\n"),
                 (".SRCINFO", "\tsha256sums = " + "f" * 64 + "\n"),
                 (".SRCINFO", "\tpkgver = 0.38.0\n"))
        for index, (changed_file, addition) in enumerate(cases):
            def clone(args, **kwargs):
                destination = Path(args[-1])
                destination.mkdir(parents=True)
                for name in ("PKGBUILD", ".SRCINFO"):
                    text = (source / name).read_text().replace(previous, current)
                    if name == changed_file:
                        if "pkgver =" in addition:
                            text = text.replace("pkgver = " + VERSION, "pkgver = 0.38.0")
                        else:
                            text += addition
                    (destination / name).write_text(text)
            with self.subTest(changed_file=changed_file, addition=addition), \
                    patch.object(publish.subprocess, "check_output", return_value=package), \
                    patch.object(publish.subprocess, "run", side_effect=clone), \
                    patch.object(publish, "git", return_value="PKGBUILD\n.SRCINFO") as git, \
                    patch.object(publish.urllib.request, "urlopen", return_value=io.StringIO('{"results": []}')):
                with self.assertRaisesRegex(ValueError, "pkgrel/version bump|downgrade"):
                    publish.prepare_aur(directory, self.directory / f"retry-{index}", [], {}, (package,))
                self.assertEqual(git.call_count, 1)

    def test_binary_library_floors_come_from_buildinfo(self):
        text = "".join(f"installed = {name}-1:6.11.2-3\n" for name in common.BINARY_LIBRARIES)
        self.assertEqual(common.binary_dependencies(text), {name: "1:6.11.2" for name in common.BINARY_LIBRARIES})
        current = text.replace("-3\n", "-3-x86_64\n")
        self.assertEqual(common.binary_dependencies(current), {name: "1:6.11.2" for name in common.BINARY_LIBRARIES})
        with self.assertRaisesRegex(ValueError, "Missing/ambiguous"):
            common.binary_dependencies(text + "installed = qt6-base-6.11.2-4\n")
        with self.assertRaisesRegex(ValueError, "Missing/ambiguous"):
            common.binary_dependencies("")

    def test_binary_recipe_must_declare_library_version_floors(self):
        directory = self.candidate()
        path = directory / "aur/zerus-ade-bin/.SRCINFO"
        path.write_text(path.read_text().replace("depends = qt6-base>=1.2.3", "depends = qt6-base"))
        common.finish_candidate(directory)
        with self.assertRaisesRegex(ValueError, "version floors"):
            common.validate_candidate(directory, VERSION)

    def test_only_successful_upstream_main_manual_candidate_is_accepted(self):
        run = {"path": ".github/workflows/release.yml", "event": "workflow_dispatch",
               "head_branch": "main", "head_repository": {"full_name": common.REPOSITORY},
               "status": "completed", "conclusion": "success", "head_sha": COMMIT}
        self.assertEqual(fetch.validate_run(run), COMMIT)
        for field, value in (("event", "pull_request"), ("path", ".github/workflows/ci.yml"),
                             ("head_branch", "feature"), ("conclusion", "failure"),
                             ("head_repository", {"full_name": "example/fork"})):
            with self.subTest(field=field), self.assertRaises(ValueError):
                fetch.validate_run({**run, field: value})

    def test_zip_traversal_and_symlinks_fail_before_extraction(self):
        for name, attributes in (("../escape", 0), ("link", (stat.S_IFLNK | 0o777) << 16)):
            with self.subTest(name=name):
                path = self.directory / "bad.zip"
                with zipfile.ZipFile(path, "w") as output:
                    entry = zipfile.ZipInfo(name)
                    entry.external_attr = attributes
                    output.writestr(entry, "target")
                destination = self.directory / "extracted"
                with self.assertRaises(ValueError):
                    fetch.unpack(path, destination)
                self.assertFalse(destination.exists())

    def test_existing_tag_must_match_exact_candidate(self):
        def response(value):
            return subprocess.CompletedProcess([], 0, json.dumps({"object": value}), "")
        with patch.object(publish.subprocess, "run", return_value=response({"type": "commit", "sha": "b" * 40})):
            with self.assertRaisesRegex(ValueError, "different commit"):
                publish.verify_tag("v" + VERSION, COMMIT)
        with patch.object(publish.subprocess, "run", return_value=response({"type": "tag", "sha": "c" * 40})), \
                patch.object(publish, "api", return_value={"object": {"type": "commit", "sha": COMMIT}}):
            self.assertTrue(publish.verify_tag("v" + VERSION, COMMIT))

    def test_unpublished_draft_is_found_in_authenticated_paginated_listing(self):
        draft = {"id": 42, "tag_name": "v" + VERSION, "draft": True,
                 "target_commitish": COMMIT, "assets": []}
        missing = subprocess.CompletedProcess([], 1, "", "gh: HTTP 404")
        pages = [[{"tag_name": "v0.36.2", "draft": False}], [draft]]
        with patch.object(publish.subprocess, "run", return_value=missing), \
                patch.object(publish, "gh", return_value=json.dumps(pages)) as listing:
            self.assertEqual(publish.release_for_tag(draft["tag_name"]), draft)
            listing.assert_called_once_with("api", "--paginate", "--slurp",
                                            f"repos/{common.REPOSITORY}/releases?per_page=100")
        with patch.object(publish.subprocess, "run", return_value=missing), \
                patch.object(publish, "gh", return_value=json.dumps([[]])):
            self.assertIsNone(publish.release_for_tag(draft["tag_name"]))
        with patch.object(publish.subprocess, "run", return_value=missing), \
                patch.object(publish, "gh", return_value=json.dumps([[draft], [draft]])):
            with self.assertRaisesRegex(ValueError, "Multiple releases"):
                publish.release_for_tag(draft["tag_name"])

    def test_release_lookup_errors_cannot_be_treated_as_absence(self):
        forbidden = subprocess.CompletedProcess([], 1, "", "gh: HTTP 403")
        with patch.object(publish.subprocess, "run", return_value=forbidden), \
                patch.object(publish, "gh") as listing:
            with self.assertRaisesRegex(ValueError, "Cannot inspect"):
                publish.release_for_tag("v" + VERSION)
            listing.assert_not_called()

    def test_retry_finishes_existing_draft_then_leaves_public_assets_unchanged(self):
        directory = self.candidate()
        info, sums = common.validate_candidate(directory, VERSION)
        hashes = {**sums, "SHA256SUMS": common.sha256(directory / "SHA256SUMS")}
        release = {"draft": True, "tag_name": "v" + VERSION, "target_commitish": COMMIT, "assets": []}
        first = next(iter(hashes))
        def asset(name):
            return {"name": name, "digest": "sha256:" + hashes[name], "size": (directory / name).stat().st_size}
        release["assets"].append(asset(first))
        commands = []
        def gh(*args):
            commands.append(args)
            if args[1] == "upload": release["assets"].append(asset(Path(args[3]).name))
            elif args[1] == "edit": release["draft"] = False
            else: self.fail("Unexpected GitHub mutation: " + str(args))
        class Download(io.BytesIO):
            def geturl(self): return "https://release-assets.githubusercontent.com/verified"
        def download(url, **kwargs): return Download((directory / url.rsplit("/", 1)[1]).read_bytes())
        with patch.object(publish, "release_for_tag", side_effect=lambda _: copy.deepcopy(release)), \
                patch.object(publish, "verify_tag", return_value=True), \
                patch.object(publish, "gh", side_effect=gh), \
                patch.object(publish.urllib.request, "urlopen", side_effect=download):
            publish.publish_github(directory, info, sums)
            self.assertFalse(release["draft"])
            self.assertEqual(sum(args[1] == "upload" for args in commands), len(hashes) - 1)
            commands.clear()
            publish.publish_github(directory, info, sums)
            self.assertEqual(commands, [])

    def test_new_draft_visibility_delay_retries_reads_without_recreating(self):
        directory = self.candidate()
        info, sums = common.validate_candidate(directory, VERSION)
        hashes = {**sums, "SHA256SUMS": common.sha256(directory / "SHA256SUMS")}
        release = {"draft": True, "tag_name": "v" + VERSION,
                   "target_commitish": COMMIT, "assets": []}
        commands, lookups = [], []
        def lookup(tag):
            lookups.append(tag)
            # Initial absence, then two transient misses after successful create.
            return None if len(lookups) <= 3 else copy.deepcopy(release)
        def gh(*args):
            commands.append(args)
            if args[1] == "create":
                self.assertEqual(Path(args[args.index("--notes-file") + 1]).read_text().count(COMMIT), 1)
            elif args[1] == "upload":
                name = Path(args[3]).name
                release["assets"].append({"name": name, "digest": "sha256:" + hashes[name],
                                          "size": (directory / name).stat().st_size})
            elif args[1] == "edit":
                release["draft"] = False
            else:
                self.fail("Unexpected GitHub mutation: " + str(args))
        class Download(io.BytesIO):
            def geturl(self): return "https://release-assets.githubusercontent.com/verified"
        with patch.object(publish, "release_for_tag", side_effect=lookup), \
                patch.object(publish, "verify_tag", return_value=True), \
                patch.object(publish, "gh", side_effect=gh), \
                patch.object(publish.time, "sleep") as sleep, \
                patch.object(publish.urllib.request, "urlopen", side_effect=lambda url, **kw: Download((directory / url.rsplit("/", 1)[1]).read_bytes())):
            publish.publish_github(directory, info, sums)
            self.assertEqual([call.args[0] for call in sleep.call_args_list], [1, 2])
        self.assertEqual(sum(args[1] == "create" for args in commands), 1)
        self.assertEqual(sum(args[1] == "upload" for args in commands), len(hashes))
        self.assertFalse(release["draft"])

    def test_invisible_new_draft_times_out_without_uploading_or_recreating(self):
        directory = self.candidate()
        info, sums = common.validate_candidate(directory, VERSION)
        with patch.object(publish, "release_for_tag", return_value=None), \
                patch.object(publish, "verify_tag", return_value=False), \
                patch.object(publish, "gh") as mutations, patch.object(publish.time, "sleep") as sleep:
            with self.assertRaisesRegex(ValueError, "same candidate"):
                publish.publish_github(directory, info, sums)
        self.assertEqual(len(mutations.call_args_list), 1)
        self.assertEqual(mutations.call_args.args[:2], ("release", "create"))
        self.assertEqual(sum(call.args[0] for call in sleep.call_args_list), 60)

    def test_new_draft_lookup_api_errors_abort_without_retries(self):
        with patch.object(publish, "release_for_tag", side_effect=ValueError("Cannot inspect GitHub release: HTTP 403")) as lookup, \
                patch.object(publish.time, "sleep") as sleep:
            with self.assertRaisesRegex(ValueError, "HTTP 403"):
                publish.wait_for_created_draft("v" + VERSION)
        lookup.assert_called_once()
        sleep.assert_not_called()

    def test_changed_or_incomplete_public_release_is_never_overwritten(self):
        directory = self.candidate()
        info, sums = common.validate_candidate(directory, VERSION)
        release = {"draft": False, "tag_name": "v" + VERSION, "assets": []}
        with patch.object(publish, "release_for_tag", return_value=release), \
                patch.object(publish, "verify_tag", return_value=True), patch.object(publish, "gh") as mutations:
            with self.assertRaisesRegex(ValueError, "missing assets"):
                publish.publish_github(directory, info, sums)
            mutations.assert_not_called()
        release["assets"] = [{"name": name, "digest": "sha256:" + "0" * 64} for name in [*sums, "SHA256SUMS"]]
        with self.assertRaisesRegex(ValueError, "immutable asset differs"):
            publish.verify_assets(release, directory, {**sums, "SHA256SUMS": common.sha256(directory / "SHA256SUMS")})

    def test_draft_with_moving_target_is_rejected_before_upload(self):
        directory = self.candidate()
        info, sums = common.validate_candidate(directory, VERSION)
        with patch.object(publish, "release_for_tag", return_value={"draft": True, "target_commitish": "main"}), \
                patch.object(publish, "verify_tag", return_value=False), patch.object(publish, "gh") as mutations:
            with self.assertRaisesRegex(ValueError, "moving branch"):
                publish.publish_github(directory, info, sums)
            mutations.assert_not_called()

    def test_aur_failure_precedes_github_and_partial_push_is_retryable(self):
        directory = self.candidate()
        arguments = ["publish-release.py", "--candidate", str(directory), "--version", VERSION,
                     "--ssh-key", "/unused/key"]
        with patch.object(sys, "argv", arguments), \
                patch.object(publish, "ssh_environment", return_value=([], {})), \
                patch.object(publish, "prepare_aur", side_effect=ValueError("AUR authentication failed")), \
                patch.object(publish, "publish_github") as github:
            with self.assertRaisesRegex(ValueError, "authentication"):
                publish.main()
            github.assert_not_called()
        events = []
        def push(repo, *args, **kwargs):
            events.append(repo.name)
            if repo.name == "zerus-git": raise subprocess.CalledProcessError(1, "git push")
        plans = [(name, self.directory / name) for name in common.PACKAGES]
        with patch.object(sys, "argv", arguments), \
                patch.object(publish, "ssh_environment", return_value=([], {})), \
                patch.object(publish, "prepare_aur", return_value=plans), \
                patch.object(publish, "publish_github", side_effect=lambda *args: events.append("GitHub")), \
                patch.object(publish, "git", side_effect=push):
            with self.assertRaisesRegex(SystemExit, "same candidate"):
                publish.main()
        self.assertEqual(events, ["GitHub", *common.PACKAGES])


if __name__ == "__main__":
    unittest.main()
