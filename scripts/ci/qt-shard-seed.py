#!/usr/bin/env python3
"""Rebuild tray/tests/qt-shard-durations.txt from per-function QtTest results.

scripts/ci/gui.sh writes the JUnit results of each Qt suite to
artifacts/test-results/qt-functions/<test>.xml, and of each shard to
<suite>-<shard>.xml. Pass one directory per measured run, such as the desktop
artifacts of a main CI run, each downloaded into a directory of its own:

    gh run download <run> -n arch-desktop-results -D <dir>/arch
    gh run download <run> -n mac-desktop-results -D <dir>/mac
    gh run download <run> -n windows-x86_64-<commit> -D <dir>/windows
    python3 scripts/ci/qt-shard-seed.py <dir>/arch <dir>/mac <dir>/windows

A run sums each function over its data rows and shards. The seed keeps the
largest duration of every function and row across the runs. It covers the
suites that ran as shards and those named with --suite; a data-driven function
that took at least --split seconds in some run is recorded by row, so that its
rows run in separate shards. Prints the shard loads RunQtShard.cmake would plan.
"""
import argparse
from collections import defaultdict
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ElementTree

ROOT = Path(__file__).resolve().parents[2]
SEED = ROOT / "tray/tests/qt-shard-durations.txt"
HEADER = """\
# Milliseconds per test function, or per data row of the functions that run as
# rows, of the Qt suites that RunQtShard.cmake splits into shards: the largest
# of the measured runs. Rebuild with scripts/ci/qt-shard-seed.py.
"""
# Tags that RunQtShard.cmake runs whole (see there), or that would not survive
# the seed file unchanged.
UNSAFE_TAG = re.compile(r"^$|^\s|\s$|[:;\[\]\\]|[^\x20-\x7e]")


def measure(run):
    """{suite: {function: seconds}}, {suite: {(function, tag): seconds}} and the sharded suites of a run."""
    functions, rows, sharded = defaultdict(dict), defaultdict(dict), set()
    files = sorted(path for path in Path(run).rglob("*.xml") if path.parent.name == "qt-functions")
    if not files:
        sys.exit(f"{run}: no qt-functions/*.xml results")
    for path in files:
        suite, shard = re.fullmatch(r"(.+?)(?:-(\d+))?", path.stem).groups()
        if shard is not None:
            sharded.add(suite)
        try:
            cases = ElementTree.parse(path).getroot().iter("testcase")
            cases = [(case.get("name", ""), float(case.get("time") or 0)) for case in cases]
        except (ElementTree.ParseError, ValueError) as error:
            print(f"warning: skipping {path}: {error}", file=sys.stderr)
            continue
        for name, seconds in cases:
            # "function", or "function(tag)" per data row.
            match = re.fullmatch(r"(\w+)(?:\((.*)\))?", name, re.S)
            if not match or match[1] in ("initTestCase", "cleanupTestCase"):
                continue
            function, tag = match.groups()
            functions[suite][function] = functions[suite].get(function, 0) + seconds
            if tag is not None:
                rows[suite][function, tag] = rows[suite].get((function, tag), 0) + seconds
    return functions, rows, sharded


def plan(durations, shards):
    """Shard loads in seconds, as RunQtShard.cmake assigns known items."""
    loads = [0] * shards
    for item, milliseconds in sorted(durations.items(), key=lambda entry: (entry[1], entry[0]), reverse=True):
        loads[loads.index(min(loads))] += milliseconds
    return [load / 1000 for load in loads]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("runs", nargs="+", metavar="RUN", help="directory with the qt-functions results of one run")
    parser.add_argument("--suite", action="append", default=[], help="also seed this suite (CTest name)")
    parser.add_argument("--split", type=float, default=4.0, help="record data-driven functions this slow by row")
    parser.add_argument("--shards", type=int, default=10, help="shard count for the printed plan")
    parser.add_argument("--output", type=Path, default=SEED)
    options = parser.parse_args()
    functions, rows, suites = defaultdict(dict), defaultdict(dict), set(options.suite)
    for run in options.runs:
        measured, measured_rows, sharded = measure(run)
        suites |= sharded
        for suite, values in measured.items():
            for function, seconds in values.items():
                functions[suite][function] = max(functions[suite].get(function, 0), seconds)
        for suite, values in measured_rows.items():
            for row, seconds in values.items():
                rows[suite][row] = max(rows[suite].get(row, 0), seconds)
    lines = []
    for suite in sorted(suites):
        if suite not in functions:
            sys.exit(f"No results of {suite}")
        tags = defaultdict(list)
        for function, tag in rows[suite]:
            tags[function].append(tag)
        durations = {}
        for function, seconds in functions[suite].items():
            if seconds >= options.split and tags[function] and not any(UNSAFE_TAG.search(tag) for tag in tags[function]):
                durations.update({f"{function}:{tag}": round(rows[suite][function, tag] * 1000) for tag in tags[function]})
            else:
                durations[function] = round(seconds * 1000)
        lines += [f"{suite} {milliseconds} {item}\n" for item, milliseconds in sorted(durations.items())]
        loads = plan(durations, options.shards)
        by_row = sum(":" in item for item in durations)
        print(f"{suite}: {len(functions[suite])} functions ({by_row} items by row), {sum(loads):.1f} s; "
              f"{options.shards} shards of {min(loads):.1f}-{max(loads):.1f} s; longest item "
              f"{max(durations.values()) / 1000:.1f} s ({max(durations, key=durations.get)})")
    options.output.write_text(HEADER + "".join(lines))
    print(f"Wrote {len(lines)} durations to {options.output}")


if __name__ == "__main__":
    main()
