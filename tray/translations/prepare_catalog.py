#!/usr/bin/env python3
"""Validate completed translations and generate unambiguous inherited-tr() aliases.

Unfinished entries are explicit English fallbacks, excluded by lrelease
-nounfinished. This does not turn a partial catalog into a complete translation.
Based on the catalog preparation proposed in PR #6 by frolovnd.
"""
import argparse
import copy
from collections import defaultdict
from pathlib import Path
import re
import xml.etree.ElementTree as ET

PLACEHOLDERS = re.compile(r"%L?\d+|%n")
MARKUP = re.compile(r"<[^>]+>")


def read_catalog(path):
    data = Path(path).read_bytes()
    if len(data) > 8 * 1024 * 1024 or b"<!ENTITY" in data.upper():
        raise ValueError("Oversized catalog or XML entities are not supported")
    root = ET.fromstring(data)
    if root.tag != "TS" or root.get("language") != "ru_RU":
        raise ValueError("Expected the Russian Qt TS catalog")
    return root


def prepare(source, destination):
    root = read_catalog(source)
    candidates = defaultdict(list)
    for message in root.findall(".//message"):
        text = message.findtext("source", "")
        translation = message.find("translation")
        if translation is None:
            raise ValueError(f"Missing translation state: {text!r}")
        state = translation.get("type", "")
        if state == "unfinished":
            continue
        if state:
            raise ValueError(f"Retired or unsupported translation state: {text!r}")
        forms = translation.findall("numerusform")
        values = [form.text or "" for form in forms] if forms else [translation.text or ""]
        if message.get("numerus") == "yes" and len(forms) != 3:
            raise ValueError(f"Russian plurals need three forms: {text!r}")
        for value in values:
            if (not value or sorted(PLACEHOLDERS.findall(text)) != sorted(PLACEHOLDERS.findall(value))
                    or MARKUP.findall(text) != MARKUP.findall(value)):
                raise ValueError(f"Empty translation or changed placeholders/markup: {text!r}")
        if not message.findtext("comment"):
            candidates[text].append(message)

    shared = next((c for c in root.findall("context") if c.findtext("name") == "Zerus"), None)
    if shared is None:
        shared = ET.SubElement(root, "context")
        ET.SubElement(shared, "name").text = "Zerus"
    present = {m.findtext("source") for m in shared.findall("message")}
    for text, messages in candidates.items():
        variants = {ET.tostring(m.find("translation"), encoding="unicode").strip() for m in messages}
        if text in present or len(variants) != 1:
            continue
        message = copy.deepcopy(messages[0])
        for location in message.findall("location"):
            message.remove(location)
        shared.append(message)
    ET.indent(root)
    ET.ElementTree(root).write(destination, encoding="utf-8", xml_declaration=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source")
    parser.add_argument("destination")
    args = parser.parse_args()
    prepare(args.source, args.destination)
