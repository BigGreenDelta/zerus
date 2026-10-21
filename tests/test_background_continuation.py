"""Claude's background conversations over a private tmux socket; never real user sessions."""
import hashlib, json, subprocess, time, unittest, uuid
import test_input


class BackgroundContinuation(unittest.TestCase):
    """Claude runs a conversation sent to the background in another process,
    which inherited the run token of whichever session started Claude's
    background service; its hooks belong to the session it continues."""
    write_record = test_input.InputTransport.write_record
    tmux = test_input.InputTransport.tmux
    wait_for = test_input.InputTransport.wait_for
    script = test_input.InputTransport.script
    tearDown = test_input.InputTransport.tearDown

    def setUp(self):
        test_input.InputTransport.setUp(self)
        self.record.update(agent="claude", transcript=self.transcript(self.conversation_id))
        self.write_record()
        # The session sent to the background: its pane now shows the background job.
        name = "claude/meadow"
        target = "=" + name + ":"
        self.tmux("new-session", "-d", "-s", name, "-x", "100", "-y", "30", "sleep", "600")
        pid = int(self.tmux("display-message", "-p", "-t", target, "#{pane_pid}"))
        start = subprocess.check_output(["ps", "-p", str(pid), "-o", "lstart="],
                                        env=dict(self.env, LC_ALL="C", TZ="UTC"), text=True).strip()
        self.other = dict(version=1, name=name, agent="claude", run_id=str(uuid.uuid4()),
                          conversation_id=str(uuid.uuid4()), pid=pid, process_start=start,
                          pane=self.tmux("display-message", "-p", "-t", target, "#{pane_id}").strip(),
                          activity="busy", phase="input", main_done=False, active_tools={}, subagents={},
                          prompt="Previous request", last_message="Previous answer",
                          last_event_at=time.time(), created=time.time())
        self.other["transcript"] = self.transcript(self.other["conversation_id"])
        self.tmux("set-option", "-t", target, "@hgs_run", self.other["run_id"])
        self.other_path = self.state / (hashlib.sha256(name.encode()).hexdigest() + ".json")
        self.other_path.write_text(json.dumps(self.other))

    def transcript(self, conversation, continued_in=None):
        path = self.root / ".claude/projects/-fixture" / (conversation + ".jsonl")
        path.parent.mkdir(parents=True, exist_ok=True)
        lines = [dict(type="user", sessionId=conversation, message=dict(role="user", content="Previous request"))]
        if continued_in:
            lines.append(dict(type="continued-in", sessionId=conversation, continuedInSessionId=continued_in))
        path.write_text("".join(json.dumps(line) + "\n" for line in lines))
        return str(path)

    def hook(self, **event):
        env = dict(self.env, HGS_SESSION=self.name, HGS_RUN_ID=self.run_id, HGS_AGENT="claude")
        result = subprocess.run([str(test_input.HGS), "__state", "hook"], env=env, input=json.dumps(dict(event, cwd="/fixture")),
                                text=True, capture_output=True, timeout=12)
        self.assertEqual(result.returncode, 0, result.stderr)

    def saved(self, path):
        return json.loads(path.read_text())

    def cleared(self, name):
        result = subprocess.run([str(test_input.HGS), "inspect", name], env=self.env, text=True, capture_output=True, timeout=12)
        self.assertEqual(result.returncode, 0, result.stderr)
        return sum(event["type"] == "SessionCleared" for event in json.loads(result.stdout)["events"])

    def test_a_background_conversation_reports_to_the_session_it_continues(self):
        new = str(uuid.uuid4())
        path = self.transcript(new)
        self.transcript(self.other["conversation_id"], continued_in=new)
        self.hook(hook_event_name="SessionStart", session_id=new, source="resume", transcript_path=path)
        other = self.saved(self.other_path)
        self.assertEqual((other["conversation_id"], other["transcript"]), (new, path))
        # The same conversation goes on: nothing of it is cleared.
        self.assertEqual((other["prompt"], other["last_message"]), ("Previous request", "Previous answer"))
        self.assertEqual(self.cleared(self.other["name"]), 0)
        tool = dict(session_id=new, transcript_path=path, tool_name="Bash", tool_use_id="toolu_fixture",
                    tool_input=dict(command="true"))
        self.hook(hook_event_name="PreToolUse", **tool)
        other = self.saved(self.other_path)
        self.assertEqual((other["activity"], other["phase"]), ("busy", "tool"))
        self.hook(hook_event_name="PostToolUse", **tool)
        self.hook(hook_event_name="Stop", session_id=new, transcript_path=path)
        other = self.saved(self.other_path)
        self.assertEqual((other["activity"], other["phase"]), ("idle", "idle"))
        # The session whose token the background process inherited keeps its own conversation.
        own = self.saved(self.record_path)
        self.assertEqual((own["conversation_id"], own["transcript"], own["phase"]),
                         (self.conversation_id, self.record["transcript"], "idle"))
        self.assertEqual(self.cleared(self.name), 0)

    def test_a_session_sent_to_the_background_keeps_its_conversation(self):
        new = str(uuid.uuid4())
        path = self.transcript(new)
        self.record.update(prompt="Previous request", last_message="Previous answer",
                           transcript=self.transcript(self.conversation_id, continued_in=new))
        self.write_record()
        self.hook(hook_event_name="SessionStart", session_id=new, source="resume", transcript_path=path)
        own = self.saved(self.record_path)
        self.assertEqual((own["conversation_id"], own["transcript"]), (new, path))
        self.assertEqual((own["prompt"], own["last_message"]), ("Previous request", "Previous answer"))
        self.assertEqual(self.cleared(self.name), 0)

    def test_a_continuation_taken_by_the_token_session_goes_to_the_session_it_continues(self):
        # Before hgs knew the mark, the token's session took the background conversation.
        new = str(uuid.uuid4())
        path = self.transcript(new)
        self.record.update(conversation_id=new, transcript=path)
        self.write_record()
        self.transcript(self.other["conversation_id"], continued_in=new)
        self.hook(hook_event_name="SessionStart", session_id=new, source="compact", transcript_path=path)
        self.hook(hook_event_name="PreToolUse", session_id=new, transcript_path=path, tool_name="Bash",
                  tool_use_id="toolu_fixture", tool_input=dict(command="true"))
        other = self.saved(self.other_path)
        self.assertEqual((other["conversation_id"], other["activity"], other["phase"]), (new, "busy", "tool"))
        self.assertEqual(self.saved(self.record_path)["phase"], "idle")

    def test_a_new_conversation_of_the_session_is_still_a_clear(self):
        new = str(uuid.uuid4())
        self.record.update(prompt="Previous request")
        self.write_record()
        self.hook(hook_event_name="SessionStart", session_id=new, source="clear", transcript_path=self.transcript(new))
        own = self.saved(self.record_path)
        self.assertEqual(own["conversation_id"], new)
        self.assertNotIn("prompt", own)
        self.assertEqual(self.saved(self.other_path)["conversation_id"], self.other["conversation_id"])


if __name__ == "__main__":
    unittest.main()
