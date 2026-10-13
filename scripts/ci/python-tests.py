#!/usr/bin/env python3
"""Run integration test classes in parallel fresh processes; fixture globals stay local.

Every terminal fixture owns a private socket and temporary directory, so test
classes run concurrently. They mostly wait on terminals and timers, so
ZERUS_TEST_JOBS defaults to min(CPU count, 16); 1 runs them one after another.
"""
import concurrent.futures
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
TESTS = ROOT / "tests"
RESULTS = ROOT / "artifacts/test-results/python"
DURATIONS = RESULTS / "durations.json"
# Without local durations, those of a main run order the jobs: per job the larger
# of its Linux and macOS cli-results durations.json, in whole seconds.
SEED = Path(__file__).with_name("python-durations.json")
TIMEOUT = 600
# Larger classes run as several jobs of at most this many tests, unless they
# share a class fixture.
CHUNK = 12

# The classes unittest discovery would load from the module, by their names in
# it, with their tests and whether they share a class fixture.
LIST_CLASSES = r'''
import importlib, json, sys, unittest
module = importlib.import_module(sys.argv[1])
loader = unittest.defaultTestLoader
base = unittest.TestCase.setUpClass.__func__
print(json.dumps([dict(name=name, tests=list(loader.getTestCaseNames(value)),
                       shared=value.setUpClass.__func__ is not base)
    for name, value in sorted(vars(module).items())
    if isinstance(value, type) and issubclass(value, unittest.TestCase) and loader.getTestCaseNames(value)]))
'''


def environment():
    return dict(os.environ, PYTHONPATH=os.pathsep.join(filter(None, [str(TESTS), os.environ.get("PYTHONPATH")])))


def jobs_for(module):
    """Jobs as (label, unittest names). A module that cannot be listed runs whole and reports why."""
    result = subprocess.run([sys.executable, "-c", LIST_CLASSES, module], cwd=ROOT, env=environment(),
                            capture_output=True, text=True, timeout=120)
    try:
        classes = json.loads(result.stdout) if result.returncode == 0 else []
    except ValueError:
        classes = []
    jobs = []
    for case in classes:
        name, tests = f"{module}.{case['name']}", case["tests"]
        parts = 1 if case["shared"] else -(-len(tests) // CHUNK)
        if parts == 1:
            jobs.append((name, [name]))
        else:
            jobs += [(f"{name}[{index + 1}/{parts}]", [f"{name}.{test}" for test in tests[index::parts]]) for index in range(parts)]
    return jobs or [(module, [module])]


def run(job):
    job, names = job
    started = time.monotonic()
    command = [sys.executable, "-m", "unittest", "-v", *names]
    try:
        result = subprocess.run(command, cwd=ROOT, env=environment(), capture_output=True, text=True, timeout=TIMEOUT)
        output, failed = result.stdout + result.stderr, result.returncode != 0
    except subprocess.TimeoutExpired as error:
        output = (error.stdout or b"").decode(errors="replace") if isinstance(error.stdout, bytes) else (error.stdout or "")
        output += f"\nFAIL: {job} exceeded {TIMEOUT // 60} minutes.\n"
        failed = True
    return job, output, failed, time.monotonic() - started


def main():
    RESULTS.mkdir(parents=True, exist_ok=True)
    modules = [path.stem for path in sorted(TESTS.glob("test_*.py"))]
    workers = max(1, int(os.environ.get("ZERUS_TEST_JOBS") or min(os.cpu_count() or 1, 16)))
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        jobs = [job for listed in pool.map(jobs_for, modules) for job in listed]
    previous = {}
    for path in (DURATIONS, SEED):
        try:
            previous = json.loads(path.read_text())
            break
        except (OSError, ValueError):
            pass
    # Longest first keeps the slowest classes off the end of the run.
    jobs.sort(key=lambda job: -previous.get(job[0], len(job[1]) if len(job[1]) > 1 else
                                            (TESTS / (job[0].split(".")[0] + ".py")).stat().st_size / 1000))
    print(f"Running {len(jobs)} test jobs from {len(modules)} modules with {workers} parallel jobs", flush=True)
    outputs, failed, durations = {}, set(), {}
    started = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        for future in concurrent.futures.as_completed([pool.submit(run, job) for job in jobs]):
            job, output, job_failed, seconds = future.result()
            print(f"\nRunning {job} ({seconds:.1f} s)", flush=True)
            print(output, end="", flush=True)
            outputs[job], durations[job] = output, round(seconds, 2)
            if job_failed:
                failed.add(job)
    for module in modules:
        parts = sorted(job for job in outputs if job == module or job.startswith(module + "."))
        (RESULTS / f"{module}.log").write_text("".join(f"== {job}\n{outputs[job]}" for job in parts))
    DURATIONS.write_text(json.dumps(durations, indent=1, sort_keys=True) + "\n")
    failed_modules = sorted({job.split(".")[0] + ".py" for job in failed})
    print(f"\nIntegration modules: {len(modules)}, failed: {len(failed_modules)} "
          f"({len(jobs)} jobs in {time.monotonic() - started:.0f} s)")
    if failed:
        print("Failed modules: " + ", ".join(failed_modules))
        print("Failed jobs: " + ", ".join(sorted(failed)))
    return bool(failed)


if __name__ == "__main__":
    sys.exit(main())
