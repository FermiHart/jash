#!/usr/bin/env python3
"""DF baseline publication has admission, exact effects and typed receipts."""

import json
import os
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest

from jsonschema import Draft202012Validator

import native_data_fixture as fixture


class DfBaseline(fixture.NativeQueryCase):
    @classmethod
    def setUpClass(cls):
        temporary = tempfile.TemporaryDirectory(prefix="jash-df-baseline-hook-")
        cls.addClassCleanup(temporary.cleanup)
        cls.hook = str(Path(temporary.name) / "faults.so")
        subprocess.run(["cc", "-std=gnu11", "-shared", "-fPIC", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                        str(fixture.ROOT / "tests/df_baseline_faults.c"), "-ldl", "-o", cls.hook],
                       capture_output=True, check=True, timeout=30)

    def baseline(self, path="baseline.bin", rc=0, **kwargs):
        result = self.native(["df", "--data-v2", "--baseline", path, "."], rc=rc, **kwargs)
        self.assertEqual(result.count, 2)
        self.scalar(result.items[0], "row_kind", "symbol", "filesystem")
        self.scalar(result.items[-1], "row_kind", "symbol", "baseline")
        return result, result.items[-1].value["baseline"]

    def test_success_matches_private_file_and_delta_in_both_profiles(self):
        for strict in (False, True):
            with self.subTest(strict=strict):
                name = f"baseline-{strict}.bin"
                result, receipt = self.baseline(name, strict=strict, caps=["effect:read", "effect:mutate"])
                self.assertEqual((result.state, result.errors), (0, 0))
                self.scalar(receipt, "destination", "path", name.encode())
                self.scalar(receipt, "status", "symbol", "completed")
                self.scalar(receipt, "effect", "symbol", "changed")
                self.scalar(receipt, "durable", "bool", True)
                self.scalar(receipt, "verified", "bool", True)
                observed = (self.work / name).stat()
                self.assertEqual(stat.S_IMODE(observed.st_mode), 0o600)
                self.assertEqual(observed.st_nlink, 1)
                self.scalar(receipt, "bytes_written", "filesize", observed.st_size)
                delta = self.native(["df", "--data", "--delta", name, "."], strict=strict)
                self.assertEqual((delta.count, delta.errors, delta.state), (1, 0, 0))
        self.assertEqual(list(self.work.glob(".jash-df-*")), [])

    def test_existing_destinations_are_never_replaced(self):
        path = self.work / "baseline.bin"
        path.write_bytes(b"existing contents")
        result, receipt = self.baseline(rc=1)
        self.assertEqual((result.state, result.reason), (2, "baseline_exists"))
        self.scalar(receipt, "status", "symbol", "failed")
        self.scalar(receipt, "effect", "symbol", "none")
        self.scalar(receipt, "bytes_written", "filesize", 0)
        self.assertEqual(path.read_bytes(), b"existing contents")
        path.unlink()
        (self.work / "target").write_bytes(b"target")
        path.symlink_to("target")
        self.baseline(rc=1)
        self.assertTrue(path.is_symlink())
        self.assertEqual((self.work / "target").read_bytes(), b"target")

    def test_source_errors_and_read_only_authority_prevent_publication(self):
        result = self.native(["df", "--data", "--baseline", "baseline.bin", "missing"], rc=1)
        self.assertEqual(result.state, 2)
        self.assertFalse((self.work / "baseline.bin").exists())
        denied = self.run_command(["df", "--data", "--baseline", "baseline.bin", "."], strict=True,
                                  caps=["effect:read"])
        self.assertNotEqual(denied.returncode, 0)
        self.assertFalse((self.work / "baseline.bin").exists())

    def test_delivery_admission_precedes_any_baseline_artifact(self):
        for mode in ("--data", "--data-v2", "--json-report"):
            with self.subTest(mode=mode):
                result = subprocess.run([str(fixture.JASH), "-c", f"df {mode} --baseline baseline.bin . > /dev/full"],
                                        cwd=self.work, env=self.env, capture_output=True, timeout=20)
                self.assertEqual(result.returncode, 125, result.stderr)
                self.assertFalse((self.work / "baseline.bin").exists())
                self.assertEqual(list(self.work.glob(".jash-df-*")), [])

    def test_write_and_publication_failures_report_no_effect(self):
        for fault, code in (("write", "baseline_write_failed"), ("publish", "baseline_publish_failed")):
            with self.subTest(fault=fault):
                self.env.update(LD_PRELOAD=self.hook, JASH_TEST_DF_FAULT=fault)
                result, receipt = self.baseline(rc=1)
                self.assertEqual((result.state, result.reason), (2, code))
                self.scalar(receipt, "effect", "symbol", "none")
                self.scalar(receipt, "status", "symbol", "failed")
                self.assertFalse((self.work / "baseline.bin").exists())
                self.assertEqual(list(self.work.glob(".jash-df-*")), [])

    def test_failures_after_link_report_partial_changed_effect(self):
        for fault, code, durable in (("directory-sync", "baseline_directory_sync_failed", False),
                                     ("cleanup", "baseline_cleanup_failed", True)):
            with self.subTest(fault=fault):
                self.env.update(LD_PRELOAD=self.hook, JASH_TEST_DF_FAULT=fault)
                result, receipt = self.baseline(rc=1)
                self.assertEqual((result.state, result.reason), (2, code))
                self.scalar(receipt, "effect", "symbol", "changed")
                self.scalar(receipt, "status", "symbol", "partial")
                self.scalar(receipt, "durable", "bool", durable)
                self.scalar(receipt, "verified", "bool", True)
                self.assertTrue((self.work / "baseline.bin").is_file())
                (self.work / "baseline.bin").unlink()
                for temporary in self.work.glob(".jash-df-*"): temporary.unlink()

    def test_late_sink_failure_returns_transport_error_without_claiming_rollback(self):
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_DF_FAULT="sink-after-publish")
        result = self.run_command(["df", "--data", "--baseline", "baseline.bin", "."])
        self.assertEqual(result.returncode, 125, result.stderr)
        self.assertTrue((self.work / "baseline.bin").is_file())
        self.assertTrue(result.stdout.startswith(b"JTV2"))
        with self.assertRaises(AssertionError): fixture.stream(result.stdout)

    def test_json_report_uses_the_same_publication_receipt_and_raw_path(self):
        name = b"with space-\xff"
        result = self.run_command(["df", "--json-report", "--baseline", name, "."])
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        schema = json.loads((fixture.ROOT / "schemas/agent-query-df-v1.schema.json").read_text())
        Draft202012Validator.check_schema(schema)
        validator = Draft202012Validator(schema)
        validator.validate(report)
        self.assertIsNone(report["baseline_error"])
        receipt = report["baseline_receipt"]
        self.assertEqual((receipt["status"], receipt["effect"], receipt["durable"], receipt["verified"]),
                         ("completed", "changed", True, True))
        self.assertEqual(receipt["bytes_written"], os.stat(os.fsencode(self.work) + b"/" + name).st_size)
        missing_receipt = dict(report)
        del missing_receipt["baseline_receipt"]
        self.assertFalse(validator.is_valid(missing_receipt))
        invalid_type = dict(report, baseline_receipt=dict(receipt, verified="true"))
        self.assertFalse(validator.is_valid(invalid_type))
        impossible_effect = dict(report, baseline_receipt=dict(receipt, effect="none"))
        self.assertFalse(validator.is_valid(impossible_effect))
        failed = self.run_command(["df", "--json-report", "--baseline", name, "."])
        self.assertEqual(failed.returncode, 1, failed.stderr)
        failed_report = json.loads(failed.stdout)
        validator.validate(failed_report)
        self.assertEqual(failed_report["baseline_receipt"]["effect"], "none")
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_DF_FAULT="directory-sync")
        partial = self.run_command(["df", "--json-report", "--baseline", "baseline.bin", "."])
        self.assertEqual(partial.returncode, 1, partial.stderr)
        partial_report = json.loads(partial.stdout)
        validator.validate(partial_report)
        self.assertEqual(partial_report["baseline_receipt"]["status"], "partial")
        self.assertEqual(partial_report["baseline_receipt"]["effect"], "changed")
        self.assertFalse(partial_report["baseline_receipt"]["durable"])


if __name__ == "__main__":
    unittest.main()
