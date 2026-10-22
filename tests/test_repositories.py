"""Repository acquisition contracts: real Git, private profiles, no agent state."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import uuid

REPO = Path(__file__).resolve().parents[1]
HGS = Path(os.environ.get("HGS_TEST_BIN", REPO / "hgs")).resolve()


class Repositories(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="zerus-repository-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.env = {k: v for k, v in os.environ.items() if not k.startswith(("HGS_", "GIT_"))}
        self.env.update(HOME=str(self.root), USERPROFILE=str(self.root),
                        XDG_CONFIG_HOME=str(self.root / "config"), XDG_STATE_HOME=str(self.root / "state"),
                        XDG_DATA_HOME=str(self.root / "data"), GIT_CONFIG_NOSYSTEM="1",
                        GIT_CONFIG_GLOBAL=str(self.root / "gitconfig"),
                        GIT_AUTHOR_NAME="Fixture", GIT_AUTHOR_EMAIL="fixture@example.invalid",
                        GIT_COMMITTER_NAME="Fixture", GIT_COMMITTER_EMAIL="fixture@example.invalid")
        self.source = self.root / "source"
        self.source.mkdir()
        self.git("init", "-b", "main")
        (self.source / "content").write_text("preserved\n")
        self.git("add", "content")
        self.git("commit", "-m", "fixture")
        self.url = "https://example.invalid/team/source.git"
        subprocess.run(["git", "config", "--global", "url." + self.source.as_uri() + ".insteadOf", self.url],
                       env=self.env, check=True, capture_output=True)

    def git(self, *args):
        return subprocess.run(["git", "-C", str(self.source), *args], env=self.env,
                              capture_output=True, text=True, check=True, timeout=10)

    def hgs(self, *args, rc=0):
        result = subprocess.run([str(HGS), *args], env=self.env, cwd=self.root,
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, rc, result.stdout + result.stderr)
        return json.loads(result.stdout) if rc == 0 else result

    def operation(self, action, name="checkout", token=None, parent=None, url=None, rc=0):
        args = ["repository", action, "--url", url or self.url, "--parent", str(parent or self.root),
                "--name", name, "--json"]
        if token is not None:
            args += ["--request-id", token]
        return self.hgs(*args, rc=rc)

    def test_check_has_no_destination_or_receipt_effect(self):
        receipt = self.operation("check")
        self.assertEqual(receipt["status"], "ready")
        self.assertEqual(receipt["path"], str(self.root / "checkout"))
        self.assertFalse((self.root / "checkout").exists())
        self.assertFalse((self.root / "state" / "hgs" / "sessions" / "repository-clones").exists())

    def test_clone_checkout_and_exact_receipt(self):
        token = str(uuid.uuid4())
        receipt = self.operation("clone", token=token)
        self.assertEqual(receipt["status"], "cloned")
        self.assertEqual(receipt["request_id"], token)
        self.assertEqual(receipt["parent"], str(self.root))
        self.assertEqual((self.root / "checkout" / "content").read_text(), "preserved\n")
        self.assertEqual(self.hgs("repository", "status", "--request-id", token, "--json"), receipt)

    def test_duplicate_does_not_restore_or_modify_checkout(self):
        token = str(uuid.uuid4())
        receipt = self.operation("clone", token=token)
        target = self.root / "checkout" / "content"
        target.write_text("local changes")
        self.assertEqual(self.operation("clone", token=token), receipt)
        self.assertEqual(target.read_text(), "local changes")
        self.operation("clone", name="different", token=token, rc=1)
        self.assertFalse((self.root / "different").exists())

    def test_unconfirmed_receipt_never_replays_git(self):
        token = str(uuid.uuid4())
        receipt = self.operation("clone", token=token)
        receipt["status"] = "unconfirmed"
        journal = self.root / "state" / "hgs" / "sessions" / "repository-clones" / (token + ".json")
        journal.write_text(json.dumps(receipt))
        target = self.root / "checkout" / "content"
        target.write_text("retained after uncertain outcome")
        self.assertEqual(self.operation("clone", token=token), receipt)
        self.assertEqual(target.read_text(), "retained after uncertain outcome")

    def test_failed_clone_is_durable_and_never_replayed(self):
        token = str(uuid.uuid4())
        # The URL maps to a missing local repository: deterministic, no network.
        self.git("config", "--global", "url." + (self.root / "missing").as_uri() + ".insteadOf", "https://missing.invalid/repo")
        receipt = self.operation("clone", token=token, url="https://missing.invalid/repo")
        self.assertEqual(receipt["status"], "failed")
        self.assertIn("detail", receipt)
        self.assertEqual(self.operation("clone", token=token, url="https://missing.invalid/repo"), receipt)

    def test_existing_directory_file_and_symlink_are_preserved(self):
        (self.root / "existing").mkdir()
        sentinel = self.root / "existing" / "local"
        sentinel.write_text("retained")
        (self.root / "file").write_text("retained")
        names = ["existing", "file"]
        if os.name != "nt":
            (self.root / "link").symlink_to(self.root / "existing", target_is_directory=True)
            names.append("link")
        for name in names:
            self.operation("check", name=name, rc=1)
            self.operation("clone", name=name, token=str(uuid.uuid4()), rc=1)
        self.assertEqual(sentinel.read_text(), "retained")
        self.assertEqual((self.root / "file").read_text(), "retained")

    def test_literal_folder_name_and_canonical_parent(self):
        name = "checkout ' $(touch injected) `touch another`"
        receipt = self.operation("clone", name=name, token=str(uuid.uuid4()))
        self.assertEqual(receipt["status"], "cloned")
        self.assertEqual((self.root / name / "content").read_text(), "preserved\n")
        self.assertFalse((self.root / "injected").exists())
        self.assertFalse((self.root / "another").exists())

    def test_invalid_urls_names_and_request_ids_have_no_checkout_effect(self):
        for url in ("ext::bad", "--upload-pack=bad", "file:///tmp/repo", "https://token@example.invalid/repo"):
            self.operation("clone", url=url, token=str(uuid.uuid4()), rc=1)
        for name in ("..", "/tmp/repo", "a/b", "a\\b", "bad\nname", "trailing."):
            self.operation("clone", name=name, token=str(uuid.uuid4()), rc=1)
        for token in ("bad", str(uuid.uuid4()).upper(), "00000000-0000-0000-0000-000000000000"):
            self.operation("clone", token=token, rc=1)
        self.assertFalse((self.root / "checkout").exists())

    def test_missing_parent_and_dry_run_never_clone(self):
        self.operation("clone", parent=self.root / "missing", token=str(uuid.uuid4()), rc=1)
        self.hgs("--dry-run", "repository", "clone", "--url", self.url, "--parent", str(self.root),
                 "--name", "checkout", "--request-id", str(uuid.uuid4()), rc=1)
        self.assertFalse((self.root / "checkout").exists())

    def test_inherited_git_directory_cannot_redirect_the_clone(self):
        self.env.update(GIT_DIR=str(self.source / ".git"), GIT_WORK_TREE=str(self.source),
                        GIT_COMMON_DIR=str(self.source / ".git"))
        receipt = self.operation("clone", token=str(uuid.uuid4()))
        self.assertEqual(receipt["status"], "cloned")
        self.assertEqual((self.root / "checkout" / "content").read_text(), "preserved\n")
        self.assertEqual((self.source / "content").read_text(), "preserved\n")

    @unittest.skipIf(os.name == "nt", "symlink permission is not assumed")
    def test_parent_alias_is_resolved_on_the_computer(self):
        alias = self.root / "alias"
        alias.symlink_to(self.root, target_is_directory=True)
        receipt = self.operation("check", parent=alias)
        self.assertEqual(receipt["parent"], str(self.root))
        self.assertEqual(receipt["path"], str(self.root / "checkout"))
        self.assertFalse((self.root / "checkout").exists())

    def test_remote_dry_run_keeps_capability_and_noninteractive_guards(self):
        result = subprocess.run([str(HGS), "--dry-run", "@remote.invalid", "repository", "clone",
                                 "--url", self.url, "--parent", "/workspace", "--name", "example", "--request-id", str(uuid.uuid4())],
                                env=self.env, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("BatchMode=yes", result.stdout)
        self.assertIn("repository-clone-v1", result.stdout)
        self.assertFalse((self.root / "checkout").exists())

    def test_empty_repository_is_a_valid_checkout(self):
        self.git("checkout", "--orphan", "empty")
        empty = self.root / "empty-source"
        subprocess.run(["git", "init", "--bare", str(empty)], env=self.env, check=True, capture_output=True)
        self.git("config", "--global", "url." + empty.as_uri() + ".insteadOf", "https://empty.invalid/repo")
        receipt = self.operation("clone", url="https://empty.invalid/repo", token=str(uuid.uuid4()))
        self.assertEqual(receipt["status"], "cloned")
        self.assertTrue((self.root / "checkout" / ".git").is_dir())
