"""Exercise the checked-in workflow trust predicate and final success gate."""
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
        return bool(eval(expression, {'__builtins__': {}}, {
            'cancelled': lambda: cancelled,
            'github': SimpleNamespace(repository=repository, ref=ref, event_name=event,
                                      event=SimpleNamespace(repository=SimpleNamespace(private=private))),
            'vars': SimpleNamespace(DEV_UPDATES_ENABLED=enabled),
            'needs': SimpleNamespace(**{key: SimpleNamespace(result=value) for key,value in results.items()}),
        }))

    def test_dev_requires_exact_main_gate_without_inheriting_skipped_review(self):
        passed = {'gate':'success', 'automatic':'success', 'review':'skipped'}
        self.assertTrue(self.dev_selected(self.ci, 'dev', passed))
        for key in ['gate','automatic']:
            for result in ['failure','cancelled','skipped']:
                with self.subTest(key=key, result=result):
                    self.assertFalse(self.dev_selected(self.ci, 'dev', {**passed,key:result}))
        for overrides in [dict(cancelled=True), dict(event='pull_request'), dict(event='workflow_dispatch'),
                          dict(ref='refs/heads/feature'), dict(repository='example/zerus'),
                          dict(enabled=''), dict(private=True)]:
            with self.subTest(overrides=overrides):
                self.assertFalse(self.dev_selected(self.ci, 'dev', passed, **overrides))

    def test_dev_reusable_jobs_preserve_status_and_candidate_success_checks(self):
        workflow = (ROOT / '.github/workflows/dev.yml').read_text()
        passed = {'desktop':'success', 'android':'success', 'review':'skipped'}
        for name in ['desktop','android','channel']:
            self.assertTrue(self.dev_selected(workflow,name,passed))
            for overrides in [dict(cancelled=True),dict(event='pull_request'),
                              dict(ref='refs/heads/feature'),dict(repository='example/zerus')]:
                with self.subTest(name=name,overrides=overrides):
                    self.assertFalse(self.dev_selected(workflow,name,passed,**overrides))
        for key in ['desktop','android']:
            for result in ['failure','cancelled','skipped']:
                with self.subTest(key=key,result=result):
                    self.assertFalse(self.dev_selected(workflow,'channel',{**passed,key:result}))

    def test_dev_passes_only_required_android_secrets_to_protected_build(self):
        workflow = (ROOT / '.github/workflows/dev.yml').read_text()
        names = {'ANDROID_KEYSTORE_BASE64','ANDROID_STORE_PASSWORD','ANDROID_KEY_ALIAS',
                 'ANDROID_KEY_PASSWORD','ANDROID_GOOGLE_SERVICES_JSON'}
        caller = job(self.ci,'dev')
        passed = dict(re.findall(r'(?m)^      (\w+): \$\{\{ secrets\.(\w+) \}\}$',caller))
        self.assertEqual(passed,{name:name for name in names})
        self.assertNotIn('secrets: inherit',caller)
        header = workflow.split('\npermissions:',1)[0]
        declared = set(re.findall(r'(?m)^      (\w+):\n        required: true$',header))
        self.assertEqual(declared,names)
        self.assertEqual(field(job(workflow,'android'),'environment'),'release')

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
                      '            -m qtwebengine qtwebchannel qtpositioning qtmultimedia --outputdir "$RUNNER_TEMP/Qt" </dev/null', windows)
        self.assertIn("if: steps.qt-cache.outputs.cache-hit != 'true'", windows)
        self.assertIn("key: windows-qt-6.10.3-win64_msvc2022_64-qtwebengine-qtwebchannel-qtpositioning-qtmultimedia-"
                      "${{ hashFiles('scripts/ci/aqt-requirements.txt') }}", windows)
        # A new stable Rust release must not fail an unchanged commit.
        self.assertIn("cargo +1.85.0 clippy --locked --all-targets -- -D warnings", windows)
        self.assertNotRegex(windows, r"(?m)^\s*cargo clippy")
        self.assertIn("rustup toolchain install 1.85.0 --profile minimal --component clippy", windows)
        # The runner's stdin is a synchronous pipe that Windows OpenSSH never exits on.
        for line in ("cargo test --locked </dev/null", "test_windows_client.py -v </dev/null",
                     "bash scripts/ci/gui.sh </dev/null", "--output artifacts/zerus-windows-x86_64.zip </dev/null",
                     "--run --selftest \\\n            </dev/null"):
            self.assertIn(line, windows)
        for line in ("name: windows-x86_64-${{ needs.source.outputs.commit }}",
                     "retention-days: ${{ github.event_name == 'schedule' && 3 || 7 }}",
                     "check-windows-portable.py artifacts/zerus-windows-x86_64.zip --run"):
            self.assertIn(line, windows)

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
        self.assertIn("needs: [source, relay, cli, arch, mac-desktop, windows-desktop]", gate)
        self.assertIn("RUN_WINDOWS: ${{ needs.source.outputs.run_windows == 'true' }}", gate)
        script = re.search(r"(?s)python3 - <<'PY'\n(.*?)\n          PY", gate)[1]
        code = compile(textwrap.dedent(script), "workflow-checks-gate", "exec")
        base = {"source": "success", "relay": "success", "cli": "success", "arch": "success", "mac-desktop": "success"}
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
        self.assertIn("    needs: [gate, automatic]\n", job(self.ci, "dev"))
        # The v2 Dev catalogs have no Windows artifact kind; the portable zip
        # stays a nightly-only GitHub asset.
        dev = (ROOT / ".github/workflows/dev.yml").read_text()
        downloads = re.findall(r"uses: actions/download-artifact@\S+ # v[\d.]+\n        with:\n          name: (.+)", dev)
        self.assertEqual(downloads, ["arch-x86_64-${{ github.sha }}"])
        self.assertEqual(dev.count("actions/download-artifact@"), len(downloads))
        self.assertNotIn("windows-x86_64", dev)

    def test_privileged_publishers_have_no_cache_access(self):
        for name in ("publish.yml", "publish-nightly.yml", "dev.yml", "publish-dev.yml"):
            with self.subTest(workflow=name):
                text = (ROOT / ".github/workflows" / name).read_text()
                self.assertRegex(text, r"(?m)^cache-mode: none$")
                self.assertNotIn("actions/cache@", text)
                self.assertNotRegex(text, r"(?m)^    cache-mode:")

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
        self.assertNotIn("actions/cache@", text)
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
