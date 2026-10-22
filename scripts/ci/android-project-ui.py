#!/usr/bin/env python3
"""One finite synthetic UI suite on a disposable GitHub-hosted VM only."""
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
import zipfile

RUNNER = "app.zerus.mobile.SessionProjectSmokeInstrumentation"
CASES = "current_selection,close_no_write,durable_single_post,route_refusal,original_receipt,stale_navigation"
RESULT_NAMESPACE = "project"
RESULT_PREFIX = "Session project smoke passed:"
APP_CLASSES = {"Lapp/zerus/mobile/SessionProjectController;", "Lapp/zerus/mobile/SessionProjectProtocol;"}
IMAGE = "system-images;android-35;google_apis;x86_64"
SERIAL = "emulator-5554"
REVISIONS = {"emulator": "37.2.12", IMAGE: "9.0.0"}
HOSTED_IMAGES = frozenset({"20261012.334.1", "20261018.339.1"})


def select_suite(arguments):
    """Only these two reviewed finite runners; no arbitrary instrumentation/argv injection."""
    global RUNNER, CASES, RESULT_NAMESPACE, RESULT_PREFIX, APP_CLASSES
    assert arguments in ([], ["--approval-choices"]), "unknown UI suite"
    if arguments:
        RUNNER = "app.zerus.mobile.ApprovalChoicesSmokeInstrumentation"
        CASES = "countdown_once,broader_cancel,broader_confirm,card_replacement,feedback_keyboard,lifecycle_no_submit,readonly_stale_identity"
        RESULT_NAMESPACE = "approval"
        RESULT_PREFIX = "Approval choices smoke passed:"
        APP_CLASSES = {"Lapp/zerus/mobile/QuestionUiKt;", "Lapp/zerus/mobile/QuestionPolicies;", "Lapp/zerus/mobile/ApprovalReview;"}


def adb_server_command(adb):
    # ADB's listener parser accepts the port-only spec; omitting -a keeps localhost.
    return [str(adb), "-L", "tcp:5037", "nodaemon", "server"]


def package_absence_command(device, package):
    assert package in {"app.zerus.mobile", "app.zerus.mobile.test"}
    # `pm path` returns 1 for the desired absence; a filtered list succeeds empty.
    return [*device, "pm", "list", "packages", "--user", "0", package]


def package_absence_result(stdout, stderr):
    assert not stdout.strip() and not stderr.strip(), "fresh profile package absence"


def java_release(text):
    """Exact published base/build, with only the recognized optional LTS suffix."""
    assert len(text.encode("utf-8")) <= 65536, "java_release_size"
    fields = {}
    for line in text.splitlines():
        if not line:
            continue
        match = re.fullmatch(r'([A-Z][A-Z0-9_]*)="([^"\r\n]*)"', line)
        assert match and match[1] not in fields, "java_release_fields"
        fields[match[1]] = match[2]
    full = r"17\.0\.20\.1\+1(?:-LTS)?"
    assert fields.get("JAVA_VERSION") == "17.0.20.1" or re.fullmatch(full, fields.get("JAVA_VERSION", "")), "JAVA_VERSION"
    proof = bool(re.fullmatch(full, fields["JAVA_VERSION"]))
    for name in ("JAVA_RUNTIME_VERSION", "IMPLEMENTOR_VERSION", "FULL_VERSION", "SEMANTIC_VERSION", "JVM_VERSION"):
        if name not in fields:
            continue
        pattern = (r"(?:Temurin-)?" if name == "IMPLEMENTOR_VERSION" else "") + full
        assert re.fullmatch(pattern, fields[name]), name
        proof = True
    assert proof, "java_build_unknown"
    return {name: fields[name] for name in ("JAVA_VERSION", "JAVA_RUNTIME_VERSION", "IMPLEMENTOR_VERSION", "FULL_VERSION", "SEMANTIC_VERSION", "JVM_VERSION", "IMPLEMENTOR") if name in fields}


