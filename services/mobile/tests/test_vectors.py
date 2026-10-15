"""Golden vectors in services/mobile/vectors stay reproducible from the Python connector."""
import importlib.util
import json
from pathlib import Path
import re
import sqlite3
import unittest

VECTORS = Path(__file__).resolve().parents[1] / "vectors"
# A node_token value, also inside a JSON string holding a configuration file.
TOKEN = re.compile(r'node_token\\*"\s*:\s*\\*"([^"\\]*)')


def load_capture():
    spec = importlib.util.spec_from_file_location("zerus_mobile_vector_capture", VECTORS / "capture.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def strings(value):
    if isinstance(value, str):
        yield value
    elif isinstance(value, list):
        for item in value:
            yield from strings(item)
    elif isinstance(value, dict):
        for key, item in value.items():
            yield key
            yield from strings(item)


def records(name, data):
    if name.endswith(".jsonl"):
        return [json.loads(line) for line in data.decode().splitlines()]
    if name.endswith(".json"):
        return [json.loads(data)]
    return []


class GoldenVectorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.capture = load_capture()
        # The relay-oracle column is frozen data: it is copied, never recomputed here.
        cls.files = cls.capture.render(cls.capture.frozen_oracle(VECTORS))

    def cases(self):
        return [row for name, data in self.files.items() if name.startswith("validation/")
                for row in records(name, data) if row.get("kind") == "case"]

    def test_capture_reproduces_every_file_byte_for_byte(self):
        self.assertEqual(self.capture.differences(self.files), [],
                         "regenerate with services/mobile/vectors/capture.py and review the diff")

    def test_files_are_bounded_strict_json_or_sql(self):
        for name, data in self.files.items():
            with self.subTest(name=name):
                self.assertLessEqual(len(data), self.capture.FILE_LIMIT)
                self.assertTrue(data.endswith(b"\n") and not data.endswith(b"\n\n"))
                self.assertNotIn(b" \n", data)
                data.decode("ascii")
                # A strict UTF-8 JSON reader must accept every file: no lone surrogates.
                for record in records(name, data):
                    for text in strings(record):
                        self.assertFalse(any(0xD800 <= ord(char) <= 0xDFFF for char in text))

    def test_credentials_are_obviously_fake(self):
        for name, data in self.files.items():
            for token in TOKEN.findall(data.decode()):
                with self.subTest(name=name, token=token):
                    self.assertTrue(token == "" or token.startswith(("fixture", "synthetic")))

    def test_journal_fixtures_load_with_python_sqlite(self):
        fixtures = [name for name in self.files if name.startswith("journal/")]
        self.assertGreaterEqual(len(fixtures), 7)
        for name in fixtures:
            with self.subTest(name=name):
                db = sqlite3.connect(":memory:")
                try:
                    db.executescript(self.files[name].decode())
                    tables = {row[0] for row in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
                    self.assertIn("requests", tables)
                finally:
                    db.close()

    def test_operation_tables_have_the_reviewed_sizes(self):
        counts = json.loads(self.files["operations.json"])["counts"]
        self.assertEqual((counts["OPERATIONS"], counts["READ_OPERATIONS"], counts["MUTATIONS"], counts["CONTEXT_KEYS"]),
                         (45, 16, 29, 23))

    def test_every_operation_has_a_valid_case_and_an_envelope(self):
        operations = set(json.loads(self.files["operations.json"])["sets"]["OPERATIONS"])
        valid = {row["id"].split("/", 1)[1] for row in self.cases() if row["id"].startswith("valid/")}
        envelopes = {row["id"].removeprefix("envelope-") for row in records(".jsonl", self.files["request_envelopes.jsonl"])
                     if row["id"].startswith("envelope-")}
        self.assertEqual(valid, operations)
        self.assertEqual(envelopes, operations)

    def test_frozen_oracle_matches_the_reference_relay_while_it_exists(self):
        try:
            importlib.import_module("zerus_mobile.server")
        except ImportError:
            self.skipTest("the Python reference relay was removed; its validation column stays frozen")
        cases = self.cases()
        self.assertGreater(len(cases), 200)
        for case in cases:
            with self.subTest(case=case["id"]):
                self.assertEqual(case["oracle"], self.capture.live_oracle(case["submission"]))


if __name__ == "__main__":
    unittest.main()
