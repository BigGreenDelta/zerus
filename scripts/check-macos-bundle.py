#!/usr/bin/env python3
"""Check a Zerus.app bundle before signing or publication; never modifies it.

Every Mach-O file must be arm64 only, require at most the bundle's minimum macOS
version and reference only the bundle itself or the system. The bundle must carry
its setup files and every notice for the code it distributes.
"""
import argparse
import os
from pathlib import Path
import plistlib
import re
import subprocess
import sys
import tempfile

BUNDLE_ID = "com.hgdev.hgs-tray"
MINIMUM_SYSTEM = "14.0"
ALLOWED_LIBRARIES = ("@rpath/", "@executable_path/", "@loader_path/", "/System/Library/", "/usr/lib/")
ALLOWED_RPATHS = ("@executable_path/", "@loader_path/")
REFERENCES = {"LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB", "LC_LOAD_UPWARD_DYLIB", "LC_LAZY_LOAD_DYLIB"}
MACH_O = {b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xce",
          b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"}
QT_NOTICES = ("qtbase", "qtdeclarative", "qtsvg", "qtwebchannel", "qtpositioning", "qtmultimedia", "qtwebengine",
              "qtwebengine-chromium-webengine")
EXECUTABLES = ["Contents/MacOS/hgs-tray", "Contents/MacOS/hgs", "Contents/Resources/bin/zerus-setup"]
REQUIRED_FILES = [
    "Contents/Info.plist", *EXECUTABLES,
    "Contents/Resources/setup/hgs_state.py",
    "Contents/Resources/setup/install-macos-session-service.py",
    "Contents/Resources/setup/install-recovery-service.py",
    "Contents/Resources/setup/install-swarm-service.py",
    "Contents/Resources/setup/launchd_agent.py",
    "Contents/Resources/setup/com.hgdev.hgs-tray.plist",
    "Contents/Resources/doc/README.md",
    "Contents/Resources/doc/tmux.conf",
    "Contents/Resources/licenses/LICENSE",
    "Contents/Resources/licenses/THIRD_PARTY_NOTICES.md",
    "Contents/Resources/licenses/libvterm-LICENSE",
    "Contents/Resources/licenses/md4c-LICENSE",
    "Contents/Resources/licenses/codex-LICENSE.txt",
    "Contents/Resources/licenses/rust/inventory.json",
    "Contents/Resources/licenses/qt/LGPL-3.0.txt",
    "Contents/Resources/licenses/qt/GPL-3.0.txt",
    "Contents/Resources/licenses/qt/SOURCE.md",
    *[f"Contents/Resources/licenses/qt/{module}.spdx.json" for module in QT_NOTICES],
]
NAME = re.compile(r"^\s*name (.*) \(offset \d+\)$")
RPATH = re.compile(r"^\s*path (.*) \(offset \d+\)$")


def version_key(value):
    return tuple(int(part) for part in value.split("."))


def load_commands(text):
    """Return (command, value) pairs for library references, rpaths and minimum OS versions."""
    commands, current = [], None
    for line in text.splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[0] == "cmd":
            current = fields[1]
            continue
        if current in REFERENCES and (match := NAME.match(line)):
            commands.append((current, match.group(1)))
        elif current == "LC_RPATH" and (match := RPATH.match(line)):
            commands.append((current, match.group(1)))
        elif current == "LC_BUILD_VERSION" and fields[:1] == ["minos"]:
            commands.append((current, fields[1]))
        elif current == "LC_VERSION_MIN_MACOSX" and fields[:1] == ["version"]:
            commands.append(("LC_BUILD_VERSION", fields[1]))
        else:
            continue
        current = None
    return commands


def reference_findings(label, commands, minimum=MINIMUM_SYSTEM):
    findings = []
    for command, value in commands:
        if command in REFERENCES and not value.startswith(ALLOWED_LIBRARIES):
            findings.append(f"{label}: links {value}")
        elif command == "LC_RPATH" and not value.startswith(ALLOWED_RPATHS):
            findings.append(f"{label}: rpath {value}")
        elif command == "LC_BUILD_VERSION" and version_key(value) > version_key(minimum):
            findings.append(f"{label}: requires macOS {value}, newer than {minimum}")
    return findings


def executable_directories(bundle, binary):
    """Possible @executable_path values: the innermost enclosing application, then the main one."""
    directories = [parent / "Contents/MacOS" for parent in binary.parents
                   if parent.suffix == ".app" and (parent / "Contents/MacOS").is_dir()][:1]
    main = bundle / "Contents/MacOS"
    return directories + ([main] if main not in directories else [])


def unresolved_references(bundle, binary, commands, inherited_rpaths=()):
    """Report bundle-relative library references that do not resolve to a file in the bundle."""
    label = str(binary.relative_to(bundle))
    rpaths = [value for command, value in commands if command == "LC_RPATH"] + list(inherited_rpaths)
    findings = []
    for command, value in commands:
        if command not in REFERENCES or not value.startswith("@"):
            continue
        candidates = []
        for executable in executable_directories(bundle, binary):
            def expand(path):
                return path.replace("@loader_path", str(binary.parent)).replace("@executable_path", str(executable))
            if value.startswith("@rpath/"):
                candidates += [Path(expand(rpath)) / value[len("@rpath/"):] for rpath in rpaths]
            else:
                candidates.append(Path(expand(value)))
        if not any(os.path.isfile(candidate) for candidate in candidates):
            findings.append(f"{label}: unresolved {value}")
    return findings


def plist_findings(plist, version, release=None):
    # ZerusReleaseVersion names the stable or nightly release; the GUI uses it to notice
    # a replaced bundle even when the product version stays the same.
    expected = {"CFBundleIdentifier": BUNDLE_ID, "CFBundleExecutable": "hgs-tray", "CFBundleName": "Zerus",
                "CFBundleDisplayName": "Zerus", "CFBundleShortVersionString": version,
                "CFBundleVersion": version, "LSMinimumSystemVersion": MINIMUM_SYSTEM,
                "ZerusReleaseVersion": release or version}
    findings = [f"Info.plist {key} is {plist.get(key)!r}, expected {value!r}"
                for key, value in expected.items() if plist.get(key) != value]
    # Terminal integration runs osascript; TCC attributes its Apple Events to Zerus.
    if not str(plist.get("NSAppleEventsUsageDescription", "")).strip():
        findings.append("Info.plist lacks NSAppleEventsUsageDescription")
    return findings


def is_mach_o(path):
    with path.open("rb") as stream:
        return stream.read(4) in MACH_O


def bundle_findings(bundle, version, release=None):
    findings = plist_findings(plistlib.loads((bundle / "Contents/Info.plist").read_bytes()), version, release)
    findings += [f"missing {name}" for name in REQUIRED_FILES if not (bundle / name).is_file()]
    findings += [f"not executable: {name}" for name in EXECUTABLES
                 if (bundle / name).is_file() and not os.access(bundle / name, os.X_OK)]
    root = bundle.resolve()
    main = bundle / "Contents/MacOS/hgs-tray"
    main_rpaths = []
    if main.is_file():
        main_rpaths = [value.replace("@executable_path", str(main.parent)).replace("@loader_path", str(main.parent))
                       for command, value in load_commands(subprocess.check_output(["otool", "-l", str(main)], text=True))
                       if command == "LC_RPATH"]
    for directory, names, files in os.walk(bundle):
        for name in names + files:
            path = Path(directory) / name
            label = str(path.relative_to(bundle))
            if path.is_symlink():
                target = (path.parent / os.readlink(path)).resolve()
                if root not in target.parents and target != root:
                    findings.append(f"{label}: symlink leaves the bundle")
                continue
            if name not in files or not is_mach_o(path):
                continue
            architectures = subprocess.check_output(["lipo", "-archs", str(path)], text=True).split()
            if architectures != ["arm64"]:
                findings.append(f"{label}: architectures {' '.join(architectures)}, expected arm64 only")
            commands = load_commands(subprocess.check_output(["otool", "-l", str(path)], text=True))
            findings += reference_findings(label, commands)
            findings += unresolved_references(bundle, path, commands, main_rpaths)
    return findings


def run_selftest(bundle):
    with tempfile.TemporaryDirectory(prefix="zerus-bundle-home-") as home:
        environment = {"HOME": home, "PATH": "/usr/bin:/bin", "TMPDIR": home}
        hgs = bundle / "Contents/MacOS/hgs"
        for command in ([str(hgs), "--version"], [str(bundle / "Contents/MacOS/hgs-tray"), "--version"],
                        [str(bundle / "Contents/MacOS/hgs-tray"), "--selftest"]):
            result = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=60)
            if result.returncode:
                return [f"{' '.join(command[1:])} failed: {result.stderr.strip() or result.stdout.strip()}"]
    return []


