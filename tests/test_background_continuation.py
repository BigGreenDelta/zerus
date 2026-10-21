"""Claude's background jobs over a private tmux socket; never real user sessions."""
import hashlib, json, os, subprocess, time, unittest, uuid
import test_input

# Claude's background process runs the hook; it descends from no session's agent,
# and neither does this test process.
BACKGROUND = os.getpid()


class BackgroundContinuation(unittest.TestCase):
    """Claude runs background jobs in processes that inherited the run token of
    whichever session started Claude's background service; a job's hooks belong
    to the session whose agent window moved it to the background."""
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
        self.job = str(uuid.uuid4())

    def registry(self, pid, **entry):
        """Claude's session registry entry for a live Claude process."""
        path = self.root / ".claude/sessions" / f"{pid}.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(dict(entry, pid=pid)))

    def background(self, conversation, window=None):
        """The job runs `conversation`; `window` is the agent that moved it there."""
        self.registry(BACKGROUND, kind="bg", sessionId=conversation, jobId=self.job)
        if window:
            self.registry(window["pid"], kind="interactive", sessionId=window["conversation_id"], parkedJobId=self.job)

    def transcript(self, conversation, continued_in=None, closed=False):
        path = self.root / ".claude/projects/-fixture" / (conversation + ".jsonl")
        path.parent.mkdir(parents=True, exist_ok=True)
        lines = [dict(type="user", uuid=str(uuid.uuid4()), sessionId=conversation,
                      message=dict(role="user", content="Previous request"))]
        if continued_in:
            lines.append(dict(type="continued-in", sessionId=conversation, continuedInSessionId=continued_in))
        if closed:
            # Claude's own bookkeeping after the mark, as when the foreground process exits.
            lines += [dict(type="last-prompt", sessionId=conversation), dict(type="cost-state", sessionId=conversation)]
        path.write_text("".join(json.dumps(line) + "\n" for line in lines))
        return str(path)

    def hook(self, claude=None, **event):
        """`claude` is the Claude process that runs the hook; Claude names it
        and the hook's conversation in the hook's environment."""
        env = dict(self.env, HGS_SESSION=self.name, HGS_RUN_ID=self.run_id, HGS_AGENT="claude")
        for variable in ("CLAUDE_PID", "CLAUDE_CODE_SESSION_ID", "CLAUDE_CONFIG_DIR"):
            env.pop(variable, None)
        if claude:
            env.update(CLAUDE_PID=str(claude), CLAUDE_CODE_SESSION_ID=event["session_id"])
        result = subprocess.run([str(test_input.HGS), "__state", "hook"], env=env, input=json.dumps(dict(event, cwd="/fixture")),
                                text=True, capture_output=True, timeout=12)
        self.assertEqual(result.returncode, 0, result.stderr)

    def saved(self, path):
        return json.loads(path.read_text())

    def cleared(self, name):
        result = subprocess.run([str(test_input.HGS), "inspect", name], env=self.env, text=True, capture_output=True, timeout=12)
        self.assertEqual(result.returncode, 0, result.stderr)
        return sum(event["type"] == "SessionCleared" for event in json.loads(result.stdout)["events"])

    def tool(self, conversation, path, event="PreToolUse"):
        self.hook(BACKGROUND, hook_event_name=event, session_id=conversation, transcript_path=path, tool_name="Bash",
                  tool_use_id="toolu_fixture", tool_input=dict(command="true"))

    def test_a_background_conversation_reports_to_the_session_that_moved_it(self):
        new = str(uuid.uuid4())
        path = self.transcript(new)
        self.transcript(self.other["conversation_id"], continued_in=new)
        self.background(new, window=self.other)
        self.tool(new, path)
        other = self.saved(self.other_path)
        self.assertEqual((other["conversation_id"], other["transcript"]), (new, path))
        self.assertEqual((other["activity"], other["phase"]), ("busy", "tool"))
        # The same conversation goes on: nothing of it is cleared.
        self.assertEqual((other["prompt"], other["last_message"]), ("Previous request", "Previous answer"))
        self.tool(new, path, "PostToolUse")
        self.hook(BACKGROUND, hook_event_name="Stop", session_id=new, transcript_path=path)
        other = self.saved(self.other_path)
        self.assertEqual((other["activity"], other["phase"]), ("idle", "idle"))
        self.assertEqual(self.cleared(self.other["name"]), 0)
        # The session whose token the background process inherited keeps its own conversation.
        own = self.saved(self.record_path)
        self.assertEqual((own["conversation_id"], own["transcript"], own["phase"]),
                         (self.conversation_id, self.record["transcript"], "idle"))
        self.assertEqual(self.cleared(self.name), 0)

    def test_claude_bookkeeping_after_the_mark_keeps_the_continuation(self):
        new = str(uuid.uuid4())
        path = self.transcript(new)
        self.transcript(self.other["conversation_id"], continued_in=new, closed=True)
        self.background(new, window=self.other)
        self.tool(new, path)
        other = self.saved(self.other_path)
        self.assertEqual((other["conversation_id"], other["prompt"]), (new, "Previous request"))

    def test_a_new_conversation_in_a_background_job_is_a_clear_of_the_session_that_moved_it(self):
        new = str(uuid.uuid4())
        path = self.transcript(new)
        self.background(new, window=self.other)
        self.hook(BACKGROUND, hook_event_name="SessionStart", session_id=new, source="clear", transcript_path=path)
        other = self.saved(self.other_path)
        self.assertEqual((other["conversation_id"], other["transcript"]), (new, path))
        self.assertNotIn("prompt", other)
        self.tool(new, path)
        other = self.saved(self.other_path)
        self.assertEqual((other["activity"], other["phase"]), ("busy", "tool"))
        self.assertEqual(self.saved(self.record_path)["conversation_id"], self.conversation_id)

    def test_a_session_sent_to_the_background_keeps_its_conversation(self):
        new = str(uuid.uuid4())
        path = self.transcript(new)
        self.record.update(prompt="Previous request", last_message="Previous answer",
                           transcript=self.transcript(self.conversation_id, continued_in=new))
        self.write_record()
        self.background(new, window=self.record)
        self.hook(BACKGROUND, hook_event_name="SessionStart", session_id=new, source="resume", transcript_path=path)
        own = self.saved(self.record_path)
        self.assertEqual((own["conversation_id"], own["transcript"]), (new, path))
        self.assertEqual((own["prompt"], own["last_message"]), ("Previous request", "Previous answer"))
        self.assertEqual(self.cleared(self.name), 0)

    def test_a_job_taken_by_the_token_session_goes_to_the_session_that_moved_it(self):
        # Before hgs knew background jobs, the token's session took this conversation.
        new = str(uuid.uuid4())
        path = self.transcript(new)
        self.record.update(conversation_id=new, transcript=path)
        self.write_record()
        self.transcript(self.other["conversation_id"], continued_in=new)
        self.background(new, window=self.other)
        self.tool(new, path)
        other = self.saved(self.other_path)
        self.assertEqual((other["conversation_id"], other["activity"], other["phase"]), (new, "busy", "tool"))
        self.assertEqual(self.saved(self.record_path)["phase"], "idle")

    def test_a_background_process_no_live_session_holds_leaves_every_session_alone(self):
        self.record.update(prompt="Previous request")
        self.write_record()
        gone = dict(pid=2 ** 22 + 7, conversation_id=str(uuid.uuid4()))
        # A spare process not yet given a job, a job no window holds, and one whose window is no session.
        for window, registered in ((None, False), (None, True), (gone, True)):
            new = str(uuid.uuid4())
            path = self.transcript(new)
            self.job = str(uuid.uuid4())
            registry = self.root / ".claude/sessions" / f"{BACKGROUND}.json"
            registry.unlink(missing_ok=True)
            if registered:
                self.background(new, window=window)
            self.hook(BACKGROUND, hook_event_name="SessionStart", session_id=new, source="startup", transcript_path=path)
            self.tool(new, path)
            own = self.saved(self.record_path)
            self.assertEqual((own["conversation_id"], own["prompt"], own["phase"]),
                             (self.conversation_id, "Previous request", "idle"))
            other = self.saved(self.other_path)
            self.assertEqual((other["conversation_id"], other["phase"]), (self.other["conversation_id"], "input"))
        self.assertEqual(self.cleared(self.name), 0)

    def test_a_conversation_that_went_on_after_the_mark_is_not_continued_in_the_job(self):
        new = str(uuid.uuid4())
        path = self.transcript(new)
        mark = self.transcript(self.other["conversation_id"], continued_in=new)
        with open(mark, "a") as transcript:
            transcript.write(json.dumps(dict(type="user", uuid=str(uuid.uuid4()), sessionId=self.other["conversation_id"])) + "\n")
        self.background(new, window=self.other)
        self.tool(new, path)
        self.assertEqual(self.saved(self.other_path)["conversation_id"], self.other["conversation_id"])
        self.assertEqual(self.saved(self.record_path)["conversation_id"], self.conversation_id)

    def test_a_new_conversation_of_the_session_agent_is_still_a_clear(self):
        self.record.update(prompt="Previous request")
        self.write_record()
        for claude in (self.record["pid"], None):
            new = str(uuid.uuid4())
            self.hook(claude, hook_event_name="SessionStart", session_id=new, source="clear", transcript_path=self.transcript(new))
            own = self.saved(self.record_path)
            self.assertEqual(own["conversation_id"], new)
            self.assertNotIn("prompt", own)
        self.assertEqual(self.saved(self.other_path)["conversation_id"], self.other["conversation_id"])

if __name__ == "__main__":
    unittest.main()
