#!/usr/bin/env python3
"""Zerus's SessionStart hook gives Claude Code and Codex sessions the project
task instructions: fake agents through the real lifecycle."""
import json
import unittest

import test_fork


class TaskContext(test_fork.Base):
    def hook_output(self, name, agent, kind="SessionStart", **fields):
        record = self.binding(name)
        event = dict(session_id=record["conversation_id"], hook_event_name=kind, **fields)
        env = dict(self.env, HGS_SESSION=name, HGS_RUN_ID=record["run_id"], HGS_AGENT=agent)
        return self.hook(event, env).stdout

    def test_claude_and_codex_sessions_read_the_task_commands_at_start(self):
        for agent in ("claude", "codex"):
            with self.subTest(agent=agent):
                name = self.start(agent, "context")
                output = json.loads(self.hook_output(name, agent, source="startup"))["hookSpecificOutput"]
                self.assertEqual(output["hookEventName"], "SessionStart")
                self.assertIn("hgs task add", output["additionalContext"])
                self.assertIn("поставь таск", output["additionalContext"])
                self.assertIn("hgs guide tasks", output["additionalContext"])
                # Tracking goes on: the session is still idle in its conversation.
                self.assertEqual(self.binding(name)["activity"], "idle")

    def test_a_start_hgs_did_not_record_reads_nothing(self):
        name = self.start("claude", "unknown")
        record = self.binding(name)
        event = dict(session_id=record["conversation_id"], hook_event_name="SessionStart", source="startup")
        env = dict(self.env, HGS_SESSION=name, HGS_RUN_ID="run-that-is-not-live", HGS_AGENT="claude")
        self.assertEqual(self.hook(event, env).stdout, "")

    def test_subagents_other_agents_and_other_events_read_nothing(self):
        name = self.start("claude", "quiet")
        self.assertEqual(self.hook_output(name, "claude", agent_id="agent-1"), "")
        self.assertEqual(self.hook_output(name, "claude", kind="UserPromptSubmit", prompt="hello"), "")
        kimi = self.start("kimi", "plain")
        self.assertEqual(self.hook_output(kimi, "kimi"), "")


if __name__ == "__main__":
    unittest.main()
