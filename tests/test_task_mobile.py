#!/usr/bin/env python3
"""`hgs task mobile-send`, `mobile-cancel` and `mobile-done`: the phone's Send
now, Cancel and Done list as the connector runs them, through the real CLI."""
import fcntl
import hashlib
import json
import os
import subprocess
import time
import unittest
import uuid

from tests import test_tasks

HGS = test_tasks.HGS


class Mobile(test_tasks.DeliveryFixture):
    def mobile(self, action, request, ok=True):
        result = subprocess.run([str(HGS), "task", action, "--json"], input=json.dumps(request),
                                env=self.env, text=True, capture_output=True, timeout=60)
        if ok:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return json.loads(result.stdout)
        self.assertEqual(result.returncode, 1, result.stdout)
        refusal = json.loads(result.stdout)
        self.assertEqual((refusal["schema"], refusal["ok"]), (1, False))
        return refusal

    def send_request(self, task, session=None):
        return {"schema": 1, "project": "project-1", "task": task["uuid"], "session": session or self.name}

    def plain_refusal(self, *args):
        """What `hgs task …` itself says, without its prefix."""
        result = subprocess.run([str(HGS), "task", *args], env=self.env, text=True, capture_output=True, timeout=60)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        return result.stderr.strip().removeprefix("hgs: ")

    def accept(self, task):
        self.hgs("send", task["ref"])
        self.wait_received(task["text"])
        self.confirm()
        self.as_agent("report", task["ref"], "--json", text="Done.")
        self.hgs("stage", task["ref"], "done")

    def test_a_phone_sends_a_task_to_a_session_here(self):
        task = self.add("Add rate limiting")
        answer = self.mobile("mobile-send", self.send_request(task))
        self.assertEqual((answer["ok"], answer["task"], answer["stage"]), (True, task["uuid"], "active"))
        self.assertEqual(answer["session"]["session"], self.name)
        self.assertLessEqual(set(answer), {"schema", "ok", "task", "stage", "session", "receipt"})
        self.wait_received("Add rate limiting")
        self.assertEqual(self.hgs("get", task["ref"])["task"]["stage"], "active")

    def test_a_send_refused_before_the_paste_says_why(self):
        task = self.add("Add rate limiting")
        missing = self.mobile("mobile-send", self.send_request(task, "codex/nowhere"), ok=False)
        self.assertEqual(missing["error"], self.plain_refusal("send", task["ref"], "--session", "codex/nowhere"))
        self.assertNotIn("uncertain", missing)
        self.mobile("mobile-send", self.send_request(task))
        self.wait_received("Add rate limiting")
        again = self.mobile("mobile-send", self.send_request(task), ok=False)
        self.assertIn("already In progress", again["error"])
        self.assertNotIn("uncertain", again)
        unknown = dict(self.send_request(task), task=str(uuid.uuid4()))
        self.assertIn("no task", self.mobile("mobile-send", unknown, ok=False)["error"])

    def test_a_send_that_could_not_record_is_uncertain(self):
        task = self.add("Add rate limiting")
        self.hgs("comment", task["ref"], text="A first comment writes its updates file.")
        tasks = self.state / "replica" / hashlib.sha256(b"project-1").hexdigest() / "tasks"
        updates = tasks / (task["uuid"] + ".updates")
        self.assertTrue(updates.exists())
        updates.chmod(0o400)
        try:
            answer = self.mobile("mobile-send", self.send_request(task), ok=False)
        finally:
            updates.chmod(0o600)
        self.assertTrue(answer.get("uncertain"), answer)
        self.assertTrue(answer["error"].startswith("the prompt was sent, but the task could not record it"), answer)
        self.wait_received("Add rate limiting")

    def test_a_phone_cancels_a_task(self):
        task = self.add("Add rate limiting")
        self.mobile("mobile-send", self.send_request(task))
        self.wait_received("Add rate limiting")
        self.confirm()
        answer = self.mobile("mobile-cancel", {"schema": 1, "project": "project-1", "task": task["uuid"],
                                               "reason": "Wrong approach"})
        self.assertEqual((answer["ok"], answer["stage"]), (True, "review"))
        self.assertLessEqual(set(answer), {"schema", "ok", "task", "stage", "cancel_message"})
        got = self.hgs("get", task["ref"])["task"]
        self.assertEqual(got["stage"], "review")
        self.assertIn("Wrong approach", [e.get("text") for e in got["log"]])
        refused = self.mobile("mobile-cancel", {"schema": 1, "project": "project-1", "task": task["uuid"]}, ok=False)
        self.assertIn("In progress", refused["error"])

    def test_done_pages(self):
        tasks = [self.add(f"Task {n}", "--target", self.name) for n in range(3)]
        for task in tasks:
            self.accept(task)
        first = self.mobile("mobile-done", {"schema": 1, "project": "project-1", "limit": 2})
        self.assertEqual((first["total"], first["recent"], len(first["tasks"])), (3, 3, 2))
        self.assertEqual([row["text"] for row in first["tasks"]], ["Task 2", "Task 1"])
        row = first["tasks"][0]
        self.assertEqual(set(row), {"uuid", "id", "text", "session", "accepted_at"})
        self.assertEqual(row["session"]["session"], self.name)
        rest = self.mobile("mobile-done", {"schema": 1, "project": "project-1", "limit": 2, "before": first["next"]})
        self.assertEqual(([r["text"] for r in rest["tasks"]], rest["next"], rest["damaged"]), (["Task 0"], None, 0))

    def test_done_for_one_session(self):
        task = self.add("Task 0", "--target", self.name)
        self.accept(task)
        mine = self.mobile("mobile-done", {"schema": 1, "project": "project-1", "limit": 50})["tasks"][0]["session"]
        only = self.mobile("mobile-done", {"schema": 1, "project": "project-1", "limit": 50, "session": mine})
        self.assertEqual((only["total"], only["recent"], [r["text"] for r in only["tasks"]]), (1, 1, ["Task 0"]))
        other = dict(mine, session="codex/elsewhere")
        none = self.mobile("mobile-done", {"schema": 1, "project": "project-1", "limit": 50, "session": other})
        self.assertEqual((none["total"], none["recent"], none["tasks"]), (0, 0, []))
        broken = {"schema": 1, "project": "project-1", "limit": 50, "session": {"session": self.name}}
        self.assertIn("invalid request", self.mobile("mobile-done", broken, ok=False)["error"])

    def test_mobile_answers_stay_small(self):
        # A long prompt and 4 MiB of comments: the answer carries neither.
        prompt = "Add rate limiting " + hashlib.sha256(b"prompt").hexdigest() * 900
        task = self.add(prompt)
        for n in range(64):
            self.hgs("comment", task["ref"], text=hashlib.sha256(str(n).encode()).hexdigest() * 1000)
        result = subprocess.run([str(HGS), "task", "mobile-send", "--json"], input=json.dumps(self.send_request(task)),
                                env=self.env, text=True, capture_output=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertLess(len(result.stdout), 4096)
        self.assertNotIn(prompt[20:80], result.stdout)
        self.assertNotIn("submitted_text", result.stdout)

    def test_a_busy_project_refuses_a_send_before_anything_happens(self):
        task = self.add("Add rate limiting")
        lock = self.state / "replica" / hashlib.sha256(b"project-1").hexdigest() / ".lock"
        with lock.open("a") as held:
            fcntl.flock(held, fcntl.LOCK_EX)
            started = time.monotonic()
            refused = self.mobile("mobile-send", self.send_request(task), ok=False)
            self.assertLess(time.monotonic() - started, 10)
        self.assertEqual(refused["error"], "the project's tasks are busy; try again")
        self.assertNotIn("uncertain", refused)
        self.assertNotIn("Add rate limiting", self.received())

    def test_phone_strings_that_look_like_options_are_refused(self):
        task = self.add("Add rate limiting")
        refused = self.mobile("mobile-send", self.send_request(task, "--host"), ok=False)
        self.assertIn("is not a session name", refused["error"])
        cancel = {"schema": 1, "project": "project-1", "task": task["uuid"], "reason": "--agent"}
        self.assertIn("cannot look like an option", self.mobile("mobile-cancel", cancel, ok=False)["error"])
        # A reason with words may start with a dash.
        self.mobile("mobile-send", self.send_request(task))
        self.wait_received("Add rate limiting")
        self.confirm()
        cancel["reason"] = "- not needed after all"
        self.assertEqual(self.mobile("mobile-cancel", cancel)["stage"], "review")

    def test_invalid_mobile_requests_are_refused_in_json(self):
        task = self.add("Add rate limiting")
        base = self.send_request(task)
        self.assertIn("unknown request schema 2", self.mobile("mobile-send", dict(base, schema=2), ok=False)["error"])
        self.assertIn("no project", self.mobile("mobile-send", dict(base, project="nowhere"), ok=False)["error"])
        self.assertIn("not a task UUID", self.mobile("mobile-send", dict(base, task="T-a1b2c3"), ok=False)["error"])
        self.assertIn("invalid request", self.mobile("mobile-cancel", {"schema": 1}, ok=False)["error"])
        page = {"schema": 1, "project": "project-1", "limit": 51}
        self.assertIn("1 to 50", self.mobile("mobile-done", page, ok=False)["error"])
        cursor = dict(page, limit=2, before={"at": 1, "uuid": "x"})
        self.assertIn("not a task UUID", self.mobile("mobile-done", cursor, ok=False)["error"])


if __name__ == "__main__":
    unittest.main()
