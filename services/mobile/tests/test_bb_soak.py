"""Black-box soak (C7 parity gate): a connector heartbeating every second while
a phone polls one Terminal screen four times a second keeps its resident memory
and open descriptors flat.

It runs only with ZERUS_CONNECTOR_SOAK_SECONDS set (the connector CI job sets
300 for the Rust connector) and only on Linux, where /proc gives both figures.
After a warm-up, the last third of the samples may exceed the first third by a
small allocator margin at most: a descriptor, or a few kilobytes, kept per
request does not fit in it. Five minutes of the Rust build held 14,528 KiB and
13-14 descriptors from first to last sample on a Linux workstation (2026-10-16).
"""
import asyncio
import os
import sys
import time
import unittest

from bb_operations import OperationCase, help_text, listing
from connector_harness import BlackBoxCase, default_rules
from test_bb_terminal import terminal

SECONDS = float(os.environ.get("ZERUS_CONNECTOR_SOAK_SECONDS") or 0)
FRAME = 0.25  # four screens a second
SAMPLE = 2.0
# Slack for allocator arenas and a native call caught mid-flight.
MEMORY_MARGIN = 1.05
MEMORY_SLACK = 1024 * 1024
DESCRIPTOR_SLACK = 4


def usage(pid: int) -> tuple[int, int]:
    """(resident bytes, open descriptors) of one process."""
    with open(f"/proc/{pid}/status") as status:
        rss = next(int(line.split()[1]) * 1024 for line in status if line.startswith("VmRSS:"))
    return rss, len(os.listdir(f"/proc/{pid}/fd"))


class SoakTests(OperationCase, BlackBoxCase):
    async def asyncSetUp(self):
        if not SECONDS:
            self.skipTest("set ZERUS_CONNECTOR_SOAK_SECONDS to soak the connector")
        if not sys.platform.startswith("linux"):
            self.skipTest("the soak reads /proc")
        await super().asyncSetUp()
        self.hgs.script(terminal() + default_rules(listing(), help_text("terminal")))

    async def test_memory_and_descriptors_stay_flat_under_terminal_polling(self):
        process = await self.launch()
        warmup = min(60.0, SECONDS / 5)
        start = time.monotonic()
        samples, screens, failures = [], 0, []
        next_sample = start + warmup
        while (now := time.monotonic()) < start + SECONDS:
            result = await self.submit(self.operation("terminal_snapshot"), process)
            if result["state"] == "completed":
                screens += 1
            else:
                failures.append(result)
            if now >= next_sample:
                samples.append(usage(process.pid))
                next_sample += SAMPLE
            await asyncio.sleep(max(0.0, FRAME - (time.monotonic() - now)))
        self.assertIsNone(process.returncode, process.output()[-2000:])
        self.assertEqual(failures[:3], [])
        # At least half the frames: a stalled connector soaks nothing.
        self.assertGreaterEqual(screens, SECONDS / FRAME / 2)
        third = len(samples) // 3
        self.assertGreaterEqual(third, 3, samples)
        early, late = samples[:third], samples[-third:]
        memory = max(rss for rss, _ in early), max(rss for rss, _ in late)
        descriptors = max(fds for _, fds in early), max(fds for _, fds in late)
        print(f"\nsoak: {screens} screens in {SECONDS:.0f} s; resident {memory[0] // 1024} -> {memory[1] // 1024} KiB, "
              f"descriptors {descriptors[0]} -> {descriptors[1]}", file=sys.stderr)
        self.assertLessEqual(memory[1], memory[0] * MEMORY_MARGIN + MEMORY_SLACK, samples)
        self.assertLessEqual(descriptors[1], descriptors[0] + DESCRIPTOR_SLACK, samples)


if __name__ == "__main__":
    unittest.main()
