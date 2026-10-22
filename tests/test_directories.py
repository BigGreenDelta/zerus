"""Real filesystem directory commands in a private home, without native agents."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import uuid

REPO = Path(__file__).resolve().parents[1]
HGS = Path(os.environ.get("HGS_TEST_BIN", REPO / "hgs")).resolve()


@unittest.skipIf(os.name == "nt", "local directory commands require enabled Windows sessions")
class Directories(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="zerus-directories-")
        self.root = Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("HGS_")}
        self.env.update(HOME=str(self.root), XDG_CONFIG_HOME=str(self.root / "config"),
                        XDG_STATE_HOME=str(self.root / "state"),
                        XDG_DATA_HOME=str(self.root / "data"))

    def hgs(self, *args, rc=0, input_=None):
        result = subprocess.run([str(HGS), *args], env=self.env, cwd=self.root,
                                input=input_, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, rc, result.stdout + result.stderr)
        return result

    def test_browse_missing_path_does_not_create(self):
        target = self.root / "new-project"
        self.hgs("dirs", str(target), rc=1)
        self.assertFalse(target.exists())
        self.assertEqual(json.loads(self.hgs("dirs").stdout)["path"], str(self.root.resolve()))

    def test_project_hint_reads_existing_exact_catalog_without_git_or_membership_write(self):
        self.env['HGS_CONFIG_DIR'] = str(self.root / 'catalog')
        identity = json.loads(self.hgs('swarm', 'hello').stdout)
        project = dict(id='work',name='Work',color='#123456',sessions=[],folders=[dict(
            id='folder',machine=identity['node_id'],path=str(self.root.resolve()),name='')])
        self.hgs('swarm','initialize',input_=json.dumps(dict(projects=[project])))
        catalog = self.root / 'catalog/swarm/catalog.json'
        before = catalog.read_bytes()
        tools = self.root / 'bin'; tools.mkdir()
        git = tools / 'git'
        git.write_text('#!/bin/sh\nprintf called > "'+str(self.root / 'git-called')+'"\nexit 1\n')
        git.chmod(0o700); self.env['PATH'] = str(tools) + os.pathsep + self.env.get('PATH','')
        observed = json.loads(self.hgs('dirs',str(self.root)).stdout)
        self.assertEqual(observed['project_inference'],dict(schema=1,swarm_id=identity['swarm_id'],project_id='work'))
        self.assertFalse((self.root / 'git-called').exists())
        self.assertEqual(catalog.read_bytes(),before)
        nested = self.root / 'nested'; nested.mkdir()
        self.assertNotIn('project_inference',json.loads(self.hgs('dirs',str(nested)).stdout))
        self.assertEqual(catalog.read_bytes(),before)
        self.assertFalse((self.root / '.git').exists())

    def test_browsing_never_initializes_a_missing_project_catalog(self):
        catalog = self.root / 'missing-catalog'
        self.env['HGS_CONFIG_DIR'] = str(catalog)
        result = json.loads(self.hgs('dirs',str(self.root)).stdout)
        self.assertNotIn('project_inference',result)
        self.assertFalse(catalog.exists())

    def test_create_literal_path_then_browse_and_select(self):
        target = self.root / "new ' проект $(touch injected) `touch injected2`"
        token = str(uuid.uuid4())
        receipt = json.loads(self.hgs("dirs", "--create", str(target), "--request-id", token).stdout)
        self.assertEqual(receipt["status"], "created")
        self.assertEqual(receipt["request_id"], token)
        self.assertEqual(receipt["requested_path"], str(target))
        self.assertEqual(receipt["path"], str(target.resolve()))
        self.assertEqual(receipt["parent"], str(self.root.resolve()))
        self.assertEqual(list(target.iterdir()), [])
        self.assertFalse((self.root / "injected").exists())
        self.assertFalse((self.root / "injected2").exists())
        self.assertFalse((target / ".git").exists())
        self.assertEqual(json.loads(self.hgs("dirs", receipt["path"]).stdout)["directories"], [])

    def test_home_path_and_generated_identity(self):
        receipt = json.loads(self.hgs("dirs", "--create", "~/project").stdout)
        self.assertEqual(receipt["path"], str((self.root / "project").resolve()))
        self.assertEqual(str(uuid.UUID(receipt["request_id"])), receipt["request_id"])
        self.assertEqual(receipt["requested_path"], "~/project")

    def test_missing_parent_never_creates_intermediates(self):
        self.hgs("dirs", "--create", str(self.root / "missing" / "project"), rc=1)
        self.assertFalse((self.root / "missing").exists())

    def test_existing_directory_file_and_symlink_are_never_overwritten(self):
        directory = self.root / "directory"
        directory.mkdir()
        sentinel = directory / "retained"
        sentinel.write_text("retained")
        file = self.root / "file"
        file.write_text("retained")
        link = self.root / "link"
        link.symlink_to(directory, target_is_directory=True)
        for target in (directory, file, link):
            with self.subTest(path=target.name):
                self.hgs("dirs", "--create", str(target), rc=1)
        self.assertEqual(sentinel.read_text(), "retained")
        self.assertEqual(file.read_text(), "retained")
        self.assertTrue(link.is_symlink())

    def test_repeating_create_does_not_repeat_effect(self):
        target = self.root / "project"
        token = str(uuid.uuid4())
        self.hgs("dirs", "--create", str(target), "--request-id", token)
        (target / "retained").write_text("retained")
        self.hgs("dirs", "--create", str(target), "--request-id", token, rc=1)
        self.assertEqual((target / "retained").read_text(), "retained")

    def test_bad_request_identity_options_and_control_paths_have_no_effect(self):
        target = self.root / "project"
        for token in ("", "not-a-uuid", str(uuid.uuid4()).upper(), "00000000-0000-0000-0000-000000000000"):
            self.hgs("dirs", "--create", str(target), "--request-id", token, rc=1)
        self.hgs("dirs", "--create", str(target), "--unknown", rc=1)
        for path in ("", "bad\npath", "bad\x7fpath"):
            self.hgs("dirs", "--create", path, rc=1)
        self.assertFalse(target.exists())
        self.assertFalse((self.root / "bad\npath").exists())

    def test_invalid_leaf_and_oversized_utf8_path_have_no_effect(self):
        for suffix in ("project/", "project/.", "project/.."):
            self.hgs("dirs", "--create", str(self.root) + "/" + suffix, rc=1)
        self.hgs("dirs", "--create", str(self.root) + "/" + "界" * 1400, rc=1)
        self.assertFalse((self.root / "project").exists())

    def test_local_dry_run_never_creates(self):
        target = self.root / "project"
        self.hgs("--dry-run", "dirs", "--create", str(target), rc=1)
        self.hgs("dirs", "--create", str(target), "--dry-run", rc=1)
        self.assertFalse(target.exists())

    def test_double_slash_home_prefix_never_escapes_home(self):
        with tempfile.TemporaryDirectory(prefix="zerus-outside-home-") as outside:
            target = Path(outside) / "must-not-create"
            ambiguous = "~/" + str(target)
            self.hgs("dirs", "--create", ambiguous, rc=1)
            self.assertFalse(target.exists())

    def test_remote_dry_run_preserves_literal_path_and_does_not_create(self):
        target = self.root / "project ' with spaces"
        result = self.hgs("--dry-run", "@remote.invalid", "dirs", "--create", str(target))
        self.assertIn("BatchMode=yes", result.stdout)
        self.assertIn("dirs --create", result.stdout)
        self.assertFalse(target.exists())


if __name__ == "__main__":
    unittest.main()