def validate_inputs(environment, pins):
    """Versions are a reviewed selection, never an instruction to take latest."""
    assert re.fullmatch(r"[0-9a-f]{40}", environment["EXPECTED_SOURCE"])
    assert pins["schema"] == 1 and set(pins["packages"]) == set(REVISIONS)
    for package, key in (("emulator", "EMULATOR"), (IMAGE, "SYSTEM_IMAGE")):
        selected = pins["packages"][package]
        expected = environment["EXPECTED_" + key + "_REVISION"]
        assert re.fullmatch(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)", expected)
        assert expected == REVISIONS[package] == selected["revision"]
        assert environment["REQUESTED_" + key + "_REVISION"] == expected
        assert selected["channel"] == "channel-0"
        archive = selected["archive"]
        assert archive["checksum_type"] == "sha1" and re.fullmatch(r"[0-9a-f]{40}", archive["checksum"])
        assert type(archive["bytes"]) is int and 0 < archive["bytes"] <= 2 * 1024 ** 3
        assert re.fullmatch(r"https://dl\.google\.com/android/repository/(?:sys-img/google_apis/)?[A-Za-z0-9_.-]+\.zip", archive["url"])


def approval_progress_order():
    """Diagnostic order only; no status record can replace the final result oracle."""
    assert RESULT_NAMESPACE == "approval" and len(CASES.split(",")) == 7
    setup = [(stage, "-", 0) for stage in ("ON_START", "STORE_BEGIN", "STORE_READY", "BOOTSTRAP_BEGIN",
             "ACTIVITY_LAUNCH_BEGIN", "ACTIVITY_LAUNCH_READY", "BOOTSTRAP_READY")]
    return setup + [(stage, case, index + (stage == "CASE_COMPLETE")) for index, case in enumerate(CASES.split(","))
                    for stage in ("CASE_BEGIN", "CASE_COMPLETE")]


def instrumentation_result(text):
    """Require the one complete custom-runner result, including every case ID."""
    fields, codes = {}, []
    progress, pending = [], None
    for line in text.splitlines():
        if not line:
            continue
        if line.startswith("INSTRUMENTATION_STATUS: "):
            assert RESULT_NAMESPACE == "approval" and not fields and not codes and pending is None and len(progress) < 32
            key, separator, value = line.removeprefix("INSTRUMENTATION_STATUS: ").partition("=")
            assert key == "zerus_approval_progress" and separator and len(value) <= 160
            match = re.fullmatch(r"1\|([1-9][0-9]?)\|([A-Z_]+)\|(-|[a-z_]+)\|([0-7])\|(0|[1-9][0-9]{0,18})", value)
            assert match is not None
            sequence, stage, case, completed, elapsed = match.groups()
            sequence, completed, elapsed = int(sequence), int(completed), int(elapsed)
            order = approval_progress_order()
            assert sequence == len(progress) + 1 and sequence <= len(order)
            assert (stage, case, completed) == order[sequence - 1]
            assert elapsed <= 2 ** 63 - 1 and (not progress or elapsed >= progress[-1])
            pending = elapsed
            continue
        if line.startswith("INSTRUMENTATION_STATUS_CODE: "):
            assert pending is not None and line == "INSTRUMENTATION_STATUS_CODE: 1"
            progress.append(pending)
            pending = None
            continue
        assert pending is None, "truncated approval progress frame"
        if line.startswith("INSTRUMENTATION_CODE: "):
            codes.append(line.removeprefix("INSTRUMENTATION_CODE: "))
            continue
        assert line.startswith("INSTRUMENTATION_RESULT: "), line
        key, separator, value = line.removeprefix("INSTRUMENTATION_RESULT: ").partition("=")
        assert separator and key not in fields and key in {RESULT_NAMESPACE + "_cases", RESULT_NAMESPACE + "_case_ids", "stream"}
        fields[key] = value
    assert codes == ["-1"] and set(fields) == {RESULT_NAMESPACE + "_cases", RESULT_NAMESPACE + "_case_ids", "stream"}
    assert fields[RESULT_NAMESPACE + "_cases"] == str(len(CASES.split(","))) and fields[RESULT_NAMESPACE + "_case_ids"].split(",") == CASES.split(",")
    assert fields["stream"].startswith(RESULT_PREFIX)
    assert pending is None
    if RESULT_NAMESPACE == "approval":
        assert len(progress) == len(approval_progress_order())
    return fields


