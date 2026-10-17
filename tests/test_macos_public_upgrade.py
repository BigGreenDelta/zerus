"""Credential-free public cask qualification boundaries; no Homebrew side effects."""
import hashlib
import importlib.util
import json
from pathlib import Path
import stat
import tempfile
import types
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("macos_public_upgrade", ROOT / "scripts/ci/macos-public-upgrade.py")
upgrade = importlib.util.module_from_spec(spec)
spec.loader.exec_module(upgrade)


class PublicCaskBoundaries(unittest.TestCase):
    def cask(self, channel="stable", legacy=False):
        version = "0.44.0" if channel == "stable" else "0.44.0.r1558.gacbbee6.n19"
        text = upgrade.render_cask(version, "a" * 64, channel, "nightly-39002487144" if channel == "nightly" else None)
        if legacy:
            text = text.replace('        "com.hgdev.zerus-mobile-connector",\n', "").replace(
                '        "~/Library/LaunchAgents/com.hgdev.zerus-mobile-connector.plist",\n', "")
        return text

    def proof_inputs(self):
        cask = upgrade.accepted_cask(self.cask(), "stable")
        prefix = "https://github.com/ufna/zerus/releases/download/v0.44.0/"
        metadata = json.dumps({"version": "0.44.0", "source_commit": "b" * 40,
            "macos": {"asset": "zerus-0.44.0-macos-arm64.zip", "qt_version": "6.11.2",
            "team_id": upgrade.TEAM, "notarization_id": "11111111-1111-4111-8111-111111111111",
            "minimum_system_version": "14.0"}, "homebrew_casks": ["zerus"]}).encode()
        names = ["release-info.json", "SHA256SUMS", "zerus-0.44.0-macos-arm64.zip"]
        release = {"tag_name": "v0.44.0", "target_commitish": "b" * 40, "draft": False, "prerelease": False,
                   "assets": [{"name": name, "browser_download_url": prefix + name, "size": 12} for name in names]}
        sums = (hashlib.sha256(metadata).hexdigest() + "  release-info.json\n" + "a" * 64
                + "  zerus-0.44.0-macos-arm64.zip\n").encode()
        return cask, release, metadata, sums

    def test_full_commit_and_https_public_origins_only(self):
        self.assertEqual(upgrade.exact_commit("a" * 40), "a" * 40)
        for value in ["main", "a" * 7, "A" * 40, "a" * 40 + "\n"]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                upgrade.exact_commit(value)
        credential_url = "https://" + "fixture-user" + "@" + "github.com/asset"
        for value in ["http://github.com/ufna/zerus", "https://example.org/asset", "file:///tmp/asset",
                      credential_url, "https://github.com:8443/asset"]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                upgrade.safe_url(value)

    def test_only_literal_current_or_retained_casks_are_accepted(self):
        for channel in ("stable", "nightly"):
            for legacy in (False, True):
                text = self.cask(channel, legacy)
                self.assertEqual(upgrade.accepted_cask(text, channel)["channel"], channel)
                for injected in [text + "system 'touch /tmp/unreviewed'\n",
                                 text.replace('  app "Zerus.app"', '  uninstall quit: "com.hgdev.hgs-tray"'),
                                 text.replace("https://github.com/ufna/zerus/", "https://example.org/")]:
                    with self.subTest(channel=channel, legacy=legacy), self.assertRaises(ValueError):
                        upgrade.accepted_cask(injected, channel)

    def test_release_metadata_seal_source_and_asset_identity(self):
        args = self.proof_inputs()
        self.assertEqual(upgrade.release_proof(*args)["source_commit"], "b" * 40)
        for change in ({"target_commitish": "main"}, {"draft": True}, {"prerelease": True}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                upgrade.release_proof(args[0], dict(args[1], **change), *args[2:])
        bad_assets = [dict(row, browser_download_url="https://example.org/" + row["name"])
                      for row in args[1]["assets"]]
        with self.assertRaises(ValueError):
            upgrade.release_proof(args[0], dict(args[1], assets=bad_assets), *args[2:])
        with self.assertRaises(ValueError):
            upgrade.release_proof(*args[:2], args[2] + b" ", args[3])
        with self.assertRaises(ValueError):
            upgrade.release_proof(*args[:3], args[3].replace(b"a" * 64, b"c" * 64))

    def test_nightly_retains_product_and_release_versions_separately(self):
        cask, release, metadata, sums = self.proof_inputs()
        cask = upgrade.accepted_cask(self.cask("nightly"), "nightly")
        info = json.loads(metadata)
        info.update(version=cask["version"], product_version="0.44.0", homebrew_casks=["zerus@nightly"])
        info["macos"]["asset"] = "zerus-" + cask["version"] + "-macos-arm64.zip"
        metadata = json.dumps(info).encode()
        release.update(tag_name=cask["tag"], prerelease=True)
        prefix = "https://github.com/ufna/zerus/releases/download/" + cask["tag"] + "/"
        release["assets"] = [{"name": name, "browser_download_url": prefix + name, "size": 12}
                             for name in ["release-info.json", "SHA256SUMS", info["macos"]["asset"]]]
        sums = (hashlib.sha256(metadata).hexdigest() + "  release-info.json\n" + "a" * 64
                + "  " + info["macos"]["asset"] + "\n").encode()
        proof = upgrade.release_proof(cask, release, metadata, sums)
        self.assertEqual(proof["product_version"], "0.44.0")
        self.assertEqual(proof["version"], cask["version"])

    def test_archive_rejects_traversal_and_escaping_symlinks(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "app.zip"
            for name, target in [("../outside", None), ("Other.app/file", None),
                                 ("Zerus.app/link", "../../outside")]:
                with zipfile.ZipFile(path, "w") as archive:
                    member = zipfile.ZipInfo(name)
                    if target:
                        member.external_attr = (stat.S_IFLNK | 0o777) << 16
                    archive.writestr(member, target or "bytes")
                with self.subTest(name=name), self.assertRaises(ValueError):
                    upgrade.safe_archive(path)
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr("Zerus.app/Contents/Info.plist", "bytes")
                member = zipfile.ZipInfo("Zerus.app/Contents/Frameworks/QtCore.framework/Versions/Current")
                member.external_attr = (stat.S_IFLNK | 0o777) << 16
                archive.writestr(member, "A")
            upgrade.safe_archive(path)

    def test_owner_accounts_and_nonhosted_macs_are_refused(self):
        env = dict(GITHUB_ACTIONS="true", RUNNER_ENVIRONMENT="github-hosted", RUNNER_OS="macOS", RUNNER_TEMP="/tmp/fixture")
        with patch.object(upgrade.platform, "system", return_value="Darwin"), \
                patch.object(upgrade.platform, "machine", return_value="arm64"):
            with self.assertRaises(ValueError):
                upgrade.disposable_mac(env, Path("/Users") / "example")
            with self.assertRaises(ValueError):
                upgrade.disposable_mac(dict(env, RUNNER_ENVIRONMENT="self-hosted"), Path("/Users") / "runner")

    def test_install_native_signature_precedes_app_execution(self):
        with tempfile.TemporaryDirectory() as tmp:
            q = upgrade.Qualification(Path(tmp))
            q.env = {}
            calls = []
            q.select_tap = lambda _: calls.append("tap")
            q.native_signature = lambda *_: calls.append("signature")
            def run(args, **kwargs):
                calls.append(str(args[0]) + " " + str(args[1]))
                return json.dumps(dict(sourceCommit="b" * 40, sourceDirty=False)) if args[-1] == "--build-info" else ""
            q.run = run
            q.install(dict(cask="zerus", source_commit="b" * 40), upgrade=True)
            self.assertEqual(calls[:3], ["tap", "brew upgrade", "signature"])
            self.assertLess(calls.index("signature"), next(i for i, x in enumerate(calls) if "--build-info" in x))
            self.assertFalse(any("zap" in call or "bootout" in call for call in calls))

    def test_lifecycle_uses_real_upgrade_and_both_explicit_channel_switches(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            q = upgrade.Qualification(root)
            commands, installs, checkpoints = [], [], []
            q.run = lambda args, **kwargs: commands.append([str(x) for x in args]) or ""
            q.native_signature = lambda *_: None
            def install(proof, upgrade=False):
                installs.append((proof["version"], proof["cask"], upgrade))
                q.installed = proof["cask"]
            q.install = install
            q.open_native = lambda _: None
            q.seed_state = lambda: None
            q.start_gui = lambda _: setattr(q, "gui", types.SimpleNamespace(pid=123))
            q.stop_gui = lambda: setattr(q, "gui", None)
            q.checkpoint = lambda phase, old_gui_pid=None: checkpoints.append((phase, old_gui_pid))
            old = dict(version="0.43.0", cask="zerus", directory=str(root / "old"), asset="old.zip")
            new = dict(version="0.44.0", cask="zerus", directory=str(root / "new"), asset="new.zip")
            nightly = dict(version="0.44.0.r1.gabcdef0.n1", cask="zerus@nightly", directory=str(root / "nightly"), asset="nightly.zip")
            with patch.object(upgrade, "safe_archive"):
                q.lifecycle(old, new, nightly)
            self.assertEqual(installs, [("0.43.0", "zerus", False), ("0.44.0", "zerus", True),
                                       (nightly["version"], "zerus@nightly", False), ("0.44.0", "zerus", False)])
            self.assertEqual([x for x in commands if x[:3] == ["brew", "uninstall", "--cask"]],
                             [["brew", "uninstall", "--cask", "zerus"], ["brew", "uninstall", "--cask", "zerus@nightly"]])
            self.assertIn(("stable-version-upgrade", 123), checkpoints)
            self.assertIn(("nightly-to-stable-restart", None), checkpoints)


if __name__ == "__main__":
    unittest.main()
