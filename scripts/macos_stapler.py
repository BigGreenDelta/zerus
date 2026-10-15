#!/usr/bin/env python3
"""Run Apple ticket operations, retrying only the observed lost-connection error."""
import argparse
import subprocess
import sys
import time


def run_stapler(action, path):
    if action not in ("staple", "validate"):
        raise ValueError("Unsupported stapler operation.")
    command = ["xcrun", "stapler", action, str(path)]
    for delay in (5, 15, 30, 60, None):
        result = subprocess.run(command, capture_output=True, text=True)
        if (result.returncode == 0 or delay is None or result.returncode != 68 or
                "NSURLErrorDomain Code=-1005" not in result.stdout + result.stderr):
            return result
        print(result.stdout + result.stderr, end="", file=sys.stderr)
        print(f"Apple ticket connection was lost; retrying stapler {action} in {delay}s.", file=sys.stderr)
        time.sleep(delay)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("staple", "validate"))
    parser.add_argument("path")
    args = parser.parse_args()
    result = run_stapler(args.action, args.path)
    print(result.stdout, end="")
    print(result.stderr, end="", file=sys.stderr)
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