def phone_service_ready(run, alive, device, boot_end):
    """Require the already-used phone service within the original shared boot deadline."""
    index = 0
    while time.monotonic() < boot_end:
        alive()
        remaining = boot_end - time.monotonic()
        assert remaining > 0, "phone service boot deadline"
        result = run("phone-ready-" + str(index), [*device, "service", "check", "phone"], min(10, remaining))
        assert time.monotonic() < boot_end, "phone service boot deadline"
        if result.strip() == "Service phone: found":
            return
        assert result.strip() == "Service phone: not found", "unknown phone service readiness"
        index += 1
        time.sleep(min(1, max(0, boot_end - time.monotonic())))
    raise AssertionError("phone service boot deadline")


def failure_diagnostics(run, alive, device):
    """One bounded read on the still-owned emulator; never replace the primary failure."""
    try:
        alive()
        run("failure-logcat", [*device, "logcat", "-d", "-b", "crash", "-b", "main", "-b", "system", "-t", "400",
                               "ZerusApprovalUI:I", "AndroidRuntime:E", "*:S"], 5)
        return {"status": "captured", "scope": "owned disposable emulator, last400 crash/main/system records, fixture/runtime tags only"}
    except BaseException as error:
        return {"status": "unknown", "error": type(error).__name__}


# BEGIN APPROVAL_FOCUS_DIAGNOSTIC definition
# Android 15 source references, not installed-image equivalence or a focus-owner conclusion:
# frameworks/native @ android-15.0.0_r1, services/inputflinger/dispatcher/EventLogTags.logtags
# (SHA256 c7e8aedc514b4ad36b9606b00e106a35aa3542852e566f6c790a118bba669dfb) defines input_focus=62001.
# InputDispatcher.cpp at the same tag writes entering/leaving channel names and reasons at lines1720-1723;
# InputMonitor.java writes focus requests. frameworks/base/core/java/android/app/EventLogTags.logtags
# (SHA256 5963f351940253b23bd595d8af3c371c3cc56832e04adfb846279ef870d12468) defines the five lifecycle tags.
# Sources: https://android.googlesource.com/platform/frameworks/native/+/android-15.0.0_r1/services/inputflinger/dispatcher/EventLogTags.logtags
# https://android.googlesource.com/platform/frameworks/base/+/android-15.0.0_r1/core/java/android/app/EventLogTags.logtags
FOCUS_EVENT_TAGS = ("input_focus", "wm_on_resume_called", "wm_on_paused_called", "wm_on_stop_called",
                    "wm_on_top_resumed_gained_called", "wm_on_top_resumed_lost_called")


def focus_event_observation(text):
    """Summarize a bounded metadata tail; the unchanged raw artifact retains channel/component names."""
    assert len(text.encode("utf-8")) <= 65536, "focus event byte cap"
    counts = dict.fromkeys(FOCUS_EVENT_TAGS, 0)
    times, headers = [], 0
    for line in text.splitlines():
        if not line:
            continue
        assert len(line) <= 1024, "focus event line cap"
        if line == "--------- beginning of events":
            headers += 1
            assert headers == 1 and not times, "focus event header"
            continue
        match = re.fullmatch(r"\s*([0-9]{1,12})\.([0-9]{3})\s+[0-9]{1,10}\s+[0-9]{1,10}\s+[IWEF]\s+"
                             r"([a-z_]+)\s*:\s*(\[[ -~]{1,768}\])", line)
        assert match and match[3] in counts, "unknown focus event metadata"
        at = int(match[1]) * 1000 + int(match[2])
        assert len(times) < 128 and (not times or at >= times[-1]), "focus event order/count"
        times.append(at)
        counts[match[3]] += 1
    return {"status": "captured" if times else "empty", "records": len(times), "focus_records": counts["input_focus"],
            "tags": counts, "first_monotonic_ms": times[0] if times else None, "last_monotonic_ms": times[-1] if times else None,
            "scope": "last128 events-buffer records before tag filtering; metadata only, no complete-history or focus-owner claim"}


