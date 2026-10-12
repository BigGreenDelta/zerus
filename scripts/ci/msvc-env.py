#!/usr/bin/env python3
"""Run a command in the Visual Studio x64 developer environment (Windows).

Usage: python scripts/ci/msvc-env.py PROGRAM [ARGUMENT...]

vswhere finds the newest Visual Studio with the x64 C++ tools; vcvars64.bat
then provides cl, link, the Windows SDK and Visual Studio's CMake and Ninja.
The environment applies only to PROGRAM, so earlier and later CI steps (and
the Rust toolchain's own linker discovery) are not affected.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys


def developer_environment():
    program_files = os.environ.get("ProgramFiles(x86)") or os.environ.get("ProgramFiles") or r"C:\Program Files (x86)"
    vswhere = Path(program_files) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.is_file():
        raise SystemExit(f"msvc-env: vswhere is missing: {vswhere}")
    install = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
                                       "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                       "-property", "installationPath", "-utf8"]).decode("utf-8").strip()
    vcvars = Path(install) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
    if not install or not vcvars.is_file():
        raise SystemExit("msvc-env: no Visual Studio with the x64 C++ tools was found.")
    environment = dict(os.environ, VSCMD_SKIP_SENDTELEMETRY="1")
    # cmd /u writes the output of `set` as UTF-16, so non-ASCII values survive
    # any console code page. /s keeps the quoted batch path intact.
    result = subprocess.run(f'cmd.exe /d /u /s /c "call "{vcvars}" >nul && set"', env=environment,
                            capture_output=True)
    variables = {}
    for line in result.stdout.decode("utf-16-le", "replace").splitlines():
        name, separator, value = line.partition("=")
        if separator and name and not name.startswith("="):
            variables[name.upper()] = value
    if result.returncode or variables.get("VSCMD_ARG_TGT_ARCH") != "x64" or "VCINSTALLDIR" not in variables:
        raise SystemExit("msvc-env: vcvars64.bat failed: " + result.stderr.decode("utf-8", "replace").strip())
    environment = {key.upper(): value for key, value in os.environ.items()}
    environment.update(variables)
    # vcvars appends Visual Studio's CMake and Ninja; put them first so other
    # copies on PATH (Strawberry Perl ships both) do not shadow them.
    tools = Path(install) / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake"
    first = [str(path) for path in (tools / "CMake" / "bin", tools / "Ninja") if path.is_dir()]
    environment["PATH"] = os.pathsep.join([*first, environment.get("PATH", "")])
    # English compiler diagnostics in CI logs regardless of the system language.
    environment.setdefault("VSLANG", "1033")
    return environment


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__.strip())
    if os.name != "nt":
        raise SystemExit("msvc-env: the Visual Studio developer environment exists on Windows only.")
    environment = developer_environment()
    # CreateProcess would search System32 before PATH (System32\bash.exe is the
    # WSL launcher), so resolve the program against the new PATH explicitly.
    program = shutil.which(sys.argv[1], path=environment["PATH"])
    if program is None:
        raise SystemExit(f"msvc-env: {sys.argv[1]} is not on PATH.")
    return subprocess.run([program, *sys.argv[2:]], env=environment).returncode


if __name__ == "__main__":
    sys.exit(main())
