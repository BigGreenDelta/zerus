"""Catalog safety and current-source refresh contracts; no application settings."""
import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

from prepare_catalog import prepare, read_catalog
from refresh_catalog import refresh


def catalog(contexts):
    root = ET.Element("TS", version="2.1", language="ru_RU")
    for name, rows in contexts.items():
        context = ET.SubElement(root, "context")
        ET.SubElement(context, "name").text = name
        for source, translated, state, comment in rows:
            message = ET.SubElement(context, "message")
            ET.SubElement(message, "source").text = source
            if comment:
                ET.SubElement(message, "comment").text = comment
            tr = ET.SubElement(message, "translation", **({"type": state} if state else {}))
            tr.text = translated
    return root


class CatalogTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.source = Path(self.temporary.name) / "source.ts"
        self.result = Path(self.temporary.name) / "result.ts"

    def write(self, root):
        ET.ElementTree(root).write(self.source, encoding="utf-8")

    def test_unfinished_is_an_explicit_english_fallback(self):
        self.write(catalog({"Panel": [("New text", "", "unfinished", "")]}))
        prepare(self.source, self.result)
        root = read_catalog(self.result)
        self.assertEqual(root.find("context/message/translation").get("type"), "unfinished")
        self.assertEqual(root.findall("context")[-1].findall("message"), [])

    def test_only_unambiguous_undisambiguated_aliases(self):
        self.write(catalog({"One": [("Open", "Открыть", "", ""), ("State", "Состояние", "", "")],
                            "Two": [("Open", "Открыть", "", ""), ("State", "Штат", "", ""),
                                    ("Close", "Закрыть", "", "verb")]}))
        prepare(self.source, self.result)
        shared = read_catalog(self.result).find("context[name='Zerus']")
        self.assertEqual([m.findtext("source") for m in shared.findall("message")], ["Open"])

    def test_placeholder_and_markup_changes_are_refused(self):
        for text, translated in [("File %1", "Файл %2"), ("<b>%1</b>", "<i>%1</i>"),
                                 ("Value %L1", "Значение %1"), ("Text", "")]:
            with self.subTest(text=text):
                self.write(catalog({"Panel": [(text, translated, "", "")]}))
                with self.assertRaises(ValueError):
                    prepare(self.source, self.result)

    def test_russian_plural_forms_keep_n(self):
        root = catalog({"Panel": [("%n computer(s)", "", "", "")]})
        message = root.find("context/message")
        message.set("numerus", "yes")
        tr = message.find("translation")
        for form in ["%n компьютер", "%n компьютера", "%n компьютеров"]:
            ET.SubElement(tr, "numerusform").text = form
        self.write(root)
        prepare(self.source, self.result)
        self.assertEqual(len(read_catalog(self.result).findall(".//numerusform")), 6)
        tr.remove(tr[-1])
        self.write(root)
        with self.assertRaises(ValueError):
            prepare(self.source, self.result)

    def test_retired_and_entity_catalogs_are_refused(self):
        self.write(catalog({"Panel": [("Old", "Старое", "vanished", "")]}))
        with self.assertRaises(ValueError):
            prepare(self.source, self.result)
        self.source.write_text('<!DOCTYPE TS [<!ENTITY a "value">]><TS language="ru_RU"/>')
        with self.assertRaises(ValueError):
            read_catalog(self.source)

    def test_refresh_reconciles_contexts_retired_and_new_strings(self):
        self.write(catalog({"Panel": [("Open", "Открыть", "", ""),
                                     ("Retired", "Старое", "", ""),
                                     ("Moved", "Перемещено", "", "")]}))
        old_digest = hashlib.sha256(self.source.read_bytes()).hexdigest()
        extracted = catalog({"Panel": [("Open", "", "unfinished", ""),
                                       ("New", "", "unfinished", "")],
                             "Other": [("Moved", "", "unfinished", "")]})
        def extract(argv, **kwargs):
            self.assertIn("-no-obsolete", argv)
            self.assertTrue(kwargs["check"])
            ET.ElementTree(extracted).write(Path(argv[-1]), encoding="utf-8")
        with patch("refresh_catalog.subprocess.run", side_effect=extract):
            result = refresh(self.source, self.source, "trusted-lupdate")
        self.assertEqual(result["seed_sha256"], old_digest)
        self.assertEqual(result["entries"], 3)
        self.assertEqual(result["same_context_reused"], 1)
        self.assertEqual(result["unambiguous_source_reused"], 1)
        self.assertEqual(result["unfinished"], 1)
        self.assertEqual(result["retired_seed_entries"], 2)
        self.assertEqual(list(self.source.parent.glob(".refresh-*")), [])


if __name__ == "__main__":
    unittest.main()