def signature_findings(bundle, team):
    findings = []
    verify = subprocess.run(["codesign", "--verify", "--deep", "--strict", "--verbose=2", str(bundle)],
                            capture_output=True, text=True)
    if verify.returncode:
        findings.append("codesign verification failed: " + verify.stderr.strip())
    details = subprocess.run(["codesign", "-dv", "--verbose=4", str(bundle)], capture_output=True, text=True).stderr
    if f"TeamIdentifier={team}" not in details.splitlines():
        findings.append(f"bundle is not signed by Team ID {team}")
    if "flags=0x10000(runtime)" not in details:
        findings.append("bundle does not use the hardened runtime")
    assess = subprocess.run(["spctl", "-a", "-t", "exec", "-vv", str(bundle)], capture_output=True, text=True)
    if assess.returncode or "source=Notarized Developer ID" not in assess.stderr:
        findings.append("Gatekeeper does not accept a notarized Developer ID bundle: " + assess.stderr.strip())
    staple = subprocess.run(["xcrun", "stapler", "validate", str(bundle)], capture_output=True, text=True)
    if staple.returncode:
        findings.append("notarization ticket is not stapled: " + (staple.stdout + staple.stderr).strip())
    return findings


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--release", help="stable or nightly release version (default: --version)")
    parser.add_argument("--selftest", action="store_true", help="also run the bundled CLI and GUI self-tests")
    parser.add_argument("--team-id", help="require a notarized Developer ID signature from this team")
    args = parser.parse_args()
    findings = bundle_findings(args.bundle, args.version, args.release)
    if not findings and args.selftest:
        findings = run_selftest(args.bundle)
    if not findings and args.team_id:
        findings = signature_findings(args.bundle, args.team_id)
    for finding in findings:
        print(f"check-macos-bundle: {finding}", file=sys.stderr)
    if findings:
        return 1
    print(f"{args.bundle.name} {args.release or args.version}: bundle checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
