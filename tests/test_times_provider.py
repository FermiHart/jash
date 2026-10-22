#!/usr/bin/env python3
"""Actual text/JSON/JTV2 consumers refuse failed CPU observations.

The libc hooks are confined to an isolated host subprocess. They provide exact
units and failures, not evidence of a guest accounting provider.
"""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

from test_discovery_builtins import native_stream, project

ROOT = Path(__file__).resolve().parents[1]
JASH = Path(os.environ.get("JASH", ROOT / "build/jash")).resolve()
EXPECTED = dict(kind="cpu_times", ticks_per_second=250,
                shell_user_ticks=18750, shell_system_ticks=563,
                children_user_ticks=30000, children_system_ticks=31)


class TimesProvider(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temporary = tempfile.TemporaryDirectory(prefix="jash-times-hook-")
        cls.addClassCleanup(temporary.cleanup)
        cls.hook = Path(temporary.name) / "times.so"
        compiler = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run([*compiler, "-std=gnu11", "-shared", "-fPIC",
                        "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                        str(ROOT / "tests/times_provider_faults.c"), "-ldl",
                        "-o", str(cls.hook)], check=True, capture_output=True,
                       timeout=30)

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="jash-times-consumer-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.env = dict(PATH=os.defpath, HOME=str(self.directory), LC_ALL="C",
                        LD_PRELOAD=str(self.hook), JASH_SIGIL="0", JASH_NUDGE="0",
                        JASH_ADOPT="0", JASH_MEMO="0", JASH_BRIEF="0",
                        JASH_TIMELINE="0", JASH_LOOPGUARD="off")

    def run_times(self, selector="", mode="valid", status=0, suffix=""):
        env = dict(self.env, JASH_TEST_TIMES_MODE=mode)
        result = subprocess.run([str(JASH), "-c", "times " + selector + suffix],
                                cwd=self.directory, env=env, capture_output=True,
                                timeout=20)
        self.assertEqual(result.returncode, status,
                         (mode, selector, result.stdout, result.stderr))
        return result

    def test_text_uses_the_reported_tick_rate(self):
        result = self.run_times()
        self.assertEqual(result.stdout,
                         b"1m15.000s 0m2.252s\n2m0.000s 0m0.124s\n")

    def test_json_and_data_preserve_the_same_observation(self):
        report = json.loads(self.run_times("--json").stdout)
        self.assertEqual(report["items"], [EXPECTED])
        self.assertTrue(report["complete"])
        self.assertEqual(report["status"], 0)
        rows, terminal = native_stream(self.run_times("--data").stdout)
        self.assertEqual([project(row) for row in rows], [EXPECTED])
        self.assertEqual((terminal["state"], terminal["status"]), (0, 0))

    def test_text_refuses_unavailable_or_failed_counters(self):
        for mode in ("fail", "unsupported"):
            with self.subTest(mode=mode):
                self.assertEqual(self.run_times(mode=mode, status=1).stdout,
                                 b"times: CPU time unavailable\n")

    def test_text_never_guesses_a_tick_rate(self):
        for mode in ("rate-zero", "rate-negative"):
            with self.subTest(mode=mode):
                self.assertEqual(self.run_times(mode=mode, status=1).stdout,
                                 b"times: clock tick rate unavailable\n")

    def test_json_failure_has_no_cpu_measurement(self):
        for mode in ("fail", "unsupported", "rate-zero", "rate-negative"):
            with self.subTest(mode=mode):
                report = json.loads(self.run_times("--json", mode, 1).stdout)
                self.assertEqual(report["schema"], "jash.builtin.error.v1")
                self.assertEqual(report["command"], "times")
                self.assertFalse(report["complete"])
                self.assertEqual(report["error"]["code"], "operation_failed")
                self.assertNotIn("items", report)

    def test_data_failure_has_no_cpu_measurement(self):
        for mode in ("fail", "unsupported", "rate-zero", "rate-negative"):
            with self.subTest(mode=mode):
                rows, terminal = native_stream(self.run_times("--data", mode, 1).stdout)
                self.assertEqual(rows, [])
                self.assertEqual((terminal["state"], terminal["status"]), (2, 1))
                self.assertEqual(terminal["reason"], "operation_failed")

    def test_structured_failure_still_checks_delivery(self):
        for selector in ("--json", "--data"):
            with self.subTest(selector=selector):
                self.run_times(selector, "unsupported", 125, " > /dev/full")


if __name__ == "__main__":
    unittest.main()
