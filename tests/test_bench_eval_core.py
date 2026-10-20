#!/usr/bin/env python3
"""Falsify the evaluator benchmark's result checks; no timed benchmark or build."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import struct
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "tools/bench_eval_core.py"
spec = importlib.util.spec_from_file_location("bench_eval_core", RUNNER)
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)
decoder_spec = importlib.util.spec_from_file_location("eval_gmon", ROOT / "tools/eval_gmon.py")
decoder = importlib.util.module_from_spec(decoder_spec)
decoder_spec.loader.exec_module(decoder)


@unittest.skipUnless(Path("/usr/bin/bash").is_file(), "benchmark requires /usr/bin/bash")
class BenchmarkEvidence(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="eval-benchmark-test-")
        self.addCleanup(temp.cleanup)
        self.work = Path(temp.name)

    def test_workload_oracles_agree_with_bash(self):
        for name in bench.CASES:
            with self.subTest(name=name):
                bench.run_once("/usr/bin/bash", bench.workload(name, 5), self.work, bash=True)

    def test_zero_exit_with_missing_work_is_rejected(self):
        with self.assertRaises(RuntimeError):
            bench.run_once("/usr/bin/true", bench.workload("literal", 5), self.work)

    def test_nonzero_exit_is_rejected(self):
        with self.assertRaises(RuntimeError):
            bench.run_once("/usr/bin/false", bench.workload("literal", 5), self.work)

    def test_correct_stdout_does_not_hide_stderr_or_disk_mutation(self):
        for extra in ("printf diagnostic >&2", ": >unexpected"):
            with self.subTest(extra=extra), self.assertRaises(RuntimeError):
                case = {"name": "bad", "code": "printf '5\\n'; " + extra, "stdout": "5\n"}
                bench.run_once("/usr/bin/bash", case, self.work, bash=True)

    def test_paired_estimator_retains_shared_block_drift(self):
        rows = [{"baseline": {"wall_ms": drift}, "candidate": {"wall_ms": drift * .8}}
                for drift in (10, 20, 50, 80) for _ in range(6)]
        ratio = bench.paired_ratio(rows, "candidate", "baseline", 42)
        self.assertAlmostEqual(ratio["median_paired_ratio"], .8)
        for bound in ratio["ci95"]:
            self.assertAlmostEqual(bound, .8)
        with self.assertRaises(ValueError):
            bench.paired_ratio(rows[:-1], "candidate", "baseline", 42)

    def test_existing_evidence_directory_is_never_overwritten(self):
        marker = self.work / "marker"
        marker.write_text("KEEP")
        result = subprocess.run([os.sys.executable, "-B", str(RUNNER), "--baseline", "/usr/bin/bash",
                                 "--output", str(self.work)], capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(marker.read_text(), "KEEP")

    def test_modified_plan_fails_with_evidence(self):
        plan = bench.workload("literal", 5)
        plan["code"] = "true"
        path = self.work / "plan.json"
        path.write_text(json.dumps([plan]))
        output = self.work / "evidence"
        result = subprocess.run([os.sys.executable, "-B", str(RUNNER), "--baseline", "/usr/bin/bash",
                                 "--output", str(output), "--plan", str(path), "--cases", "literal"],
                                capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        failure = json.loads((output / "failure.json").read_text())
        self.assertIn("workload contract", failure["error"])
        self.assertFalse((output / "report.json").exists())

    def test_existing_dangling_output_symlink_is_refused(self):
        target, output = self.work / "missing", self.work / "link"
        output.symlink_to(target)
        plan = self.work / "empty-plan.json"
        plan.write_text("[]")
        result = subprocess.run([os.sys.executable, "-B", str(RUNNER), "--baseline", "/usr/bin/bash",
                                 "--output", str(output), "--plan", str(plan), "--cases", "literal"],
                                capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(output.is_symlink())
        self.assertFalse(target.exists(), "must reject the artifact path before following its symlink")

    def test_profile_clone_is_not_assigned_to_previous_function(self):
        header = b"gmon" + struct.pack("<I", 1) + bytes(12)
        arcs = b"".join(b"\x01" + struct.pack("<QQI", 0x50, pc, count)
                        for pc, count in ((0x102, 1), (0x122, 1000), (0x140, 7)))
        result = decoder.decode(header + arcs, [(0x100, 0x10, "wrapper"),
                                                (0x120, 0x20, "wrapper.part.0")])
        self.assertEqual(result["calls"], {"wrapper": 1, "wrapper.part.0": 1000, "<unmapped>": 7})

    def test_profile_truncation_is_rejected(self):
        data = b"gmon" + struct.pack("<I", 1) + bytes(12) + b"\x01\x00"
        with self.assertRaises(struct.error):
            decoder.decode(data, [(0x100, 0x10, "function")])


if __name__ == "__main__":
    unittest.main()
