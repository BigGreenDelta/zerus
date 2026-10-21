#!/usr/bin/env python3
"""AST/memory-only diagnostic controls. Never import test_pause or execute an agent/CLI/tmux/proc read."""
import ast
import copy
import hashlib
import io
import json
from pathlib import Path
import re
from types import SimpleNamespace
import unittest


SOURCE = (Path(__file__).resolve().parent / "test_pause.py").read_text()
TREE = ast.parse(SOURCE)
AGENT = next(ast.literal_eval(node.value) for node in TREE.body if isinstance(node, ast.Assign)
             and any(isinstance(target, ast.Name) and target.id == "AGENT" for target in node.targets))
SELECTED = [node for node in TREE.body if isinstance(node, (ast.FunctionDef, ast.ClassDef))
            and node.name in ("direct_signal_agent", "DirectSignalTrace")]
assert {node.name for node in SELECTED} == {"direct_signal_agent", "DirectSignalTrace"}


def load(**values):
    namespace = {"AGENT": AGENT, "json": json, "hashlib": hashlib, "Path": Path, "re": re,
                 "time": SimpleNamespace(monotonic_ns=lambda: 100), **values}
    exec(compile(ast.Module(body=copy.deepcopy(SELECTED), type_ignores=[]), "<pause-signal-source>", "exec"), namespace)
    return namespace


class MemoryFile(io.BytesIO):
    def __init__(self, value, reads):
        super().__init__(value)
        self.reads = reads

    def read(self, size=-1):
        if size not in (4097, 8193, 8705):
            raise AssertionError("Unbounded diagnostic read")
        self.reads.append(size)
        return super().read(size)


class MemoryPath:
    def __init__(self, value):
        self.value = value

    def __truediv__(self, other):
        return MemoryPath(self.value + "/" + other)

    def __fspath__(self):
        return self.value

    def __str__(self):
        return self.value

    def mkdir(self, **options):
        if options != {"parents": True, "exist_ok": True}:
            raise AssertionError("Unexpected directory operation")


class MemoryArtifact(io.BytesIO):
    name = "/repo/artifacts/test-results/python/direct-signal-memory.json"

    def __init__(self, retained):
        super().__init__()
        self.retained = retained

    def fileno(self):
        return 17

    def __exit__(self, *args):
        self.retained.append(self.getvalue())
        return super().__exit__(*args)


