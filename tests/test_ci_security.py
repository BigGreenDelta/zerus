"""Exercise the checked-in workflow trust predicate and final success gate."""
import datetime
import io
import json
from itertools import takewhile
import os
from pathlib import Path
import re
import textwrap
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def job(text, name):
    match = re.search(rf"(?ms)^  {name}:\n(.*?)(?=^  [a-zA-Z][\w-]*:|\Z)", text)
    if not match:
        raise AssertionError(f"Missing workflow job: {name}")
    return match[1]


def steps(text):
    """The steps of one job, in order, each starting at its list marker."""
    return re.split(r"(?m)^(?=      - )", text.split("\n    steps:\n", 1)[1])[1:]


def inline_python(text):
    return textwrap.dedent(re.search(r"(?s)python3 - <<'PY'\n(.*?)\n          PY", text)[1])


def field(text, name):
    match = re.search(rf"(?m)^    {name}: (.*)$", text)
    if not match:
        raise AssertionError(f"Missing job field: {name}")
    if match[1] == ">-":
        lines = text[match.end():].splitlines()[1:]
        return " ".join(line.strip() for line in takewhile(lambda line: line.startswith("      "), lines))
    return match[1]


class WorkflowSecurityTests(unittest.TestCase):
    def setUp(self):
        self.ci = (ROOT / ".github/workflows/ci.yml").read_text()

    def dev_selected(self, workflow, name, results, cancelled=False, event='push',
                     ref='refs/heads/main', repository='ufna/zerus', enabled='true', private=False):
        expression = field(job(workflow, name), 'if')
        # Without a status function GitHub adds success() over the dependency
        # chain, including the deliberately skipped PR/manual branch on main.
        self.assertIn('!cancelled()', expression)
        expression = expression.replace('&&', ' and ')
        expression = re.sub(r'!(?!=)', 'not ', expression)
        expression = re.sub(r'\bfalse\b', 'False', expression)
        # Job IDs such as dev-android are not Python identifiers.
        expression = re.sub(r"\bneeds\.([\w-]+)\.", r"needs['\1'].", expression)
        return bool(eval(expression, {'__builtins__': {}}, {
            'cancelled': lambda: cancelled,
            'github': SimpleNamespace(repository=repository, ref=ref, event_name=event,
                                      event=SimpleNamespace(repository=SimpleNamespace(private=private))),
            'vars': SimpleNamespace(DEV_UPDATES_ENABLED=enabled),
            'needs': {key: SimpleNamespace(result=value) for key,value in results.items()},
        }))

    def test_dev_requires_exact_main_gate_without_inheriting_skipped_review(self):
        passed = {'gate':'success', 'automatic':'success', 'dev-android':'success', 'review':'skipped'}
        self.assertTrue(self.dev_selected(self.ci, 'dev', passed))
        for key in ['gate','automatic','dev-android']:
            for result in ['failure','cancelled','skipped']:
                with self.subTest(key=key, result=result):
                    self.assertFalse(self.dev_selected(self.ci, 'dev', {**passed,key:result}))
        for overrides in [dict(cancelled=True), dict(event='pull_request'), dict(event='workflow_dispatch'),
                          dict(ref='refs/heads/feature'), dict(repository='example/zerus'),
                          dict(enabled=''), dict(private=True)]:
            with self.subTest(overrides=overrides):
                self.assertFalse(self.dev_selected(self.ci, 'dev', passed, **overrides))

    def test_dev_reusable_jobs_preserve_status_and_candidate_success_checks(self):
        dev = (ROOT / '.github/workflows/dev.yml').read_text()
        android = (ROOT / '.github/workflows/dev-android.yml').read_text()
        passed = {'desktop':'success', 'review':'skipped'}
        for workflow, name in [(dev,'desktop'),(dev,'channel'),(android,'android')]:
            self.assertTrue(self.dev_selected(workflow,name,passed))
            for overrides in [dict(cancelled=True),dict(event='pull_request'),
                              dict(ref='refs/heads/feature'),dict(repository='example/zerus')]:
                with self.subTest(name=name,overrides=overrides):
                    self.assertFalse(self.dev_selected(workflow,name,passed,**overrides))
        # The caller checks the Android candidate; the tag waits for the desktop seal.
        self.assertEqual(field(job(dev,'channel'),'needs'),'[desktop]')
        for result in ['failure','cancelled','skipped']:
            with self.subTest(result=result):
                self.assertFalse(self.dev_selected(dev,'channel',{**passed,'desktop':result}))
        self.assertNotRegex(dev, r'(?m)^  android:')
        self.assertNotIn('zerus-dev-android', dev)

    def test_android_candidate_builds_with_the_checks_but_only_on_dev_main_pushes(self):
        caller = job(self.ci,'dev-android')
        # No needs: building starts with the checks; the callee waits for the gate.
        self.assertNotRegex(caller, r'(?m)^    needs:')
        self.assertEqual(field(caller,'uses'),'./.github/workflows/dev-android.yml')
        self.assertIn('    permissions:\n      contents: read\n      actions: read\n', caller)
        self.assertTrue(self.dev_selected(self.ci,'dev-android',{}))
        for overrides in [dict(cancelled=True), dict(event='pull_request'), dict(event='workflow_dispatch'),
                          dict(event='schedule'), dict(ref='refs/heads/feature'), dict(repository='example/zerus'),
                          dict(enabled=''), dict(private=True)]:
            with self.subTest(overrides=overrides):
                self.assertFalse(self.dev_selected(self.ci,'dev-android',{},**overrides))
        # A newer main push must never cancel a signed build in progress.
        android = (ROOT / '.github/workflows/dev-android.yml').read_text()
        self.assertNotIn('concurrency', caller)
        self.assertNotIn('concurrency', android)

    def test_dev_passes_only_required_android_secrets_to_protected_build(self):
        workflow = (ROOT / '.github/workflows/dev-android.yml').read_text()
        names = {'ANDROID_KEYSTORE_BASE64','ANDROID_STORE_PASSWORD','ANDROID_KEY_ALIAS',
                 'ANDROID_KEY_PASSWORD','ANDROID_GOOGLE_SERVICES_JSON'}
        caller = job(self.ci,'dev-android')
        passed = dict(re.findall(r'(?m)^      (\w+): \$\{\{ secrets\.(\w+) \}\}$',caller))
        self.assertEqual(passed,{name:name for name in names})
        self.assertNotIn('secrets: inherit',caller)
        header = workflow.split('\npermissions:',1)[0]
        declared = set(re.findall(r'(?m)^      (\w+):\n        required: true$',header))
        self.assertEqual(declared,names)
        android = job(workflow,'android')
        self.assertEqual(field(android,'environment'),'release')
        self.assertEqual(set(re.findall(r'secrets\.(\w+)', workflow)), names)
        self.assertEqual(set(re.findall(r'secrets\.(\w+)', android)), names)
        # Dev assembly and the tag move no longer receive the signer.
        self.assertNotIn('secrets', job(self.ci,'dev'))
        self.assertNotIn('secrets', (ROOT / '.github/workflows/dev.yml').read_text())
        for path in sorted((ROOT / '.github/workflows').glob('*.yml')):
            text = path.read_text()
            if path.name == 'ci.yml':
                text = text.replace(caller, '')
            elif path.name == 'dev-android.yml':
                continue
            for name in names:
                with self.subTest(workflow=path.name, secret=name):
                    self.assertNotIn(name, text)

    def test_dev_main_build_finishes_while_newer_main_waits(self):
        expression = re.search(r'(?m)^  cancel-in-progress: \$\{\{ (.*) \}\}$', self.ci)[1]
        expression = expression.replace('&&', ' and ')
        expression = re.sub(r'!(?!=)', 'not ', expression)
        expression = re.sub(r'\bfalse\b', 'False', expression)
        for event, ref, repository, enabled, private, cancels in [
            ('push', 'refs/heads/main', 'ufna/zerus', 'true', False, False),
            ('push', 'refs/heads/main', 'ufna/zerus', '', False, True),
            ('push', 'refs/heads/main', 'ufna/zerus', 'true', True, True),
            ('push', 'refs/heads/main', 'example/zerus', 'true', False, True),
            ('push', 'refs/heads/feature', 'ufna/zerus', 'true', False, True),
            ('pull_request', 'refs/pull/123/merge', 'ufna/zerus', 'true', False, True),
            ('workflow_dispatch', 'refs/heads/main', 'ufna/zerus', 'true', False, True),
        ]:
            github = SimpleNamespace(repository=repository, ref=ref, event_name=event,
                                     event=SimpleNamespace(repository=SimpleNamespace(private=private)))
            with self.subTest(event=event, repository=repository, enabled=enabled, private=private):
                self.assertEqual(bool(eval(expression, {'__builtins__': {}}, {
                    'github': github, 'vars': SimpleNamespace(DEV_UPDATES_ENABLED=enabled)})), cancels)

    def test_dev_publisher_accepts_only_successful_main_events_or_main_retry(self):
        workflow = (ROOT / '.github/workflows/publish-dev.yml').read_text()
        expression = field(job(workflow, 'publish'), 'if').replace('&&', ' and ').replace('||', ' or ')
        expression = re.sub(r'\bfalse\b', 'False', expression)
        fixtures = [
            ('workflow_run', 'push', 'success', 'main', 'ufna/zerus', 'refs/heads/main', 'true', False, True),
            ('workflow_run', 'push', 'failure', 'main', 'ufna/zerus', 'refs/heads/main', 'true', False, False),
            ('workflow_run', 'push', 'cancelled', 'main', 'ufna/zerus', 'refs/heads/main', 'true', False, False),
            ('workflow_run', 'pull_request', 'success', 'main', 'ufna/zerus', 'refs/heads/main', 'true', False, False),
            ('workflow_run', 'push', 'success', 'feature', 'ufna/zerus', 'refs/heads/main', 'true', False, False),
            ('workflow_run', 'push', 'success', 'main', 'example/zerus', 'refs/heads/main', 'true', False, False),
            ('workflow_run', 'push', 'success', 'main', 'ufna/zerus', 'refs/heads/main', '', False, False),
            ('workflow_run', 'push', 'success', 'main', 'ufna/zerus', 'refs/heads/main', 'true', True, False),
            ('workflow_dispatch', 'push', 'success', 'main', 'ufna/zerus', 'refs/heads/main', 'true', False, True),
            ('workflow_dispatch', 'push', 'success', 'main', 'ufna/zerus', 'refs/heads/feature', 'true', False, False),
            ('pull_request', 'push', 'success', 'main', 'ufna/zerus', 'refs/heads/main', 'true', False, False),
        ]
        for event, source_event, conclusion, branch, repository, ref, enabled, private, allowed in fixtures:
            source = SimpleNamespace(event=source_event, conclusion=conclusion, head_branch=branch,
                                     head_repository=SimpleNamespace(full_name=repository))
            github = SimpleNamespace(repository='ufna/zerus', ref=ref, event_name=event,
                                     event=SimpleNamespace(repository=SimpleNamespace(private=private), workflow_run=source))
            with self.subTest(event=event, source=source, ref=ref, enabled=enabled, private=private):
                self.assertEqual(bool(eval(expression, {'__builtins__': {}}, {
                    'github': github, 'vars': SimpleNamespace(DEV_UPDATES_ENABLED=enabled)})), allowed)
        self.assertEqual(field(job(workflow, 'publish'), 'environment'), 'release')
        self.assertIn('actions: read', workflow)
        self.assertIn('contents: read', workflow)
        self.assertNotRegex(workflow, r'(?m)^\s*(actions|contents): write')
        self.assertEqual(re.search(r'(?m)^cache-mode: (.*)$', workflow)[1], 'none')

    def selected(self, name, event, ref="refs/heads/main", test_ref="", repository="ufna/zerus"):
        # These are the actual committed conditions, evaluated over synthetic
        # event fixtures. No user strings become executable test expressions.
        expression = field(job(self.ci, name), "if")
        expression = expression.replace("&&", " and ").replace("||", " or ")
        expression = re.sub(r"!(?!=)", "not ", expression)
        return bool(eval(expression, {"__builtins__": {}}, {
            "always": lambda: True,
            "github": SimpleNamespace(repository=repository, ref=ref, event_name=event, sha="a" * 40),
            "inputs": SimpleNamespace(ref=test_ref),
        }))

    def test_only_exact_upstream_main_push_or_schedule_can_write_caches(self):
        fixtures = [
            ("push", "refs/heads/main", "", "ufna/zerus", True),
            ("schedule", "refs/heads/main", "a" * 40, "ufna/zerus", True),
            ("push", "refs/heads/main", "refs/pull/123/head", "ufna/zerus", False),
            ("pull_request", "refs/pull/123/merge", "", "ufna/zerus", False),
            ("pull_request_target", "refs/heads/main", "", "ufna/zerus", False),
            ("workflow_dispatch", "refs/heads/main", "", "ufna/zerus", False),
            ("workflow_dispatch", "refs/heads/main", "refs/pull/123/head", "ufna/zerus", False),
            ("workflow_dispatch", "refs/heads/main", "b" * 40, "ufna/zerus", False),
            ("push", "refs/heads/feature", "", "ufna/zerus", False),
            ("push", "refs/heads/main", "", "example/zerus", False),
        ]
        for event, ref, test_ref, repository, trusted in fixtures:
            with self.subTest(event=event, ref=ref, test_ref=test_ref, repository=repository):
                self.assertEqual(self.selected("automatic", event, ref, test_ref, repository), trusted)
                self.assertEqual(self.selected("review", event, ref, test_ref, repository), not trusted)
                self.assertEqual(self.selected("cache-trim", event, ref, test_ref, repository), trusted)
        self.assertEqual(field(job(self.ci, "automatic"), "cache-mode"), "write")
        self.assertEqual(field(job(self.ci, "review"), "cache-mode"), "read")
        for name in ("automatic", "review"):
            self.assertEqual(field(job(self.ci, name), "uses"), "./.github/workflows/checks.yml")
        # A callee cannot request broader permissions than its caller's limit.
        checks = (ROOT / ".github/workflows/checks.yml").read_text()
        self.assertNotRegex(checks, r"(?m)^\s*cache-mode:")

    def test_final_gate_rejects_failure_cancellation_and_all_skipped(self):
        gate = job(self.ci, "gate")
        self.assertEqual(field(gate, "cache-mode"), "none")
        script = re.search(r"(?s)python3 - <<'PY'\n(.*?)\n          PY", gate)[1]
        code = compile(textwrap.dedent(script), "workflow-CI-gate", "exec")
        for first, second, success in [
            ("success", "skipped", True), ("skipped", "success", True),
            ("failure", "skipped", False), ("cancelled", "skipped", False),
            ("skipped", "skipped", False), ("success", "success", False),
            ("success", "failure", False),
        ]:
            results = {"automatic": {"result": first}, "review": {"result": second}}
            with self.subTest(results=results), patch.dict(os.environ, {"RESULTS": json.dumps(results)}):
                if success:
                    exec(code, {})
                else:
                    with self.assertRaises(SystemExit):
                        exec(code, {})

    def test_windows_suite_selection_matches_the_other_full_suites(self):
        checks = (ROOT / ".github/workflows/checks.yml").read_text()
        automatic = "github.event_name == 'push' || github.event_name == 'pull_request' || inputs.run_windows"
        self.assertIn(f"RUN_WINDOWS: ${{{{ {automatic} }}}}", job(checks, "source"))
        self.assertIn("run_windows: ${{ steps.revision.outputs.run_windows }}", job(checks, "source"))
        for name in ("automatic", "review"):
            with self.subTest(caller=name):
                self.assertIn(f"run_windows: ${{{{ {automatic} }}}}", job(self.ci, name))
        # Manual CI selects every suite by default; reusable callers opt in.
        self.assertRegex(self.ci, r"(?m)^      run_windows:\n        description: .+\n        type: boolean\n        default: true$")
        self.assertRegex(self.ci, r"(?m)^      run_windows:\n        type: boolean\n        default: false$")
        nightly = (ROOT / ".github/workflows/nightly.yml").read_text()
        self.assertIn("      run_windows: true\n", job(nightly, "verify"))
        self.assertIn("name: windows-x86_64-${{ github.sha }}", job(nightly, "assemble"))
        self.assertIn('--windows-zip "${portables[0]}"', job(nightly, "assemble"))
        # A stable Windows asset is a separate decision: release checks skip it.
        self.assertNotIn("run_windows", (ROOT / ".github/workflows/release.yml").read_text())

    def test_windows_job_is_bounded_credential_free_and_pinned(self):
        windows = job((ROOT / ".github/workflows/checks.yml").read_text(), "windows-desktop")
        self.assertEqual(field(windows, "if"), "needs.source.outputs.run_windows == 'true'")
        self.assertEqual(field(windows, "runs-on"), "windows-2025")
        self.assertRegex(field(windows, "timeout-minutes"), r"^\d+$")
        self.assertNotIn("secrets.", windows)
        self.assertNotIn("permissions", windows)
        # Line endings are fixed before checking out the exact tested commit.
        self.assertLess(windows.index("core.autocrlf false"), windows.index("actions/checkout@"))
        self.assertIn("ref: ${{ needs.source.outputs.commit }}\n          persist-credentials: false", windows)
        # One exact interpreter; Qt's installer runs only from hash-locked wheels
        # in its own environment, and its cache key covers that lock.
        self.assertIn("uses: actions/setup-python@5fda3b95a4ea91299a34e894583c3862153e4b97 # v7.0.0\n"
                      "        with:\n          python-version: '3.13.15'\n", windows)
        self.assertNotIn("install-qt-action", windows)
        self.assertEqual(windows.count("pip install"), 1)
        self.assertIn('"$aqt_python" -m pip install --no-input --disable-pip-version-check --require-hashes '
                      '--only-binary :all: \\\n            -r scripts/ci/aqt-requirements.txt </dev/null', windows)
        self.assertIn('python -m venv "$RUNNER_TEMP/aqt"', windows)
        self.assertIn('"$aqt_python" -m aqt install-qt windows desktop 6.10.3 win64_msvc2022_64 \\\n'
                      '            -m qtmultimedia --outputdir "$RUNNER_TEMP/Qt" </dev/null', windows)
        # Qt WebEngine is an optional Linux plugin; the Windows bundle never ships it.
        self.assertNotIn("qtwebengine", windows)
        self.assertIn("if: steps.qt-cache.outputs.cache-hit != 'true'", windows)
        self.assertIn("key: windows-qt-6.10.3-win64_msvc2022_64-qtmultimedia-"
                      "${{ hashFiles('scripts/ci/aqt-requirements.txt') }}", windows)
        # A new stable Rust release must not fail an unchanged commit.
        self.assertIn("cargo +1.85.0 clippy --locked --all-targets -- -D warnings", windows)
        self.assertNotRegex(windows, r"(?m)^\s*cargo clippy")
        self.assertIn("rustup toolchain install 1.85.0 --profile minimal --component clippy", windows)
        # The runner's stdin is a synchronous pipe that Windows OpenSSH never exits on.
        for line in ("cargo test --locked </dev/null", "test_windows_client.py -v </dev/null",
                     "bash scripts/ci/gui.sh build </dev/null", "bash scripts/ci/gui.sh test </dev/null",
                     "--output artifacts/zerus-windows-x86_64.zip </dev/null",
                     "--run --selftest \\\n            </dev/null"):
            self.assertIn(line, windows)
        for line in ("name: windows-x86_64-${{ needs.source.outputs.commit }}",
                     "retention-days: ${{ github.event_name == 'schedule' && 3 || 7 }}",
                     "check-windows-portable.py artifacts/zerus-windows-x86_64.zip --run"):
            self.assertIn(line, windows)

    def test_android_check_runs_in_every_call_without_credentials(self):
        checks = (ROOT / ".github/workflows/checks.yml").read_text()
        android = job(checks, "android")
        self.assertEqual(field(android, "name"), "Android unit tests and lint")
        self.assertEqual(field(android, "runs-on"), "ubuntu-24.04")
        self.assertRegex(field(android, "timeout-minutes"), r"^\d+$")
        # PR, main, nightly and release verification calls all run it, and the gate requires it.
        self.assertNotRegex(android, r"(?m)^    if:")
        self.assertIn("'android': True,", job(checks, "gate"))
        # No signer, Firebase project, protected environment or broader token.
        # continue-on-error would report a failed check to the gate as success.
        self.assertRegex(checks, r"(?m)^permissions:\n  contents: read\n\njobs:\n")
        for text in ("secrets", "vars.", "environment", "permissions", "github.token", "ANDROID_", "continue-on-error"):
            with self.subTest(forbidden=text):
                self.assertNotIn(text, android.replace("$ANDROID_HOME/", ""))
        self.assertEqual(re.findall(r"uses: ([\w/-]+)@[0-9a-f]{40} # v", android),
                         ["actions/checkout", "actions/cache/restore", "actions/cache/save", "actions/upload-artifact"])
        listed = self.checks_steps("android")
        names = [re.match(r"      - (?:name|uses): (.*)", step)[1] for step in listed]
        configure, gradle = (names.index("JDK 17 and synthetic Firebase configuration"),
                             names.index("Unit tests and release lint"))
        self.assertLess(configure, gradle)
        self.assertLess(names.index("Restore Gradle cache"), gradle)
        self.assertLess(gradle, names.index("Save Gradle cache"))
        # The signed build's toolchain and Firebase variant, with placeholder values only.
        signed = self.dev_android()
        sdkmanager = r'(?m)^ +"\$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" (.+)$'
        self.assertEqual(re.findall(sdkmanager, android), re.findall(sdkmanager, signed))
        self.assertIn('echo "JAVA_HOME=$JAVA_HOME_17_X64" >> "$GITHUB_ENV"', listed[configure])
        self.assertIn("os.environ['JAVA_HOME_17_X64']", signed)
        self.assertIn("        working-directory: mobile/android\n"
                      "        run: ./gradlew --no-daemon testDebugUnitTest lintRelease -PzerusFirebase=true\n", listed[gradle])
        self.assertIn("-PzerusFirebase=true", signed)
        config = re.search(r"(?s)cat > mobile/android/app/google-services\.json <<'JSON'\n(.*?)\n          JSON\n",
                           listed[configure])[1]
        self.assertEqual(json.loads(textwrap.dedent(config)), {
            "project_info": {"project_number": "000000000000", "project_id": "zerus-ci-example"},
            "client": [{"client_info": {"mobilesdk_app_id": "1:000000000000:android:0000000000000000",
                                        "android_client_info": {"package_name": "app.zerus.mobile"}},
                        "api_key": [{"current_key": "synthetic-placeholder"}]}],
            "configuration_version": "1"})
        # Reports leave the job only after a failure, from inside artifacts/.
        for step in listed[-2:]:
            self.assertIn("        if: failure()\n", step)
        self.assertIn("name: android-results\n          path: artifacts/test-results\n", listed[-1])

    def test_android_gradle_cache_is_a_monthly_content_keyed_family(self):
        restore = self.checks_step("android", "gradle-cache")
        self.assertIn('echo "epoch=$(date -u +%Y-%m)" >> "$GITHUB_OUTPUT"', self.checks_step("android", "gradle"))
        self.assertEqual(self.step_paths(restore), ["~/.gradle/caches", "~/.gradle/wrapper"])
        family = "android-gradle-v1-${{ steps.gradle.outputs.epoch }}-"
        content = ("${{ hashFiles('mobile/android/gradle/wrapper/gradle-wrapper.properties', "
                   "'mobile/android/**/*.gradle.kts') }}")
        self.assertIn(f"          key: {family}{content}\n          restore-keys: {family}\n", restore)
        # cache-trim groups main entries by the key without its last segment:
        # that segment must be the content hash, so the month stays in the family.
        rendered = (family + content).replace("${{ steps.gradle.outputs.epoch }}", "2026-10").replace(content, "f" * 64)
        self.assertEqual(rendered.rsplit("-", 1)[0], "android-gradle-v1-2026-10")
        checks = (ROOT / ".github/workflows/checks.yml").read_text()
        self.assertEqual(checks.count("android-gradle-v1-"), 2)

    def test_qt_installer_dependencies_are_exact_hashed_wheels(self):
        lock = (ROOT / "scripts/ci/aqt-requirements.txt").read_text()
        lines = [line for line in lock.splitlines() if not line.lstrip().startswith("#")]
        requirements = lines[0::2]
        hashes = lines[1::2]
        self.assertEqual(len(requirements), len(hashes))
        pins = {}
        for requirement, digest in zip(requirements, hashes):
            # No options (index URLs, editable or local paths), only name==version and one hash.
            self.assertRegex(requirement, r"^[a-z0-9][a-z0-9.-]*==[0-9][A-Za-z0-9.]* \\$")
            self.assertRegex(digest, r"^    --hash=sha256:[0-9a-f]{64}$")
            name, version = requirement[:-2].split("==")
            self.assertNotIn(name, pins)
            pins[name] = version
        self.assertEqual((pins.get("aqtinstall"), pins.get("py7zr")), ("3.3.0", "1.1.3"))

    def test_checks_gate_requires_exactly_the_selected_suites(self):
        gate = job((ROOT / ".github/workflows/checks.yml").read_text(), "gate")
        self.assertIn("needs: [source, quick, relay, android, cli, arch, arch-package, mac-desktop, windows-desktop]", gate)
        self.assertIn("RUN_WINDOWS: ${{ needs.source.outputs.run_windows == 'true' }}", gate)
        script = re.search(r"(?s)python3 - <<'PY'\n(.*?)\n          PY", gate)[1]
        code = compile(textwrap.dedent(script), "workflow-checks-gate", "exec")
        base = {"source": "success", "quick": "success", "relay": "success", "android": "success", "cli": "success",
                "arch": "success", "arch-package": "success", "mac-desktop": "success"}
        for selected, result, success in [("true", "success", True), ("true", "failure", False),
                                          ("true", "skipped", False), ("true", "cancelled", False),
                                          ("false", "skipped", True), ("false", "success", False)]:
            results = {name: {"result": value} for name, value in {**base, "windows-desktop": result}.items()}
            environment = {"RESULTS": json.dumps(results), "RUN_CLI": "true", "RUN_ARCH": "true",
                           "RUN_MACOS": "true", "RUN_WINDOWS": selected}
            with self.subTest(selected=selected, result=result), patch.dict(os.environ, environment):
                if success:
                    exec(code, {})
                else:
                    with self.assertRaises(SystemExit):
                        exec(code, {})
        # Quick checks and the Android check always run; the Arch package is
        # selected exactly with Arch desktop.
        base["windows-desktop"] = "success"
        arch_skipped = {**base, "arch": "skipped", "arch-package": "skipped"}
        for results, run_arch, success in [
            ({**base, "quick": "failure"}, "true", False), ({**base, "quick": "skipped"}, "true", False),
            ({**base, "android": "failure"}, "true", False), ({**base, "android": "skipped"}, "true", False),
            ({**base, "android": "cancelled"}, "true", False), ({**arch_skipped, "android": "skipped"}, "false", False),
            ({**base, "arch-package": "failure"}, "true", False), ({**base, "arch-package": "skipped"}, "true", False),
            ({**base, "arch-package": "cancelled"}, "true", False), (arch_skipped, "false", True),
            ({**arch_skipped, "arch-package": "success"}, "false", False),
            ({**arch_skipped, "arch": "success"}, "false", False),
        ]:
            environment = {"RESULTS": json.dumps({name: {"result": value} for name, value in results.items()}),
                           "RUN_CLI": "true", "RUN_ARCH": run_arch, "RUN_MACOS": "true", "RUN_WINDOWS": "true"}
            with self.subTest(results=results, run_arch=run_arch), patch.dict(os.environ, environment):
                if success:
                    exec(code, {})
                else:
                    with self.assertRaises(SystemExit):
                        exec(code, {})

    def checks_steps(self, name):
        """One checks.yml job's steps, each with its indented body."""
        body = job((ROOT / ".github/workflows/checks.yml").read_text(), name).split("\n    steps:\n", 1)[1]
        return [step for step in re.split(r"(?m)^(?=      - )", body) if step.startswith("      - ")]

    def checks_step(self, name, step_id):
        return next(step for step in self.checks_steps(name) if f"\n        id: {step_id}\n" in step)

    @staticmethod
    def step_paths(step):
        match = re.search(r"(?m)^          path: (.*)$", step)
        if match[1] != "|":
            return [match[1]]
        lines = step[match.end():].splitlines()[1:]
        return [line.strip() for line in takewhile(lambda line: line.startswith("            "), lines)]

    def test_source_only_resolves_and_every_suite_starts_from_it(self):
        checks = (ROOT / ".github/workflows/checks.yml").read_text()
        steps = self.checks_steps("source")
        self.assertEqual(len(steps), 2)
        self.assertIn("uses: actions/checkout@", steps[0])
        self.assertIn("\n        id: revision\n", steps[1])
        quick = job(checks, "quick")
        self.assertEqual(field(quick, "name"), "Quick checks (source and Rust 1.85)")
        for line in ("run: python3 scripts/ci/check-source.py", "-p 'test_*publication.py'", "-p test_ci_security.py",
                     "sha256sum --check --ignore-missing actionlint_1.7.12_checksums.txt",
                     "run: cargo +1.85.0 test --locked"):
            self.assertIn(line, quick)
        for name in ("quick", "relay", "android", "cli", "arch", "arch-package", "mac-desktop", "windows-desktop"):
            with self.subTest(job=name):
                self.assertEqual(field(job(checks, name), "needs"), "source")
                self.assertIn("ref: ${{ needs.source.outputs.commit }}", job(checks, name))
        # Builds use every runner vCPU; recipes leave the job count to Ninja.
        self.assertNotIn("CARGO_BUILD_JOBS", job(checks, "cli"))
        for name, jobs in (("arch", 4), ("mac-desktop", 3), ("windows-desktop", 4)):
            self.assertRegex(job(checks, name), rf"(?m)^      ZERUS_BUILD_JOBS: {jobs}$")
        for recipe in ("packaging/aur/zerus-git/PKGBUILD", "packaging/aur/zerus/PKGBUILD.in"):
            with self.subTest(recipe=recipe):
                text = (ROOT / recipe).read_text()
                self.assertIn("\n    cmake --build build\n", text)
                self.assertNotIn("--parallel", text)
                # The published recipes keep their check() and never use ccache.
                self.assertIn("\ncheck() {\n", text)
                self.assertNotIn("ccache", text)
        self.assertNotIn("--nocheck", (ROOT / "scripts/build-arch-package.sh").read_text())

    def test_arch_package_job_alone_produces_the_candidate_artifact(self):
        checks = (ROOT / ".github/workflows/checks.yml").read_text()
        desktop, package = job(checks, "arch"), job(checks, "arch-package")
        image = re.search(r"(?m)^      image: (archlinux@sha256:[0-9a-f]{64})$", desktop)[1]
        for text in (desktop, package):
            self.assertEqual(field(text, "if"), "needs.source.outputs.run_arch == 'true'")
            self.assertIn(f"      image: {image}\n", text)
        # Dev, nightly and release assembly take the package from this one artifact.
        self.assertEqual(checks.count("name: arch-x86_64-"), 1)
        upload = next(step for step in self.checks_steps("arch-package") if "name: arch-x86_64-" in step)
        self.assertIn("\n        if: always()\n", upload)
        self.assertIn("name: arch-x86_64-${{ needs.source.outputs.commit }}\n          path: artifacts\n", upload)
        for line in ("ZERUS_DEV_BUILD_NUMBER: ${{ github.repository == 'ufna/zerus'", "fetch-depth: 0",
                     "run: runuser -u user -- bash scripts/build-arch-package.sh",
                     "runuser -u user -- python3 scripts/check-installed-package.py"):
            self.assertIn(line, package)
        self.assertLess(package.index("build-arch-package.sh"), package.index("name: arch-x86_64-"))
        for line in ("ZERUS_DEV_BUILD_NUMBER", "build-arch-package.sh", "makepkg", "arch-x86_64"):
            self.assertNotIn(line, desktop)
        self.assertIn("name: arch-desktop-results\n          path: artifacts/test-results\n", desktop)
        for name in ("arch", "mac-desktop", "windows-desktop"):
            text = job(checks, name)
            with self.subTest(job=name):
                build = text.index("scripts/ci/gui.sh build")
                self.assertLess(build, text.index("name: Save ccache"))
                self.assertLess(text.index("name: Save ccache"), text.index("scripts/ci/gui.sh test"))
                self.assertNotRegex(text, r"scripts/ci/gui\.sh(?! (?:build|test)\b)")

    def test_only_trusted_main_checks_request_cache_saves(self):
        self.assertIn("\n      save_caches: true\n", job(self.ci, "automatic"))
        self.assertNotIn("save_caches", job(self.ci, "review"))
        for path in sorted((ROOT / ".github/workflows").glob("*.yml")):
            text = path.read_text()
            with self.subTest(workflow=path.name):
                passed = re.findall(r"(?m)^ +save_caches: *(.*)$", text)
                if path.name == "ci.yml":
                    self.assertEqual(passed, ["true"])
                elif path.name == "checks.yml":
                    self.assertEqual(passed, [""])
                    self.assertRegex(text, r"(?m)^      save_caches:\n        description: .+\n"
                                           r"        type: boolean\n        default: false$")
                else:
                    self.assertNotIn("save_caches", text)

    def test_checks_caches_are_restored_everywhere_and_saved_only_on_request(self):
        checks = (ROOT / ".github/workflows/checks.yml").read_text()
        # No implicit post-job save: every cache is an explicit restore/save pair.
        self.assertEqual(set(re.findall(r"uses: (actions/cache[\w/]*)@", checks)),
                         {"actions/cache/restore", "actions/cache/save"})
        names = re.findall(r"(?m)^  ([a-z][\w-]*):\n", checks.split("\njobs:\n", 1)[1])
        saves = 0
        for name in names:
            restores, saved = {}, []
            for step in self.checks_steps(name):
                if "uses: actions/cache/restore@" in step:
                    self.assertNotRegex(step, r"(?m)^        if:", name)
                    restores[re.search(r"(?m)^        id: (\S+)$", step)[1]] = self.step_paths(step)
                elif "uses: actions/cache/save@" in step:
                    saves += 1
                    with self.subTest(job=name, step=step.splitlines()[0]):
                        condition = re.search(r"(?m)^        if: (.*)$", step)[1]
                        self.assertTrue(condition.startswith("inputs.save_caches && "), condition)
                        self.assertNotIn("always()", condition)
                        source = re.search(r"(?m)^          key: \$\{\{ steps\.([\w-]+)\.outputs\.cache-primary-key \}\}$",
                                           step)[1]
                        # The same paths, so later restores find the saved entry.
                        self.assertEqual(self.step_paths(step), restores[source])
                        saved.append(source)
            with self.subTest(job=name):
                self.assertEqual(sorted(saved), sorted(restores))
        self.assertGreaterEqual(saves, 10)
        # Qt is saved only after the desktop build has used the installation.
        windows = job(checks, "windows-desktop")
        self.assertLess(windows.index("bash scripts/ci/gui.sh build"), windows.index("name: Save Qt 6.10.3"))

    def test_compiler_caches_refresh_every_saving_run_and_rust_caches_do_not(self):
        families = {"arch": "arch-ccache-v1-", "arch-package": "arch-package-ccache-v1-",
                    "mac-desktop": "mac-desktop-ccache-v1-${{ runner.arch }}-",
                    "windows-desktop": "windows-ccache-v1-${{ runner.arch }}-qt6.10.3-"}
        for name, family in families.items():
            text = job((ROOT / ".github/workflows/checks.yml").read_text(), name)
            restore, stats = self.checks_step(name, "ccache-restore"), self.checks_step(name, "ccache")
            with self.subTest(job=name):
                self.assertIn(f"          key: {family}${{{{ github.run_id }}}}\n          restore-keys: {family}\n", restore)
                self.assertRegex(text, r"(?m)^      CCACHE_MAXSIZE: \d+M$")
                self.assertIn("ccache -z", self.checks_step(name, "ccache-start"))
                # Statistics always print; a save needs a successful build that
                # compiled something and keeps only the entries that build used.
                self.assertIn("        if: ${{ !cancelled() }}\n", stats)
                self.assertIn("SAVE: ${{ inputs.save_caches && steps.build.outcome == 'success' }}", stats)
                self.assertIn("--print-stats | awk -F'\\t' '$1 == \"cache_miss\" { print $2 + 0 }'", stats)
                self.assertIn('if [ "$SAVE" = true ] && [ "${misses:-0}" -gt 0 ]; then', stats)
                self.assertIn('ccache --evict-older-than "$(( $(date +%s) - STARTED + 60 ))s"', stats)
                self.assertIn("if: inputs.save_caches && steps.ccache.outputs.save == 'true'",
                              next(step for step in self.checks_steps(name) if "name: Save ccache" in step))
        # Different compiler flags never share a family.
        for first in families.values():
            for second in families.values():
                self.assertTrue(first == second or not first.startswith(second))
        identity = "${{ steps.rust.outputs.rustc }}-${{ steps.rust.outputs.epoch }}-"
        rust = {"quick": ("msrv-${{ runner.os }}-${{ runner.arch }}-", "rustc +1.85.0 -Vv"),
                "cli": ("cli-${{ runner.os }}-${{ runner.arch }}-", "rustc -Vv"),
                "windows-desktop": ("windows-cli-${{ runner.arch }}-", "rustc -Vv")}
        for name, (family, rustc) in rust.items():
            with self.subTest(job=name):
                self.assertIn(f'echo "rustc=$({rustc} | git hash-object --stdin | cut -c1-16)" >> "$GITHUB_OUTPUT"\n'
                              f'          echo "epoch=$(date -u +%Y-%m)" >> "$GITHUB_OUTPUT"\n',
                              self.checks_step(name, "rust"))
                restore = self.checks_step(name, "rust-cache")
                self.assertIn(f"          key: {family}{identity}${{{{ hashFiles('Cargo.lock') }}}}\n"
                              f"          restore-keys: {family}{identity}\n", restore)
        cargo = self.checks_step("arch-package", "cargo-cache")
        self.assertIn("          key: arch-cargo-v1-${{ steps.epoch.outputs.month }}-${{ hashFiles('Cargo.lock') }}\n"
                      "          restore-keys: arch-cargo-v1-${{ steps.epoch.outputs.month }}-\n", cargo)
        for name, step_id in (("quick", "rust-cache"), ("cli", "rust-cache"), ("windows-desktop", "rust-cache"),
                              ("arch-package", "cargo-cache"), ("windows-desktop", "qt-cache"),
                              ("android", "gradle-cache")):
            with self.subTest(job=name, cache=step_id):
                # Exact content keys: 200-800 MB entries are saved only when missing.
                self.assertNotIn("run_id", self.checks_step(name, step_id))
                save = next(step for step in self.checks_steps(name)
                            if f"key: ${{{{ steps.{step_id}.outputs.cache-primary-key }}}}" in step)
                self.assertIn(f"if: inputs.save_caches && steps.{step_id}.outputs.cache-hit != 'true'\n", save)

    def test_windows_ccache_is_an_exact_release_verified_before_use(self):
        windows = job((ROOT / ".github/workflows/checks.yml").read_text(), "windows-desktop")
        self.assertEqual(windows.count("ccache/releases/download"), 1)
        self.assertNotRegex(windows, r"\b(?:choco|winget|scoop)\b")
        install = next(step for step in self.checks_steps("windows-desktop") if "ccache/releases/download" in step)
        version = re.search(r"\n          curl -fsSLO https://github\.com/ccache/ccache/releases/download/"
                            r"v(\d+\.\d+\.\d+)/ccache-\1-windows-x86_64\.zip\n", install)[1]
        # MSVC caching is officially supported from ccache 4.13.
        self.assertGreaterEqual(tuple(map(int, version.split("."))), (4, 13))
        archive = re.escape(f"ccache-{version}-windows-x86_64.zip")
        self.assertRegex(install, rf"\n          echo '[0-9a-f]{{64}}  {archive}' \| sha256sum --check --strict\n")
        verified = install.index("sha256sum --check")
        for later in (f"python -m zipfile -e ccache-{version}-windows-x86_64.zip .", '>> "$GITHUB_PATH"'):
            self.assertLess(verified, install.index(later))
        self.assertLess(install.index("curl -fsSLO"), verified)
        self.assertLess(windows.index(install), windows.index("key: windows-ccache-v1-"))
        self.assertLess(windows.index("key: windows-ccache-v1-"), windows.index("bash scripts/ci/gui.sh build"))

    def test_cache_directories_stay_outside_uploaded_paths(self):
        checks = (ROOT / ".github/workflows/checks.yml").read_text()

        def inside(path, directory):
            return path == directory or path.startswith(directory.rstrip("/") + "/")

        for name in re.findall(r"(?m)^  ([a-z][\w-]*):\n", checks.split("\njobs:\n", 1)[1]):
            steps = self.checks_steps(name)
            cached = [path for step in steps if "uses: actions/cache/" in step for path in self.step_paths(step)]
            uploaded = [path for step in steps if "uses: actions/upload-artifact@" in step for path in self.step_paths(step)]
            for upload in uploaded:
                with self.subTest(job=name, upload=upload):
                    self.assertTrue(inside(upload, "artifacts"), upload)
                    for path in cached:
                        self.assertFalse(inside(path, upload) or inside(upload, path), path)
            for path in cached:
                if "ccache" in path:
                    # Compiler caches live outside the checkout entirely.
                    self.assertTrue(path.startswith(("/home/user/", "~/Library/Caches/", "${{ runner.temp }}/")), path)
        for name in ("arch", "arch-package"):
            self.assertIn("      CCACHE_DIR: /home/user/.cache/ccache\n", job(checks, name))
        self.assertIn('echo "CCACHE_DIR=$(cygpath -w "$RUNNER_TEMP/ccache")" >> "$GITHUB_ENV"',
                      job(checks, "windows-desktop"))
        # makepkg uses ccache only through a drop-in in the disposable package
        # container, after checking that it changes nothing but ccache.
        package = job(checks, "arch-package")
        guard = package.index("grep -qx 'BUILDENV=(!distcc color !ccache check !sign)' /etc/makepkg.conf")
        self.assertLess(guard, package.index(
            "echo 'BUILDENV=(!distcc color ccache check !sign)' > /etc/makepkg.conf.d/ccache.conf"))
        self.assertEqual(checks.count("makepkg.conf"), 2)

    def test_actions_are_pinned_and_runners_are_standard(self):
        # Standard runners are free for this public repository; larger ones are not.
        labels = {"ubuntu-24.04", "macos-15", "windows-2025"}
        for path in sorted((ROOT / ".github/workflows").glob("*.yml")):
            text = path.read_text()
            with self.subTest(workflow=path.name):
                for action in re.findall(r"(?m)^\s*(?:- )?uses: (\S.*)$", text):
                    if not action.startswith("./"):
                        self.assertRegex(action, r"^[\w.-]+/[\w./-]+@[0-9a-f]{40} # v\d+(?:\.\d+)*$")
                for runner in re.findall(r"(?m)^\s*runs-on: (.+)$", text):
                    if runner != "${{ matrix.os }}":
                        self.assertIn(runner, labels)
                for runner in re.findall(r'"((?:ubuntu|macos|windows)-[^"]+)"', text):
                    self.assertIn(runner, labels)

    def test_dev_candidates_follow_the_whole_main_matrix_but_take_only_the_arch_package(self):
        # Dev starts after the main gate, which selects every suite on a push,
        # Windows included: a Windows failure also holds back Dev.
        selected = dict(re.findall(r"(?m)^      (run_\w+): \$\{\{ (.*) \}\}$", job(self.ci, "automatic")))
        self.assertEqual(set(selected), {"run_linux", "run_arch", "run_macos", "run_windows"})
        for name, expression in selected.items():
            with self.subTest(suite=name):
                self.assertTrue(eval(expression.replace("||", " or "), {"__builtins__": {}}, {
                    "github": SimpleNamespace(event_name="push"), "inputs": SimpleNamespace(**{name: False})}))
        self.assertIn("    needs: [gate, automatic, dev-android]\n", job(self.ci, "dev"))
        # The v2 Dev catalogs have no Windows artifact kind; the portable zip
        # stays a nightly-only GitHub asset.
        dev = (ROOT / ".github/workflows/dev.yml").read_text()
        downloads = re.findall(r"uses: actions/download-artifact@\S+ # v[\d.]+\n        with:\n          name: (.+)", dev)
        self.assertEqual(downloads, ["arch-x86_64-${{ github.sha }}"])
        self.assertEqual(dev.count("actions/download-artifact@"), len(downloads))
        self.assertNotIn("windows-x86_64", dev)

    def test_privileged_publishers_have_no_cache_access(self):
        for name in ("publish.yml", "publish-nightly.yml", "dev.yml", "dev-android.yml", "publish-dev.yml"):
            with self.subTest(workflow=name):
                text = (ROOT / ".github/workflows" / name).read_text()
                self.assertRegex(text, r"(?m)^cache-mode: none$")
                self.assertNotRegex(text, r"actions/cache[@/]")
                self.assertNotRegex(text, r"(?m)^    cache-mode:")

    def dev_android(self):
        return job((ROOT / ".github/workflows/dev-android.yml").read_text(), "android")

    def test_android_candidate_is_uploaded_only_after_this_runs_ci_gate(self):
        android = self.dev_android()
        listed = steps(android)
        names = [re.match(r"      - (?:name|uses): (.*)", step)[1] for step in listed]
        remove, wait = "Remove private signing inputs before waiting", "Wait for this run's CI gate"
        self.assertEqual(names[-3:-1], [remove, wait])
        self.assertTrue(names[-1].startswith("actions/upload-artifact@"))
        self.assertEqual(android.count("actions/upload-artifact@"), 1)
        self.assertLess(names.index("Prepare existing Android identity and private Firebase configuration"),
                        names.index("Seal the signed APK and exact source"))
        self.assertLess(names.index("Seal the signed APK and exact source"), names.index(remove))
        # The signer, its properties and the Firebase file are gone before the
        # wait, also after a failed build; the upload needs a successful wait.
        removal, waiting, upload = listed[-3:]
        self.assertIn("        if: always()\n", removal)
        for path in ('"$RUNNER_TEMP/zerus-dev-signing"', "mobile/android/app/google-services.json"):
            self.assertIn(path, removal)
        for step in (waiting, upload):
            self.assertNotRegex(step, r"(?m)^        if:")
        self.assertIn("'CI gate'", waiting)
        for line in ("name: zerus-dev-android-a${{ github.run_attempt }}", "path: artifacts/dev-android",
                     "retention-days: 3", "if-no-files-found: error"):
            self.assertIn(f"          {line}\n", upload)
        # Only the wait step receives the job token; it can only read Actions.
        self.assertEqual([step for step in listed if "github.token" in step], [waiting])
        self.assertIn("    permissions:\n      contents: read\n      actions: read\n", android)
        self.assertEqual(field(android, "environment"), "release")
        # The wait covers the slowest checks (quick checks 20 plus Windows 90)
        # and the gate; the job timeout also covers the build before it.
        minutes = int(re.search(r"time\.monotonic\(\) \+ (\d+) \* 60", waiting)[1])
        self.assertGreaterEqual(minutes, 20 + 90 + 5)
        self.assertGreaterEqual(int(field(android, "timeout-minutes")), minutes + 35)
        # A hung build stays bounded like the former 35-minute job.
        build = listed[names.index("Build the dev APK with the shared signer")]
        self.assertLessEqual(int(re.search(r"(?m)^        timeout-minutes: (\d+)$", build)[1]), 35)

    def test_signed_android_job_only_assembles_after_the_credential_free_checks(self):
        workflow = (ROOT / ".github/workflows/dev-android.yml").read_text()
        listed = steps(self.dev_android())
        build = next(step for step in listed if "./gradlew" in step)
        self.assertEqual(workflow.count("./gradlew"), 1)
        self.assertIn('          ./gradlew --no-daemon assembleRelease \\\n'
                      '            -PzerusFirebase=true -PzerusVersionCode="$version_code" \\\n'
                      '            -PzerusDevBuildNumber="$DEV_RUN_NUMBER"\n', build)
        # Unit tests and lint run in checks.yml, never with the signer.
        self.assertNotRegex(workflow, r"\b(?:test|lint|check|connected)[A-Z]\w*")
        # The wait accepts only ci.yml's CI gate. It requires exactly one
        # checks.yml call, whose own gate requires the Android check.
        self.assertIn("'CI gate'", listed[-2])
        gate = job(self.ci, "gate")
        self.assertEqual(field(gate, "name"), "CI gate")
        self.assertEqual(field(gate, "needs"), "[automatic, review]")
        for name in ("automatic", "review"):
            self.assertEqual(field(job(self.ci, name), "uses"), "./.github/workflows/checks.yml")
        checks = (ROOT / ".github/workflows/checks.yml").read_text()
        suites = job(checks, "gate")
        self.assertIn("android", field(suites, "needs").strip("[]").split(", "))
        self.assertIn("'android': True,", suites)
        self.assertIn("testDebugUnitTest lintRelease -PzerusFirebase=true", job(checks, "android"))

    def test_android_candidate_accepts_only_a_successful_gate_of_this_attempt(self):
        namespace = {}
        exec(compile(inline_python(steps(self.dev_android())[-2]), "workflow-dev-android-wait", "exec"), namespace)
        gate = namespace["gate"]
        def record(attempt, status="completed", conclusion=None, name="CI gate"):
            return {"name": name, "run_attempt": attempt, "status": status, "conclusion": conclusion}
        others = [record(1, conclusion="success", name="Trusted main checks / Suite results"),
                  record(1, conclusion="success", name="Signed Android dev candidate / CI gate")]
        for jobs, attempt, state in [
            ([], 1, "pending"),
            ([record(1, "queued")], 1, "pending"),
            ([record(1, "in_progress")], 1, "pending"),
            ([record(1, conclusion="success")], 1, "success"),
            ([record(1, conclusion="failure")], 1, "failure"),
            ([record(1, conclusion="cancelled")], 1, "cancelled"),
            ([record(1, conclusion="skipped")], 1, "skipped"),
            ([record(1, conclusion="timed_out")], 1, "timed_out"),
            # A failed-job retry reruns a failed gate: the earlier record is not final.
            ([record(1, conclusion="failure")], 2, "pending"),
            ([record(1, conclusion="cancelled")], 2, "pending"),
            ([record(1, conclusion="failure"), record(2, "queued")], 2, "pending"),
            ([record(2, conclusion="failure")], 2, "failure"),
            ([record(1, conclusion="failure"), record(2, conclusion="success")], 2, "success"),
            # A gate that already passed is not rerun with the failed jobs.
            ([record(1, conclusion="success")], 2, "success"),
            ([record(3, conclusion="success")], 2, "invalid attempt"),
            ([record(1, conclusion="success"), record(1, conclusion="success")], 1, "ambiguous"),
        ]:
            with self.subTest(jobs=jobs, attempt=attempt):
                self.assertEqual(gate(others + jobs, attempt), state)

    def test_android_candidate_wait_fails_closed_without_printing_the_token(self):
        code = compile(inline_python(steps(self.dev_android())[-2]), "workflow-dev-android-wait", "exec")
        def page(jobs, total=None):
            body = {"total_count": len(jobs) if total is None else total, "jobs": jobs}
            return SimpleNamespace(returncode=0, stdout=json.dumps(body), stderr="")
        def gate(status, conclusion=None):
            return {"name": "CI gate", "run_attempt": 1, "status": status, "conclusion": conclusion}
        failed = SimpleNamespace(returncode=1, stdout="", stderr="gh: Server Error (HTTP 502)\n")
        filler = [{"name": f"job {n}", "run_attempt": 1, "status": "completed", "conclusion": "success"}
                  for n in range(100)]
        for responses, clock, succeeds in [
            ([page([gate("in_progress")]), page([gate("completed", "success")])], None, True),
            ([page(filler, 101), page([gate("completed", "success")], 101)], None, True),
            ([failed, page([gate("completed", "success")])], None, True),
            ([page([gate("in_progress")]), page([gate("completed", "failure")])], None, False),
            ([page([gate("completed", "cancelled")])], None, False),
            ([failed] * 6, None, False),
            ([page([gate("queued")])], [0, 135 * 60 + 1], False),
        ]:
            calls = []
            def run(command, **kwargs):
                calls.append(command)
                return responses[len(calls) - 1]
            environment = {"RUN_ID": "123", "RUN_ATTEMPT": "1", "GH_TOKEN": "synthetic-token"}
            with (self.subTest(responses=len(responses), succeeds=succeeds),
                  patch.dict(os.environ, environment), patch("subprocess.run", run), patch("time.sleep"),
                  patch("time.monotonic", side_effect=clock or (lambda: 0)),
                  patch("sys.stdout", new_callable=io.StringIO) as output):
                if succeeds:
                    exec(code, {"__name__": "__main__"})
                else:
                    with self.assertRaises(SystemExit) as stopped:
                        exec(code, {"__name__": "__main__"})
                    self.assertIn("nothing uploaded", str(stopped.exception.code))
                self.assertEqual(len(calls), len(responses))
                for command in calls:
                    self.assertEqual(command[:2], ["gh", "api"])
                    self.assertRegex(command[2], r"^repos/ufna/zerus/actions/runs/123/jobs\?filter=all&per_page=100&page=\d$")
                self.assertNotIn("synthetic-token", output.getvalue())

    def test_cache_trim_is_main_only_housekeeping_without_checked_out_code(self):
        trim = job(self.ci, "cache-trim")
        self.assertEqual(field(trim, "needs"), "[automatic]")
        self.assertEqual(field(trim, "if"), "always() && " + field(job(self.ci, "automatic"), "if"))
        self.assertEqual(field(trim, "cache-mode"), "none")
        self.assertEqual(field(trim, "runs-on"), "ubuntu-24.04")
        self.assertLessEqual(int(field(trim, "timeout-minutes")), 10)
        # A housekeeping failure must not fail the run, which gates Publish Dev.
        self.assertEqual(field(trim, "continue-on-error"), "true")
        self.assertIn("    permissions:\n      actions: write\n    steps:\n", trim)
        self.assertEqual(len(re.findall(r"(?m)^\s*actions: write", self.ci)), 1)
        # Only the inline trusted script runs with the write token.
        self.assertNotRegex(trim, r"(?m)^\s*(?:- )?uses:")
        self.assertNotIn("checkout", trim)
        self.assertEqual(len(steps(trim)), 1)
        self.assertIn("GH_TOKEN: ${{ github.token }}", trim)
        self.assertIn("'repos/ufna/zerus/actions/caches'", trim)

    def test_cache_trim_keeps_the_two_newest_main_entries_per_key_family(self):
        namespace = {}
        exec(compile(inline_python(job(self.ci, "cache-trim")), "workflow-cache-trim", "exec"), namespace)
        def entry(number, key, created, ref="refs/heads/main"):
            return {"id": number, "key": key, "ref": ref, "created_at": created, "size_in_bytes": 1}
        qt = "windows-qt-6.10.3-win64_msvc2022_64-qtmultimedia-"
        caches = [
            entry(1, "arch-ccache-v1-100", "2026-10-12T10:00:00.123456Z"),
            entry(2, "arch-ccache-v1-101", "2026-10-12T11:00:00Z"),
            entry(3, "arch-ccache-v1-102", "2026-10-12T12:00:00Z"),
            entry(4, "arch-package-ccache-v1-100", "2026-10-12T10:00:00Z"),
            # Equal timestamps keep the later entry.
            entry(5, "mac-desktop-ccache-v1-ARM64-100", "2026-10-12T10:00:00Z"),
            entry(6, "mac-desktop-ccache-v1-ARM64-101", "2026-10-12T10:00:00Z"),
            entry(7, "mac-desktop-ccache-v1-ARM64-102", "2026-10-12T10:00:00Z"),
            entry(8, "windows-ccache-v1-X64-qt6.10.3-100", "2026-10-12T10:00:00Z"),
            entry(9, "cli-Linux-X64-1a2b3c-2026w41-" + "a" * 64, "2026-10-10T10:00:00Z"),
            entry(10, "cli-Linux-X64-1a2b3c-2026w41-" + "b" * 64, "2026-10-12T10:00:00Z"),
            entry(11, "cli-Linux-X64-1a2b3c-2026w41-" + "c" * 64, "2026-10-11T10:00:00Z"),
            entry(12, "cli-macOS-ARM64-1a2b3c-2026w41-" + "a" * 64, "2026-10-01T10:00:00Z"),
            entry(13, "msrv-Linux-1.85-2026w41-" + "a" * 64, "2026-10-01T10:00:00Z"),
            entry(14, qt + "d" * 64, "2026-10-01T10:00:00Z"),
            entry(15, qt + "e" * 64, "2026-10-12T10:00:00Z"),
            entry(16, "arch-ccache-v1-90", "2026-10-12T11:59:59Z", "refs/pull/7/merge"),
            entry(17, "arch-ccache-v1-91", "2026-10-12T12:00:01Z", "refs/pull/8/merge"),
            entry(18, "arch-ccache-v1-1", "2026-09-01T00:00:00Z", "refs/heads/feature"),
        ]
        now = datetime.datetime(2026, 10, 13, 12, tzinfo=datetime.timezone.utc)
        self.assertEqual(sorted(cache["id"] for cache in namespace["stale"](caches, now)), [1, 5, 9, 16])

    def test_cache_trim_tolerates_concurrent_deletion_and_reports_totals(self):
        code = compile(inline_python(job(self.ci, "cache-trim")), "workflow-cache-trim", "exec")
        caches = [{"id": n, "key": f"arch-ccache-v1-{n}", "ref": "refs/heads/main",
                   "created_at": f"2026-10-0{n}T00:00:00Z", "size_in_bytes": n * 2**20} for n in range(1, 6)]
        pages = [{"total_count": 5, "actions_caches": caches[:3]}, {"total_count": 5, "actions_caches": caches[3:]}]
        done = SimpleNamespace(returncode=0, stdout="", stderr="")
        gone = SimpleNamespace(returncode=1, stdout="{}", stderr="gh: Not Found (HTTP 404)\n")
        denied = SimpleNamespace(returncode=1, stdout="{}", stderr="gh: Resource not accessible by integration (HTTP 403)\n")
        for results, succeeds in [({1: done, 2: gone, 3: done}, True), ({1: done, 2: denied, 3: done}, False)]:
            deleted = []
            def run(command, **kwargs):
                self.assertEqual(command[:2], ["gh", "api"])
                if command[3:] == ["--method", "DELETE"]:
                    number = int(command[2].removeprefix("repos/ufna/zerus/actions/caches/"))
                    deleted.append(number)
                    return results[number]
                page = int(command[2].removeprefix("repos/ufna/zerus/actions/caches?per_page=100&page="))
                return SimpleNamespace(returncode=0, stdout=json.dumps(pages[page - 1]), stderr="")
            with (self.subTest(succeeds=succeeds), patch("subprocess.run", run),
                  patch("sys.stdout", new_callable=io.StringIO) as output):
                if succeeds:
                    exec(code, {"__name__": "__main__"})
                else:
                    with self.assertRaises(SystemExit):
                        exec(code, {"__name__": "__main__"})
                self.assertEqual(sorted(deleted), [1, 2, 3])
                log = output.getvalue()
                self.assertIn("Deleted refs/heads/main arch-ccache-v1-1 (1.0 MiB)\n", log)
                self.assertIn("Deleted refs/heads/main arch-ccache-v1-3 (3.0 MiB)\n", log)
                self.assertIn(("Already deleted " if succeeds else "Not deleted ") + "refs/heads/main arch-ccache-v1-2", log)
                self.assertIn("Deleted 2 of 5 entries, 0.00 of 0.01 GiB.\n", log)

    def test_candidate_verification_has_a_read_only_reusable_chain(self):
        for name in ("release.yml", "nightly.yml"):
            with self.subTest(workflow=name):
                text = (ROOT / ".github/workflows" / name).read_text()
                verify = job(text, "verify")
                self.assertEqual(field(verify, "cache-mode"), "read")
                self.assertEqual(field(verify, "uses"), "./.github/workflows/checks.yml")
                checks = (ROOT / ".github/workflows/checks.yml").read_text()
                # GitHub validates nested permissions even for skipped jobs.
                # A candidate must never request publication write permissions.
                self.assertNotRegex(checks, r"(?m)^\s*contents:\s*write\s*$")
                self.assertNotIn("./.github/workflows/dev.yml", checks)
                self.assertIn("run_linux: true", verify)
                self.assertIn("run_arch: true", verify)
                self.assertIn("run_macos:", verify)


    def test_apple_signing_secrets_stay_in_the_signing_job(self):
        text = (ROOT / ".github/workflows/macos-cask.yml").read_text()
        self.assertRegex(text, r"(?m)^cache-mode: none$")
        self.assertNotRegex(text, r"actions/cache[@/]")
        sign = job(text, "sign")
        self.assertRegex(sign, r"(?m)^    environment: macos-signing$")
        for secret in ("MACOS_DEVELOPER_ID_P12", "MACOS_DEVELOPER_ID_P12_PASSWORD", "APPLE_NOTARY_API_KEY_P8",
                       "APPLE_NOTARY_API_KEY_ID", "APPLE_NOTARY_API_ISSUER_ID"):
            self.assertIn(f"secrets.{secret}", sign)
        self.assertIn("vars.MACOS_TEAM_ID", sign)
        self.assertIn('test "$WORKFLOW_REF" = refs/heads/main', sign)
        self.assertRegex(sign, r"if: always\(\)\n        run: bash scripts/ci/macos-keychain.sh delete")
        for name in ("bundle", "verify"):
            self.assertNotIn("secrets.", job(text, name), name)
            self.assertNotIn("environment:", job(text, name), name)
        self.assertNotIn("environment: release", text)
        for name in ("release.yml", "nightly.yml"):
            workflow = (ROOT / ".github/workflows" / name).read_text()
            self.assertNotIn("MACOS_DEVELOPER_ID", workflow, name)
            self.assertNotIn("APPLE_NOTARY", workflow, name)
            self.assertNotIn("secrets: inherit", workflow, name)

    def assemble_runs(self, workflow, verify, macos, enabled, requested=True):
        expression = field(job(workflow, "assemble"), "if")
        self.assertIn("!cancelled()", expression)
        expression = expression.replace("&&", " and ").replace("||", " or ")
        expression = re.sub(r"!(?!=)", "not ", expression)
        expression = re.sub(r"(?<!')\btrue\b(?!')", "True", expression)
        return bool(eval(expression, {"__builtins__": {}}, {
            "cancelled": lambda: False, "inputs": SimpleNamespace(macos_cask=requested),
            "vars": SimpleNamespace(MACOS_CASKS_ENABLED="true" if enabled else ""),
            "needs": SimpleNamespace(verify=SimpleNamespace(result=verify), macos=SimpleNamespace(result=macos)),
        }))

    def test_casks_are_built_only_once_signing_is_enabled(self):
        for name, channel in (("release.yml", "stable"), ("nightly.yml", "nightly")):
            with self.subTest(workflow=name):
                workflow = (ROOT / ".github/workflows" / name).read_text()
                macos = job(workflow, "macos")
                self.assertEqual(field(macos, "uses"), "./.github/workflows/macos-cask.yml")
                self.assertIn(f"channel: {channel}", macos)
                self.assertIn("vars.MACOS_CASKS_ENABLED == 'true'", field(macos, "if"))
                self.assertIn("verify", field(macos, "needs"))
                for requested in (True, False):
                    if name == "nightly.yml" and not requested:
                        continue
                    for enabled in (True, False):
                        wanted = enabled and requested
                        expected = {"success": True, "skipped": not wanted, "failure": False, "cancelled": False}
                        for result, runs in expected.items():
                            self.assertEqual(self.assemble_runs(workflow, "success", result, enabled, requested), runs,
                                             (requested, enabled, result))
                        self.assertFalse(self.assemble_runs(workflow, "failure", "skipped", enabled, requested))

    def test_release_draft_survives_a_skipped_cask(self):
        workflow = (ROOT / ".github/workflows/release.yml").read_text()
        expression = field(job(workflow, "draft"), "if")
        # Without a status function GitHub adds success() over the whole chain,
        # which would skip the draft whenever the optional cask was skipped.
        self.assertIn("!cancelled()", expression)
        self.assertIn("needs.assemble.result == 'success'", expression)
        self.assertIn("inputs.create_draft", expression)

    def test_tap_deploy_key_is_only_available_to_the_publishers(self):
        for workflow in (ROOT / ".github/workflows").glob("*.yml"):
            text = workflow.read_text()
            if "HOMEBREW_TAP_DEPLOY_KEY" in text:
                self.assertIn(workflow.name, {"publish.yml", "publish-nightly.yml"})
                self.assertRegex(job(text, "publish"), r"(?m)^    environment: release$")
                self.assertIn("--tap-ssh-key", job(text, "publish"))

if __name__ == "__main__":
    unittest.main()
