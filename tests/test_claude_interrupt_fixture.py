"""Pure controls for the native interrupt fixture; no agent, tmux or CLI runs."""
import os
from pathlib import Path
import select
import shlex
import sys
import tempfile
import unittest
from unittest.mock import patch

import test_claude_approval_native as approvals


class InterruptFixture(unittest.TestCase):
    def tool(self, work):
        fixture = approvals.NativeClaudeInterrupt('test_escape_interrupt_is_recorded_without_a_hook')
        # Keep the exact-parent control executable: it exposes the old command.
        return fixture.native_tool_input(work) if hasattr(fixture, 'native_tool_input') else fixture.tool_input

    def test_interrupt_work_does_not_depend_on_a_long_foreground_sleep(self):
        with tempfile.TemporaryDirectory() as root:
            tool = self.tool(Path(root))
            self.assertNotRegex(tool['command'], r'\bsleep\s+30\b',
                                'Interrupt fixture must reach real work without the rejected foreground sleep')
            arguments = shlex.split(tool['command'])
            self.assertEqual(arguments, [sys.executable, str(Path(root) / 'interrupt-work.py')])
            self.assertTrue(Path(arguments[1]).is_file())
            self.assertFalse((Path(root) / 'interrupt-started.txt').exists())
            self.assertFalse((Path(root) / 'slept.txt').exists())

    def execute(self, work, interrupted):
        self.tool(work)
        script = work / 'interrupt-work.py'
        def ready(readers, writers, errors, timeout):
            self.assertEqual((readers, writers, errors, timeout), ([17], [], [], 30))
            self.assertEqual((work / 'interrupt-started.txt').read_text(), 'started')
            self.assertFalse((work / 'slept.txt').exists())
            if interrupted:
                raise KeyboardInterrupt
            return [], [], []
        with patch.object(os, 'pipe', return_value=(17, 18)), \
                patch.object(os, 'close') as close, patch.object(select, 'select', side_effect=ready) as wait:
            if interrupted:
                with self.assertRaises(KeyboardInterrupt):
                    exec(compile(script.read_text(), str(script), 'exec'), {'__file__': str(script)})
            else:
                exec(compile(script.read_text(), str(script), 'exec'), {'__file__': str(script)})
            self.assertEqual(wait.call_count, 1)
            self.assertEqual([call.args for call in close.call_args_list], [(17,), (18,)])

    def test_interruption_after_work_admission_never_writes_completion(self):
        with tempfile.TemporaryDirectory() as root:
            work = Path(root)
            self.execute(work, interrupted=True)
            self.assertFalse((work / 'slept.txt').exists())

    def test_original_thirty_second_bound_can_complete_only_after_wait(self):
        with tempfile.TemporaryDirectory() as root:
            work = Path(root)
            self.execute(work, interrupted=False)
            self.assertEqual((work / 'slept.txt').read_text(), 'done')


if __name__ == '__main__':
    unittest.main()
