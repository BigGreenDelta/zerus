#!/usr/bin/env bash
# Build and run all Qt suites. Each suite owns its settings and offscreen
# display, so CTest runs them in parallel: ZERUS_TEST_JOBS (default: the CPU
# count, at most 8); 1 runs them one after another.
# On Windows (Git Bash) the build uses MSVC and Ninja: the script re-enters
# itself in the Visual Studio x64 developer environment, and QT_ROOT_DIR names
# the Qt installation (the Windows CI job sets it).
# An optional `build` or `test` argument runs only that half; CI saves its
# compiler cache in between.
set -euo pipefail
phase=${1:-all}
case "$phase" in all|build|test) ;; *) echo 'Usage: gui.sh [build|test]' >&2; exit 2 ;; esac
windows=false
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) windows=true ;; esac
if [ "$windows" = true ] && [ -z "${VCINSTALLDIR:-}" ]; then
    exec "${PYTHON:-python}" "$(dirname "$0")/msvc-env.py" "$(cygpath -w "$BASH")" "$0" "$@"
fi
cd "$(dirname "$0")/../.."
mkdir -p artifacts/test-results
gui_build="${ZERUS_GUI_BUILD_DIR:-$PWD/.ci-build/gui}"
# Every suite also writes per-function JUnit results here (qt-shard-seed.py),
# unless the path has a comma, where QtTest's -o ends the file name.
qt_results="$PWD/artifacts/test-results/qt-functions"
if [ "$windows" = true ]; then
    qt_results=$(cygpath -m "$qt_results")
fi
case "$qt_results" in *,*) qt_results= ;; esac
cmake_args=(-S tray -B "$gui_build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
    "-DZERUS_QT_RESULTS_DIR=$qt_results")
if [ "$(uname -s)" = Darwin ]; then
    cmake_args+=("-DCMAKE_PREFIX_PATH=$(brew --prefix qt)")
fi
if [ "$windows" = true ]; then
    # Explicit cl: other compilers on PATH (Strawberry Perl's MinGW on hosted
    # runners) must not be picked up by the Ninja generator.
    cmake_args+=(-G Ninja -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl)
    if [ -n "${QT_ROOT_DIR:-}" ]; then
        cmake_args+=("-DCMAKE_PREFIX_PATH=$(cygpath -m "$QT_ROOT_DIR")")
        # Test executables load the Qt DLLs from PATH.
        PATH="$(cygpath -u "$QT_ROOT_DIR")/bin:$PATH"
        export PATH
    fi
    # Without a console window (hosted runners) QtTest results and warnings go
    # to OutputDebugString; keep them in the CTest log instead.
    export QT_FORCE_STDERR_LOGGING=1
fi
if command -v ccache >/dev/null 2>&1; then
    cmake_args+=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi
if [ "$phase" != test ]; then
    cmake "${cmake_args[@]}"
    # ccache learns an MSVC compile's headers from /showIncludes lines with the
    # English prefix. A localized compiler prints another one, so ccache would
    # miss header changes and return stale objects: it only passes compiles on.
    if [ "$windows" = true ] && command -v ccache >/dev/null 2>&1 \
        && ! grep -q '^msvc_deps_prefix = Note: including file:' "$gui_build/CMakeFiles/rules.ninja"; then
        echo "ccache disabled: this compiler does not report included files in English" >&2
        export CCACHE_DISABLE=1
    fi
    cmake --build "$gui_build" --parallel "${ZERUS_BUILD_JOBS:-2}"
fi
if [ "$phase" = build ]; then
    exit 0
fi
export QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software
export QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu
rm -rf artifacts/test-results/qt-functions
mkdir -p artifacts/test-results/qt-functions
cpus=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
jobs=${ZERUS_TEST_JOBS:-$(( cpus < 8 ? cpus : 8 ))}
ctest --test-dir "$gui_build" --parallel "$jobs" --timeout 240 --output-on-failure \
    --output-junit "$PWD/artifacts/test-results/qt.xml" 2>&1 | tee artifacts/test-results/qt.log