class SignalTraceSource(unittest.TestCase):
    def test_only_the_three_linux_subcases_get_trace_without_changing_the_one_signal(self):
        archive = next(node for node in TREE.body if isinstance(node, ast.ClassDef) and node.name == "Archive")
        method = next(node for node in archive.body if isinstance(node, ast.FunctionDef) and node.name == "test_pause_and_agent_signal_remain_saved")
        text = ast.get_source_segment(SOURCE, method)
        self.assertIn('for agent in ("claude", "codex", "kimi"):', text)
        self.assertIn('trace = DirectSignalTrace(self.root, agent) if sys.platform == "linux" else None', text)
        self.assertEqual(text.count('os.kill(record["pid"], signal.SIGTERM)'), 1)
        self.assertEqual(text.count('self.wait_stopped(name)'), 1)
        self.assertLess(text.index('before_SIGTERM'), text.index('os.kill('))
        self.assertLess(text.index('os.kill('), text.index('after_SIGTERM_returned'))
        self.assertLess(text.index('after_SIGTERM_returned'), text.index('self.wait_stopped(name)'))
        self.assertIn('error.add_note(trace.failure())', text)
        wait = next(node for node in TREE.body if isinstance(node, ast.ClassDef) and node.name == "Harness")
        wait = next(node for node in wait.body if isinstance(node, ast.FunctionDef) and node.name == "wait")
        self.assertIn('until = time.monotonic() + 6', ast.get_source_segment(SOURCE, wait))
        self.assertNotIn('time.sleep(', text)
        self.assertNotIn('signal.SIGKILL', text)

    def test_wrapper_only_adds_failure_swallowing_phase_calls_to_the_exact_original_agent(self):
        wrapped = load()["direct_signal_agent"]()
        tree = ast.parse(wrapped)

        class StripTrace(ast.NodeTransformer):
            def visit_FunctionDef(self, node):
                return None if node.name == "signal_phase" else self.generic_visit(node)

            def visit_Expr(self, node):
                if isinstance(node.value, ast.Call) and isinstance(node.value.func, ast.Name) and node.value.func.id == "signal_phase":
                    return None
                return self.generic_visit(node)

        self.assertEqual(ast.dump(StripTrace().visit(tree)), ast.dump(ast.parse(AGENT)))
        phases = re.findall(r'signal_phase\("([A-Za-z0-9_]+)"\)', wrapped)
        self.assertEqual(phases, ['handler_enter', 'SessionEnd_begin', 'SessionEnd_end', 'handler_exit_143',
                                  'handler_registered', 'SessionStart_begin', 'SessionStart_end'])
        signal_phase = next(node for node in ast.parse(wrapped).body if isinstance(node, ast.FunctionDef) and node.name == "signal_phase")
        self.assertEqual(len(signal_phase.body), 1)
        self.assertIsInstance(signal_phase.body[0], ast.Try)
        self.assertEqual(ast.dump(signal_phase.body[0].handlers[0].type), "Name(id='Exception', ctx=Load())")
        self.assertIsInstance(signal_phase.body[0].handlers[0].body[0], ast.Pass)

    def test_agent_labels_and_paths_are_finite_and_cannot_mix(self):
        namespace = load()
        for agent in ("claude", "codex", "kimi"):
            trace = namespace['DirectSignalTrace'](Path('/fixture'), agent)
            self.assertEqual(trace.agent, agent)
        for agent in ("opencode", "../codex", "codex/other", "", None, False):
            with self.subTest(agent=agent), self.assertRaises(ValueError):
                namespace['DirectSignalTrace'](Path('/fixture'), agent)
        self.assertIn('home / ("direct-signal-agent-" + agent + ".jsonl")', namespace['direct_signal_agent']())
        self.assertIn('self.root / ("direct-signal-agent-" + self.agent + ".jsonl")', SOURCE)
        self.assertNotIn('glob(', ast.get_source_segment(SOURCE, SELECTED[1]))

    def test_phase_logging_errors_never_escape_or_send_a_signal(self):
        wrapped = load()['direct_signal_agent']()
        function = next(node for node in ast.parse(wrapped).body if isinstance(node, ast.FunctionDef) and node.name == "signal_phase")
        for failed in ('getpid', 'clock', 'open', 'fstat', 'write', 'close', None):
            calls = []; written = []

            def operation(name, value=None):
                def invoke(*args, **kwargs):
                    calls.append(name)
                    if name == failed:
                        raise OSError("memory-only fault")
                    if name == 'write':
                        written.append(args[1]); return len(args[1])
                    return value
                return invoke

            memory_os = SimpleNamespace(getpid=operation('getpid', 41), environ={'HGS_RUN_ID': 'run'},
                open=operation('open', 19), fstat=operation('fstat', SimpleNamespace(st_size=0)), write=operation('write'), close=operation('close'),
                O_WRONLY=1, O_CREAT=2, O_APPEND=4, O_CLOEXEC=8, O_NOFOLLOW=16)
            namespace = dict(os=memory_os, time=SimpleNamespace(monotonic_ns=operation('clock', 12)), json=json,
                             home=Path('/fixture'), agent='kimi', sid='conversation')
            exec(compile(ast.Module(body=[copy.deepcopy(function)], type_ignores=[]), '<phase-source>', 'exec'), namespace)
            namespace['signal_phase']('handler_enter')
            if written:
                self.assertEqual(json.loads(written[0])['agent'], 'kimi')
            if failed in ('fstat', 'write'):
                self.assertIn('close', calls)
            self.assertFalse(hasattr(memory_os, 'kill'))

    def test_bounded_status_samples_include_pending_bits_and_preserve_identity_refusal(self):
        def stat(pid, parent, birth):
            return (str(pid) + ' (fixture) ' + ' '.join(['S', str(parent)] + ['0'] * 17 + [str(birth), '0']) + '\n').encode()

        for mode in ('valid', 'changed-after', 'expected-mismatch', 'missing'):
            paths = []; reads = []; stats = 0

            def opened(path, flags):
                nonlocal stats
                paths.append(path)
                if mode == 'missing':
                    raise FileNotFoundError('memory-only missing')
                if path == '/proc/41/stat':
                    stats += 1
                    return MemoryFile(stat(41, 17, 200 if mode == 'changed-after' and stats > 1 else 100), reads)
                if path == '/proc/41/status':
                    return MemoryFile(b'SigBlk:\t0\nSigIgn:\t0\nSigCgt:\t4000\nSigPnd:\t4000\nShdPnd:\t0\nSigQ:\t1/1024\n', reads)
                raise AssertionError('Unexpected process read')

            namespace = load(open=opened)
            expected = {'pid': 41, 'ppid': 17, 'birth': 200} if mode == 'expected-mismatch' else None
            result = namespace['DirectSignalTrace'].process(41, expected)
            if mode == 'valid':
                self.assertEqual(result['masks'], {'SigBlk': '0', 'SigIgn': '0', 'SigCgt': '4000', 'SigPnd': '4000', 'ShdPnd': '0'})
                self.assertEqual(reads, [4097, 8193, 4097])
            else:
                self.assertIn('unknown', result)
            if mode == 'expected-mismatch':
                self.assertEqual(paths, ['/proc/41/stat'])

    def test_capture_keeps_agent_and_before_identity_without_adopting_an_unknown_target(self):
        namespace = load()
        trace = namespace['DirectSignalTrace'](Path('/fixture'), 'kimi')
        calls = []
        identities = {17: {'pid': 17, 'ppid': 3, 'birth': 90}, 41: {'pid': 41, 'ppid': 17, 'birth': 100}}

        def process(pid, expected=None):
            calls.append((pid, expected))
            return {'identity': identities[pid], 'masks': {'SigPnd': '0'}}

        trace.process = process
        record = {'pid': 41, 'run_id': 'run', 'conversation_id': 'conversation', 'pane': '%0',
                  'process_start': 'synthetic start', 'supervisor': {'pid': 17, 'start': 'synthetic parent'}}
        before = trace.capture('before_SIGTERM', record)
        self.assertEqual(before['agent'], 'kimi')
        self.assertEqual(calls, [(17, None), (41, None)])
        calls.clear()
        after = trace.capture('after_SIGTERM_returned', record, before)
        self.assertEqual(after['agent'], 'kimi')
        self.assertEqual(calls, [(17, identities[17]), (41, identities[41])])
        calls.clear()
        unknown = trace.capture('after_SIGTERM_returned', record, {'child': {'unknown': 'missing'}, 'supervisor': {'unknown': 'missing'}})
        self.assertEqual(calls, [])
        self.assertEqual(unknown['child'], {'unknown': 'no pre-signal identity'})
        self.assertEqual(unknown['supervisor'], {'unknown': 'no pre-signal identity'})

    def test_failure_reader_rejects_other_agent_rows_and_keeps_bounds_without_disk(self):
        row = {'agent': 'kimi', 'phase': 'handler_registered', 'pid': 41, 'run': 'run', 'conversation': 'conversation', 'at_ns': 12}
        raw = (json.dumps(row) + '\n' + json.dumps({**row, 'agent': 'codex'}) + '\n').encode()
        paths = []; reads = []; retained = []; syncs = []

        def opened(path, mode):
            paths.append(str(path))
            self.assertEqual(str(path), '/fixture/direct-signal-agent-kimi.jsonl')
            self.assertEqual(mode, 'rb')
            return MemoryFile(raw, reads)

        def temporary(**options):
            self.assertEqual(options['prefix'], 'direct-signal-')
            self.assertEqual(str(options['dir']), '/repo/artifacts/test-results/python')
            return MemoryArtifact(retained)

        namespace = load(open=opened, REPO=MemoryPath('/repo'), tempfile=SimpleNamespace(NamedTemporaryFile=temporary),
                         os=SimpleNamespace(open=lambda *args: 23, close=lambda fd: syncs.append(('close', fd)),
                                            fsync=lambda fd: syncs.append(('sync', fd)), O_RDONLY=0, O_DIRECTORY=1, O_CLOEXEC=2))
        trace = namespace['DirectSignalTrace'](Path('/fixture'), 'kimi')
        message = trace.failure()
        self.assertIn('file and parent synced', message)
        self.assertEqual(paths, ['/fixture/direct-signal-agent-kimi.jsonl'])
        self.assertEqual(reads, [8705])
        saved = json.loads(retained[0])
        self.assertEqual(saved['agent'], 'kimi')
        self.assertEqual(saved['agent_phases'], [row])
        self.assertEqual(saved['malformed_or_incomplete_rows'], 1)
        self.assertEqual(syncs, [('sync', 17), ('sync', 23), ('close', 23)])


if __name__ == '__main__':
    unittest.main()
