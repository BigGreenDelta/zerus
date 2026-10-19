#!/usr/bin/env python3
"""`hgs task exchange` and `hgs task digests`: what the relay hands a phone,
through the real CLI. A second replica with the same catalog stands in for the
phone: what this computer answers goes into the phone's own exchange, which
imports it, and what the phone answers is what it sends."""
import base64
import hashlib
import json
import os
import subprocess
import unittest

from tests import test_tasks

HGS = test_tasks.HGS
EMPTY = {"project": {}, "tasks": {}}
# Comments that make one task larger than a page, yet small enough that the
# phone stand-in imports it whole through its own exchange, which takes 1 MiB.
COMMENTS = 5


def noise(seed, length):
    """Text that does not compress."""
    out = ""
    while len(out) < length:
        out += hashlib.sha256(f"{seed}-{len(out)}".encode()).hexdigest()
    return out[:length]


class Exchange(test_tasks.DeliveryFixture):
    def setUp(self):
        super().setUp()
        # The phone: its own replica of the same projects.
        (self.root / "phone-state").mkdir()
        self.phone = dict(self.env, HGS_CONFIG_DIR=str(self.root / "phone-config"),
                          HGS_STATE_DIR=str(self.root / "phone-state"))
        catalog = self.run_hgs(["swarm", "get"], self.env)["organization"]
        initialized = subprocess.run([str(HGS), "swarm", "initialize"], input=json.dumps(catalog), env=self.phone,
                                     text=True, capture_output=True, timeout=30)
        self.assertEqual(initialized.returncode, 0, initialized.stderr)

    def run_hgs(self, args, env, data=None, ok=True):
        result = subprocess.run([str(HGS), *args], input=None if data is None else json.dumps(data), env=env,
                                text=True, capture_output=True, timeout=60)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(result.stdout)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        return result.stderr

    def exchange(self, request, env=None, ok=True):
        return self.run_hgs(["task", "exchange", "--json"], env or self.env, request, ok)

    def digests(self, env=None):
        return {k: v["digest"] for k, v in self.run_hgs(["task", "digests", "--json"], env or self.env)["projects"].items()}

    def inventory(self, env):
        swarm = self.run_hgs(["swarm", "export"], env)["catalog"]["swarm_id"]
        return self.run_hgs(["swarm", "replica-inventory"], env, {"swarm_id": swarm, "project": "project-1"})["inventory"]

    def give(self, updates):
        """The phone imports `updates`."""
        if updates:
            self.exchange({"schema": 1, "project": "project-1", "inventory": EMPTY, "updates": updates}, env=self.phone)

    def pull(self):
        """Everything this computer answers the phone, page by page and part by
        part; the number of parts that arrived."""
        resume, collected, parts = None, b"", 0
        for _ in range(60):
            request = {"schema": 1, "project": "project-1", "inventory": self.inventory(self.phone)}
            if resume:
                request["continue"] = resume
            answer = self.exchange(request)
            self.give(answer["updates"])
            part = answer.get("part")
            resume = None
            if part:
                parts += 1
                chunk = base64.b64decode(part["bytes"])
                collected = (b"" if part["offset"] == 0 else collected) + chunk
                if part["offset"] + len(chunk) < part["size"]:
                    resume = {"task": part["task"], "sha256": part["sha256"], "offset": part["offset"] + len(chunk)}
                else:
                    self.assertEqual(hashlib.sha256(collected).hexdigest(), part["sha256"])
                    self.give([{"task": part["task"], "bytes": base64.b64encode(collected).decode()}])
            if not answer["more"] and resume is None:
                return parts
        self.fail("the exchange never finished")

    def push(self):
        """What the phone holds and this computer lacks goes to this computer."""
        sent = self.exchange({"schema": 1, "project": "project-1", "inventory": self.inventory(self.env)}, env=self.phone)
        return self.exchange({"schema": 1, "project": "project-1", "inventory": self.inventory(self.phone),
                              "updates": sent["updates"]})

    def accept(self, task):
        self.hgs("send", task["ref"])
        self.wait_received(task["text"])
        self.confirm()
        self.as_agent("report", task["ref"], "--json", text="Done.")
        self.hgs("stage", task["ref"], "done")

    def phone_tasks(self, *args):
        return self.run_hgs(["task", "list", "--project", "orchard", *args], self.phone)["tasks"]

    def test_digests_name_every_project_and_change_with_a_task(self):
        before = self.digests()
        self.assertEqual(set(before), {"project-1", "project-2"})
        task = self.add("Add rate limiting", "--target", self.name)
        after = self.digests()
        self.assertNotEqual(after["project-1"], before["project-1"])
        self.assertEqual(after["project-2"], before["project-2"])
        self.accept(task)
        # The Done task leaves the phone's view: the digest is that of an empty project again.
        self.assertEqual(self.digests()["project-1"], before["project-1"])

    def test_an_empty_phone_gets_the_project_and_its_live_tasks(self):
        self.add("Add rate limiting")
        self.add("Measure first", "--backlog")
        done = self.add("Document the limits", "--target", self.name)
        self.accept(done)
        self.pull()
        self.assertEqual(sorted(t["text"] for t in self.phone_tasks()), ["Add rate limiting", "Measure first"])
        self.assertEqual(self.digests(self.phone)["project-1"], self.digests()["project-1"])

    def test_the_phone_s_updates_are_imported(self):
        added = subprocess.run([str(HGS), "task", "add", "--project", "orchard"], input="Filed on the phone",
                               env=self.phone, text=True, capture_output=True, timeout=30)
        self.assertEqual(added.returncode, 0, added.stderr)
        answer = self.push()
        self.assertGreaterEqual(answer["imported"], 1)
        here = self.hgs("list", "--project", "orchard")["tasks"]
        self.assertEqual([(t["text"], t["author"]["kind"]) for t in here], [("Filed on the phone", "person")])
        self.assertEqual(answer["digest"], self.digests()["project-1"])

    def test_a_task_the_phone_holds_follows_it_into_done(self):
        task = self.add("Add rate limiting", "--target", self.name)
        self.pull()
        self.assertEqual([t["stage"] for t in self.phone_tasks()], ["todo"])
        self.accept(task)
        self.pull()
        self.assertEqual([t["stage"] for t in self.phone_tasks()], ["done"])
        self.assertEqual(self.digests(self.phone)["project-1"], self.digests()["project-1"])

    def test_a_large_document_travels_in_parts(self):
        task = self.add("Add rate limiting")
        for n in range(COMMENTS):
            self.hgs("comment", task["ref"], text=noise(f"comment-{n}", 60000))
        parts = self.pull()
        self.assertGreater(parts, 1)
        got = self.run_hgs(["task", "get", task["id"], "--project", "orchard"], self.phone)["task"]
        self.assertEqual(sum(1 for e in got["log"] if e["kind"] == "comment"), COMMENTS)

    def test_a_part_of_a_changed_update_starts_again(self):
        task = self.add("Add rate limiting")
        for n in range(COMMENTS):
            self.hgs("comment", task["ref"], text=noise(f"comment-{n}", 60000))
        first = self.exchange({"schema": 1, "project": "project-1", "inventory": EMPTY})["part"]
        self.assertEqual(first["offset"], 0)
        self.hgs("comment", task["ref"], text="One more.")
        resume = {"task": first["task"], "sha256": first["sha256"], "offset": len(base64.b64decode(first["bytes"]))}
        again = self.exchange({"schema": 1, "project": "project-1", "inventory": EMPTY, "continue": resume})["part"]
        self.assertEqual(again["offset"], 0)
        self.assertNotEqual(again["sha256"], first["sha256"])

    def test_damaged_updates_are_refused_and_named(self):
        added = subprocess.run([str(HGS), "task", "add", "--project", "orchard"], input="Filed on the phone",
                               env=self.phone, text=True, capture_output=True, timeout=30)
        self.assertEqual(added.returncode, 0, added.stderr)
        sent = self.exchange({"schema": 1, "project": "project-1", "inventory": self.inventory(self.env)}, env=self.phone)
        damaged = {"task": "00000000-0000-4000-8000-000000000001", "bytes": base64.b64encode(os.urandom(64)).decode()}
        answer = self.exchange({"schema": 1, "project": "project-1", "inventory": EMPTY,
                                "updates": sent["updates"] + [damaged]})
        self.assertEqual([r["uuid"] for r in answer["refused"]], [damaged["task"]])
        self.assertGreaterEqual(answer["imported"], 1)
        self.assertEqual([t["text"] for t in self.hgs("list", "--project", "orchard")["tasks"]], ["Filed on the phone"])

    def test_an_unknown_project_is_refused(self):
        stored = sorted((self.state / "replica").iterdir()) if (self.state / "replica").exists() else []
        error = self.exchange({"schema": 1, "project": "nowhere", "inventory": EMPTY}, ok=False)
        self.assertIn('no project "nowhere" here', error)
        after = sorted((self.state / "replica").iterdir()) if (self.state / "replica").exists() else []
        self.assertEqual(after, stored)

    def test_invalid_requests_are_refused(self):
        base = {"schema": 1, "project": "project-1", "inventory": EMPTY}
        self.assertIn("unknown exchange schema 2", self.exchange(dict(base, schema=2), ok=False))
        self.assertIn("invalid exchange request", self.exchange({"schema": 1, "project": "project-1"}, ok=False))
        self.assertIn("invalid base64", self.exchange(dict(base, updates=[{"task": None, "bytes": "%%%"}]), ok=False))
        self.assertIn("exceeds 1 MiB", self.exchange(dict(base, padding="x" * (1 << 20)), ok=False))
        self.assertIn("usage", self.run_hgs(["task", "exchange"], self.env, base, ok=False))


if __name__ == "__main__":
    unittest.main()