def focus_failure_diagnostics(run, alive, device):
    """One independent failure-only read within the original run deadline; never retries or changes UI state."""
    try:
        alive()
        text = run("failure-focus-events", [*device, "logcat", "-d", "-b", "events", "-v", "threadtime", "-v", "monotonic",
                                            "-v", "printable", "-t", "128", *[tag + ":I" for tag in FOCUS_EVENT_TAGS], "*:S"], 5)
        return focus_event_observation(text)
    except BaseException as error:
        return {"status": "unknown", "error": type(error).__name__, "scope": "failure-only focus metadata capture; raw output kept when available"}


# END APPROVAL_FOCUS_DIAGNOSTIC definition
def certificate_digest(text):
    lines = [line for line in text.splitlines() if "certificate SHA-256 digest:" in line]
    assert len(lines) == 1
    match = re.fullmatch(r"Signer #1 certificate SHA-256 digest: ([0-9A-Fa-f]{64})", lines[0])
    assert match
    return match[1].lower()


def matching_signers(app_output, test_output):
    app, test = certificate_digest(app_output), certificate_digest(test_output)
    assert app == test
    return app


def instrumentation_manifest(text):
    blocks = re.split(r"\bE: instrumentation(?:\s|$)", text)
    assert len(blocks) == 2
    block = blocks[1].split("E:", 1)[0]
    for attribute, expected in (("name", RUNNER), ("targetPackage", "app.zerus.mobile")):
        values = re.findall(r'android:' + attribute + r'(?:\([^\n=]*\))?="([^"\n]*)"', block)
        assert values == [expected], (attribute, values)


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def revision(path):
    root = ET.parse(path).getroot()
    rev = next(item for item in root.iter() if item.tag.rsplit("}", 1)[-1] == "revision")
    fields = {item.tag.rsplit("}", 1)[-1]: item.text for item in rev}
    value = ".".join(fields.get(name, "0") for name in ("major", "minor", "micro"))
    assert re.fullmatch(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)", value)
    return value


def defined_classes(apk):
    """Class definitions -> type IDs -> string IDs; descriptor text alone is no proof."""
    classes = set()
    with zipfile.ZipFile(apk) as archive:
        names = archive.namelist()
        assert len(names) == len(set(names)) and len(names) <= 10000
        dex_names = [name for name in names if re.fullmatch(r"classes(?:[2-9]|[1-9][0-9]+)?\.dex", name)]
        assert "classes.dex" in dex_names
        for name in dex_names:
            info = archive.getinfo(name)
            assert info.file_size <= 64 * 1024 * 1024
            data = archive.read(name)
            assert data[:4] == b"dex\n" and data[7] == 0 and len(data) >= 112
            def word(offset):
                assert 0 <= offset <= len(data) - 4
                return struct.unpack_from("<I", data, offset)[0]
            assert word(32) == len(data) and word(40) == 0x12345678
            strings, string_at, types, type_at, count, class_at = [word(offset) for offset in (56, 60, 64, 68, 96, 100)]
            assert strings <= 1000000 and types <= 1000000 and count <= 1000000
            assert string_at + 4 * strings <= len(data) and type_at + 4 * types <= len(data) and class_at + 32 * count <= len(data)
            for index in range(count):
                type_id = word(class_at + index * 32)
                assert type_id < types
                string_id = word(type_at + type_id * 4)
                assert string_id < strings
                offset = word(string_at + string_id * 4)
                for _ in range(5):
                    byte = data[offset]
                    offset += 1
                    if byte < 128:
                        break
                else:
                    raise ValueError("invalid DEX string length")
                end = data.index(0, offset, min(offset + 4096, len(data)))
                descriptor = data[offset:end]
                if descriptor.isascii():
                    classes.add(descriptor.decode("ascii"))
    return classes


