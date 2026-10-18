"""Exercise the source privacy guard on synthetic files."""
import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("check_source_privacy", ROOT / "scripts/check-source-privacy.py")
privacy = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(privacy)

WORK = "maintainer" + "@" + "work-mail.dev"


def issue(**fields):
    return json.dumps({"_type": "issue", "id": "zerus-abc", "title": "Synthetic issue", **fields})


class BeadsAttributionTest(unittest.TestCase):
    def test_work_addresses_attribute_beads_records(self):
        # The team commits with these identities; Beads records who did what.
        line = issue(owner=WORK, created_by=WORK, assignee=WORK,
                     comments=[{"author": WORK, "text": "Checked."}],
                     dependencies=[{"depends_on_id": "zerus-def", "created_by": WORK}])
        self.assertEqual(privacy.findings(".beads/issues.jsonl", line), [])

    def test_free_text_and_other_files_stay_checked(self):
        for name, text in [
            (".beads/issues.jsonl", issue(owner=WORK, description=f"Ask {WORK} for access.")),
            (".beads/issues.jsonl", issue(comments=[{"author": WORK, "text": f"Mail {WORK}."}])),
            ("docs/notes.md", f"Contact {WORK}."),
            ("notes.jsonl", json.dumps({"owner": WORK})),
        ]:
            with self.subTest(name=name, text=text):
                self.assertEqual(privacy.findings(name, text), [(1, "non-example email address")])

    def test_vendored_license_texts_keep_their_contacts(self):
        text = "Send bug reports to " + "freetype" + "@" + "nongnu.org."
        self.assertEqual(privacy.findings("packaging/windows/licenses/FTL.txt", text), [])
        self.assertEqual(privacy.findings("packaging/windows/README.txt", text), [(1, "non-example email address")])
        self.assertEqual(privacy.findings("packaging/windows/licenses/notes.txt", "See /home/" + "someone/notes"),
                         [(1, "non-example absolute home path")])
        # Only the generated verbatim Google notices, not their neighbors.
        google = "mobile/android/app/src/main/assets/licenses/google-notices.txt"
        self.assertEqual(privacy.findings(google, text), [])
        self.assertEqual(privacy.findings(google, "See /home/" + "someone/notes"), [(1, "non-example absolute home path")])
        for name in ("mobile/android/app/src/main/assets/licenses/libraries.tsv",
                     "mobile/android/app/src/main/assets/licenses/google-notices.txt.orig",
                     "mobile/android/app/src/main/assets/licenses/library-notices.txt"):
            with self.subTest(name=name):
                self.assertEqual(privacy.findings(name, text), [(1, "non-example email address")])

    def test_attribution_does_not_hide_paths(self):
        line = issue(owner=WORK, description="See /home/" + "someone/notes")
        self.assertEqual(privacy.findings(".beads/issues.jsonl", line), [(1, "non-example absolute home path")])


class FileNameTest(unittest.TestCase):
    def test_homebrew_cask_files_are_not_addresses(self):
        # Homebrew names channel casks token@channel; their files end in .rb.
        for text in ("Edit Casks/zerus@nightly.rb.", "packaging/homebrew/zerus@nightly.rb.in"):
            with self.subTest(text=text):
                self.assertEqual(privacy.findings("docs/notes.md", text), [])
        self.assertEqual(privacy.findings("docs/notes.md", "Mail zerus" + "@" + "nightly.dev."),
                         [(1, "non-example email address")])


if __name__ == "__main__":
    unittest.main()
