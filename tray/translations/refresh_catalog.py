#!/usr/bin/env python3
"""Refresh current source strings; preserve reviewed translations, never guess new ones."""
import argparse
import copy
import hashlib
import json
from collections import defaultdict
from pathlib import Path
import shutil
import subprocess
import tempfile
import os
import xml.etree.ElementTree as ET

from prepare_catalog import read_catalog

ROOT = Path(__file__).resolve().parents[2]


def entries(root):
    return {(context.findtext("name"), message.findtext("source", ""),
             message.findtext("comment", ""), message.get("numerus", "no")): message
            for context in root.findall("context") for message in context.findall("message")}


def finished(message):
    translation = message.find("translation")
    return translation is not None and not translation.get("type")


def refresh(seed, catalog, lupdate):
    seed_digest = hashlib.sha256(Path(seed).read_bytes()).hexdigest()
    old = entries(read_catalog(seed))
    by_source = defaultdict(list)
    for key, message in old.items():
        if finished(message):
            by_source[key[1:]].append(message)
    # Same parent as the final catalog: extracted locations stay repository-relative.
    descriptor, name = tempfile.mkstemp(dir=catalog.parent, prefix=".refresh-", suffix=".ts")
    os.close(descriptor)
    target = Path(name)
    try:
        target.unlink()
        subprocess.run([lupdate, str(ROOT / "tray/src"), "-no-obsolete", "-ts", str(target)], check=True)
        current_root = ET.parse(target).getroot()
        current_root.set("language", "ru_RU")
        current_root.set("sourcelanguage", "en")
        current = entries(current_root)
        direct = reused = 0
        for key, message in current.items():
            candidate = old.get(key)
            if candidate is not None and finished(candidate):
                direct += 1
            else:
                variants = by_source.get(key[1:], [])
                if len({ET.tostring(m.find("translation")) for m in variants}) != 1:
                    continue
                candidate = variants[0]
                reused += 1
            original = message.find("translation")
            if original is not None:
                message.remove(original)
            message.append(copy.deepcopy(candidate.find("translation")))
        ET.indent(current_root)
        ET.ElementTree(current_root).write(catalog, encoding="utf-8", xml_declaration=True)
    finally:
        target.unlink(missing_ok=True)
    return {"entries": len(current), "same_context_reused": direct,
            "unambiguous_source_reused": reused, "unfinished": len(current) - direct - reused,
            "retired_seed_entries": len(set(old) - set(current)),
            "seed_sha256": seed_digest,
            "catalog_sha256": hashlib.sha256(catalog.read_bytes()).hexdigest()}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", type=Path, default=ROOT / "tray/translations/zerus_ru.ts")
    parser.add_argument("--seed", type=Path)
    parser.add_argument("--lupdate", default=shutil.which("lupdate6") or shutil.which("lupdate"))
    args = parser.parse_args()
    if not args.lupdate:
        parser.error("Qt lupdate is required")
    print(json.dumps(refresh(args.seed or args.catalog, args.catalog, args.lupdate), indent=2))