def main():
    assert sys.platform == "linux" and os.environ.get("RUNNER_ENVIRONMENT") == "github-hosted"
    assert os.environ.get("ImageVersion") in HOSTED_IMAGES
    pins = json.loads(Path(__file__).with_name("android-project-ui-pins.json").read_text())
    validate_inputs(os.environ, pins)
    source = os.environ["EXPECTED_SOURCE"]
    assert hasattr(os, "pidfd_open") and hasattr(signal, "pidfd_send_signal")
    project = Path.cwd()
    sdk = Path(os.environ["ANDROID_SDK_ROOT"]).resolve(strict=True)
    java = Path(os.environ["JAVA_HOME"]).resolve(strict=True)
    with (java / "release").open("rb") as release:
        raw_release = release.read(65537)
    assert len(raw_release) <= 65536
    java_identity = java_release(raw_release.decode("utf-8"))
    root = Path(tempfile.mkdtemp(prefix="zerus-project-ui-", dir=os.environ["RUNNER_TEMP"]))
    root.chmod(0o700)
    evidence = Path(os.environ["RUNNER_TEMP"]) / "android-project-ui-evidence"
    evidence.mkdir(mode=0o700)
    env = {"PATH": str(java / "bin") + ":/usr/bin:/bin", "JAVA_HOME": str(java), "ANDROID_HOME": str(sdk),
           "ANDROID_SDK_ROOT": str(sdk), "LANG": "C.UTF-8", "GRADLE_OPTS": "-Dorg.gradle.daemon=false -Xmx2048m"}
    for key, directory in {"HOME": "home", "TMPDIR": "tmp", "XDG_CONFIG_HOME": "config", "XDG_DATA_HOME": "data",
                           "XDG_CACHE_HOME": "cache", "ANDROID_USER_HOME": "android", "ANDROID_AVD_HOME": "avd", "GRADLE_USER_HOME": "gradle"}.items():
        path = root / directory
        path.mkdir(mode=0o700)
        env[key] = str(path)
    env["GRADLE_OPTS"] += " -Duser.home=" + env["HOME"]
    end = time.monotonic() + 1200
    children, handles, results = [], [], []
    tools_before = {}
    source_before = {}
    apk_before = {}
    success = False
    instrumentation_started = False
    primary_failure, diagnostics = None, None

    def save(name, value):
        with (evidence / name).open("x") as output:
            json.dump(value, output, sort_keys=True, indent=2)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())

    def run(name, argv, cap=60, stdin=None):
        remaining = min(cap, end - time.monotonic() - 20)
        assert remaining > 0
        out, err = evidence / (name + ".stdout"), evidence / (name + ".stderr")
        save(name + ".command.json", {"argv": list(map(str, argv)), "remaining": remaining})
        with out.open("xb") as stdout, err.open("xb") as stderr:
            completed = subprocess.run(list(map(str, argv)), cwd=project, env=env, input=stdin, stdin=subprocess.DEVNULL if stdin is None else None, stdout=stdout, stderr=stderr,
                                       timeout=remaining, check=False)
        assert time.monotonic() < end - 20 and out.stat().st_size <= 8 * 1024 * 1024 and err.stat().st_size <= 8 * 1024 * 1024
        results.append({"name": name, "exit": completed.returncode, "stdout_sha256": sha(out), "stderr_sha256": sha(err)})
        assert completed.returncode == 0, name
        return out.read_text(errors="strict")

    def spawn(name, argv):
        assert end - time.monotonic() > 20
        stdout, stderr = (evidence / (name + ".stdout")).open("xb"), (evidence / (name + ".stderr")).open("xb")
        handles.extend((stdout, stderr))
        child = subprocess.Popen(list(map(str, argv)), cwd=project, env=env, stdin=subprocess.DEVNULL,
                                 stdout=stdout, stderr=stderr, start_new_session=True)
        entry = {"name": name, "child": child, "fd": None}
        children.append(entry)  # Own the unreaped child before the fallible pidfd open.
        entry["fd"] = os.pidfd_open(child.pid)
        save(name + ".process.json", {"pid": child.pid, "argv": list(map(str, argv)), "pidfd_open": True})
        return child

    def alive():
        assert time.monotonic() < end - 20
        assert all(entry["child"].poll() is None for entry in children)

    try:
        assert run("source", ["git", "rev-parse", "HEAD"]).strip() == source
        assert run("source-clean", ["git", "status", "--porcelain"]).strip() == ""
        paths = run("source-paths", ["git", "ls-files", "-z"]).split("\0")
        source_before = {name: sha(project / name) for name in paths if name}
        save("source.json", {"commit": source, "files": source_before, "scope": "owned disposable VM; no installed/production/tool-closure claim"})
        save("selected-packages.json", pins)
        save("java-release.json", java_identity)
        sdkmanager = sdk / "cmdline-tools/latest/bin/sdkmanager"
        assert "Pkg.Revision=12.0" in (sdkmanager.parent.parent / "source.properties").read_text().splitlines()
        save("setup-tools.json", {str(path): sha(path) for path in (java / "release", java / "bin/java", sdkmanager,
             project / "mobile/android/gradle/wrapper/gradle-wrapper.jar", project / "mobile/android/gradle/wrapper/gradle-wrapper.properties")})
        run("sdk-packages", [sdkmanager, "--channel=0", "platforms;android-36", "build-tools;36.0.0", "emulator", IMAGE], 240, b"y\n" * 8)
        image = sdk / "system-images/android-35/google_apis/x86_64"
        assert revision(sdk / "emulator/package.xml") == os.environ["EXPECTED_EMULATOR_REVISION"]
        assert revision(image / "package.xml") == os.environ["EXPECTED_SYSTEM_IMAGE_REVISION"]
        assert "Pkg.Revision=37.0.1" in (sdk / "platform-tools/source.properties").read_text().splitlines()
        assert "Pkg.Revision=36.0.0" in (sdk / "build-tools/36.0.0/source.properties").read_text().splitlines()
        adb, emulator = sdk / "platform-tools/adb", sdk / "emulator/emulator"
        build = sdk / "build-tools/36.0.0"
        for path in [adb, emulator, build / "aapt", build / "apksigner", build / "lib/apksigner.jar", sdk / "platforms/android-36/android.jar"]:
            tools_before[str(path)] = sha(path)
        for path in sorted(image.rglob("*")):
            if path.is_file():
                tools_before[str(path)] = sha(path)
        save("tools-before.json", tools_before)
        missing = root / "absent-signing.properties"
        assert not missing.exists()
        gradle = project / "mobile/android/gradlew"
        run("apk-build", [gradle, "-p", project / "mobile/android", "--no-daemon", "--max-workers=2",
            "-PzerusSigningProperties=" + str(missing), "-PzerusInstrumentationRunner=" + RUNNER,
            "assembleDebug", "assembleDebugAndroidTest"], 600)
        stores = [Path(env["ANDROID_USER_HOME"]) / "debug.keystore", Path(env["HOME"]) / ".android/debug.keystore"]
        stores = [path for path in stores if path.exists()]
        assert len(stores) == 1 and not stores[0].is_symlink() and stores[0].is_file()
        store_stat = stores[0].stat()
        assert store_stat.st_uid == os.getuid() and store_stat.st_nlink == 1 and store_stat.st_size > 0
        save("synthetic-signer.json", {"private_store": str(stores[0]), "new_private_parent": True,
                                      "bytes": store_stat.st_size, "mode": oct(store_stat.st_mode & 0o777)})
        app = project / "mobile/android/app/build/outputs/apk/debug/app-debug.apk"
        test = project / "mobile/android/app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk"
        certificate_outputs = []
        for label, apk in (("app", app), ("test", test)):
            text = run(label + "-signer", [build / "apksigner", "verify", "--print-certs", apk])
            certificate_outputs.append(text)
        certificate = matching_signers(*certificate_outputs)
        xml = run("test-manifest", [build / "aapt", "dump", "xmltree", test, "AndroidManifest.xml"])
        instrumentation_manifest(xml)
        assert "L" + RUNNER.replace(".", "/") + ";" in defined_classes(test)
        assert APP_CLASSES <= defined_classes(app)
        for label, apk, package in (("app", app, "app.zerus.mobile"), ("test", test, "app.zerus.mobile.test")):
            assert "package: name='" + package + "'" in run(label + "-package", [build / "aapt", "dump", "badging", apk])
        apk_before = {str(app): sha(app), str(test): sha(test)}
        save("apks.json", {"sha256": apk_before, "certificate_sha256": certificate, "runner": RUNNER,
                           "signer_scope": "new synthetic debug store under fresh Android/HOME state only"})
        assert os.access("/dev/kvm", os.R_OK | os.W_OK)
        for port in (5037, 5554, 5555):
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", port))
        server = spawn("adb-server", adb_server_command(adb))
        ready_end = min(end - 20, time.monotonic() + 5)
        server_ready = False
        while time.monotonic() < ready_end:
            alive()
            try:
                with socket.create_connection(("127.0.0.1", 5037), timeout=min(.2, ready_end - time.monotonic())):
                    server_ready = True
                    break
            except OSError:
                time.sleep(min(.1, max(0, ready_end - time.monotonic())))
        assert server_ready and time.monotonic() < ready_end
        adb_args = [adb, "-H", "127.0.0.1", "-P", "5037"]
        initial = run("devices-before", [*adb_args, "devices", "-l"])
        assert initial.strip() == "List of devices attached" and server.poll() is None
        avdmanager = sdk / "cmdline-tools/latest/bin/avdmanager"
        run("avd-create", [avdmanager, "create", "avd", "-n", "zerus-project-ui", "-k", IMAGE, "-d", "pixel_6"], 60, b"no\n")
        spawn("emulator", [emulator, "-avd", "zerus-project-ui", "-ports", "5554,5555", "-accel", "on", "-cores", "2",
            "-memory", "2048", "-no-window", "-no-audio", "-no-boot-anim", "-no-snapshot", "-wipe-data",
            "-camera-back", "none", "-camera-front", "none", "-gpu", "swiftshader_indirect"])
        boot_end = min(end - 20, time.monotonic() + 180)
        booted, index = False, 0
        while time.monotonic() < boot_end:
            alive()
            listing = run("devices-" + str(index), [*adb_args, "devices", "-l"], 10)
            devices = [line.split()[:2] for line in listing.splitlines()[1:] if line.strip()]
            assert all(len(row) == 2 and row[0] == SERIAL and row[1] in {"offline", "device"} for row in devices) and len(devices) <= 1
            if devices == [[SERIAL, "device"]]:
                boot = run("boot-" + str(index), [*adb_args, "-s", SERIAL, "shell", "getprop", "sys.boot_completed"], min(10, boot_end - time.monotonic()))
                assert time.monotonic() < boot_end
                if boot.strip() == "1":
                    booted = True
                    break
            index += 1
            time.sleep(min(1, max(0, boot_end - time.monotonic())))
        assert booted
        device = [*adb_args, "-s", SERIAL, "shell"]
        phone_service_ready(run, alive, device, boot_end)
        for name, args in (("wifi-off", ["svc", "wifi", "disable"]), ("data-off", ["svc", "data", "disable"]),
                           ("airplane", ["cmd", "connectivity", "airplane-mode", "enable"])):
            alive()
            run(name, [*device, *args])
        network_end = min(end - 20, time.monotonic() + 10)
        offline, index = False, 0
        while time.monotonic() < network_end:
            alive()
            state = run("network-" + str(index), [*device, "dumpsys", "connectivity"], max(0, network_end - time.monotonic()))
            if "Active default network: none" in state and time.monotonic() < network_end:
                offline = True
                break
            index += 1
            time.sleep(min(.2, max(0, network_end - time.monotonic())))
        assert offline
        for package in ("app.zerus.mobile", "app.zerus.mobile.test"):
            alive()
            name = "absent-" + package
            listed = run(name, package_absence_command(device, package))
            package_absence_result(listed, (evidence / (name + ".stderr")).read_text())
        for label, apk in (("app", app), ("test", test)):
            alive()
            installed = run("install-" + label, [*adb_args, "-s", SERIAL, "install", "--no-streaming", apk], 60)
            assert len(re.findall(r"^Success$", installed, re.M)) == 1 and "Failure" not in installed
        alive()
        instrumentation_started = True
        # Passing runs take about a minute; slow standard runners have needed 1.3-1.8 times as long.
        output = run("instrumentation", [*device, "am", "instrument", "-w", "-r", "app.zerus.mobile.test/" + RUNNER], 240)
        alive()
        save("instrumentation-result.json", instrumentation_result(output))
        success = True
    except BaseException as error:
        primary_failure = {"error": type(error).__name__, "message": str(error)[:2048]}
        if RESULT_NAMESPACE == "approval" and instrumentation_started:
            diagnostics = failure_diagnostics(run, alive, device)
            # BEGIN APPROVAL_FOCUS_DIAGNOSTIC capture
            diagnostics["focus"] = focus_failure_diagnostics(run, alive, device)
            # END APPROVAL_FOCUS_DIAGNOSTIC capture
        raise
    finally:
        cleanup = []
        for entry in reversed(children):
            child, fd = entry["child"], entry["fd"]
            try:
                if child.poll() is None:
                    assert fd is not None and end - time.monotonic() > 0
                    signal.pidfd_send_signal(fd, signal.SIGTERM)
                    try:
                        child.wait(timeout=min(8, max(0.001, end - time.monotonic())))
                    except subprocess.TimeoutExpired:
                        signal.pidfd_send_signal(fd, signal.SIGKILL)
                        child.wait(timeout=min(5, max(0.001, end - time.monotonic())))
                cleanup.append({"name": entry["name"], "reaped": child.returncode is not None})
            except BaseException as error:
                success = False
                cleanup.append({"name": entry["name"], "unknown": type(error).__name__})
            finally:
                if fd is not None:
                    os.close(fd)
        for handle in handles:
            handle.close()
        equal = all(sha(path) == digest for path, digest in tools_before.items())
        source_equal = all(sha(project / name) == digest for name, digest in source_before.items())
        apk_equal = all(sha(path) == digest for path, digest in apk_before.items())
        success = success and equal and source_equal and apk_equal and time.monotonic() < end and all(row.get("reaped") for row in cleanup)
        save("outcome.json", {"qualified": success, "commands": results, "cleanup": cleanup, "tools_unchanged": equal, "source_unchanged": source_equal, "apks_unchanged": apk_equal,
                              "primary_failure": primary_failure, "diagnostics": diagnostics,
                              "whole_end_retained": True, "scope": "synthetic " + RESULT_NAMESPACE + " UI only; no installed/provider/tool-closure proof"})
    assert success


if __name__ == "__main__":
    select_suite(sys.argv[1:])
    main()
