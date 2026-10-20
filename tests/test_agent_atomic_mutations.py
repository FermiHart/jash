#!/usr/bin/env python3
"""W03: atomic move reports describe commit and rollback, not tentative success."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import test_agent_mutations_json as fixture


class Atomic(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temp = tempfile.TemporaryDirectory(prefix="jash-atomic-hooks-")
        cls.addClassCleanup(temp.cleanup)
        cls.hook = str(Path(temp.name) / "faults.so")
        subprocess.run(["cc", "-shared", "-fPIC", str(fixture.ROOT / "tests/mutation_metadata_faults.c"), "-ldl", "-o", cls.hook],
                       check=True, capture_output=True, timeout=30)

    setUp = fixture.Mutations.setUp
    runj = fixture.Mutations.runj
    manifest = fixture.Mutations.manifest
    check_report = fixture.Mutations.check_report

    def prepare(self):
        (self.work / "first").write_text("first-new")
        (self.work / "second").write_text("second-new")
        (self.work / "dst").mkdir()
        (self.work / "dst/first").write_text("first-old")

    def test_commit_reports_completed_only_after_commit(self):
        self.prepare()
        d = self.manifest("mv", "--json", "--atomic", "first", "second", "dst")
        self.assertEqual(d["transaction"], "committed")
        self.assertEqual(d["counts"]["completed"], 2)
        self.assertEqual((self.work / "dst/first").read_text(), "first-new")
        self.assertFalse((self.work / "first").exists())

    def test_failure_rolls_back_previous_move_and_previous_destination(self):
        self.prepare(); (self.work / "dst/second").mkdir()
        d = self.manifest("mv", "--json", "--atomic", "first", "second", "dst", rc=1)
        self.assertEqual(d["transaction"], "rolled_back")
        self.assertEqual(d["items"][0]["code"], "rolled_back")
        self.assertEqual(d["counts"]["completed"], 0)
        self.assertEqual((self.work / "first").read_text(), "first-new")
        self.assertEqual((self.work / "dst/first").read_text(), "first-old")
        self.assertEqual((self.work / "second").read_text(), "second-new")

    def test_preflight_refuses_before_first_effect(self):
        self.prepare()
        d = self.manifest("mv", "--json", "--atomic", "first", "missing", "dst", rc=1)
        self.assertEqual(d["transaction"], "not_started")
        self.assertEqual((self.work / "first").read_text(), "first-new")
        self.assertEqual((self.work / "dst/first").read_text(), "first-old")

    def test_preview_and_sink_failure_leave_targets_unchanged(self):
        self.prepare()
        d = self.manifest("mv", "--json", "--atomic", "--dry-run", "first", "second", "dst")
        self.assertEqual(d["transaction"], "preview")
        self.assertEqual(d["counts"]["planned"], 2)
        p = self.runj("mv --json --atomic first second dst > /dev/full")
        self.assertEqual(p.returncode, 125)
        self.assertTrue((self.work / "first").exists())
        self.assertEqual((self.work / "dst/first").read_text(), "first-old")

    def test_cancel_after_first_publication_rolls_back(self):
        self.prepare()
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_MUTATION_FAULT="atomic_cancel")
        d = self.manifest("mv", "--json", "--atomic", "first", "second", "dst", rc=143)
        self.assertEqual(d["status"], "cancelled")
        self.assertEqual(d["transaction"], "rolled_back")
        self.assertEqual(d["items"][1]["code"], "not_attempted")
        self.assertEqual((self.work / "first").read_text(), "first-new")
        self.assertEqual((self.work / "dst/first").read_text(), "first-old")

    def test_sink_after_first_publication_rolls_back(self):
        self.prepare()
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_MUTATION_FAULT="atomic_sink")
        p = self.runj("mv --json --atomic first second dst")
        self.assertEqual(p.returncode, 125, p.stderr)
        self.assertEqual((self.work / "first").read_text(), "first-new")
        self.assertEqual((self.work / "dst/first").read_text(), "first-old")
        self.assertTrue((self.work / "second").exists())

    def test_rollback_conflict_preserves_both_versions_and_reports_hold(self):
        self.prepare(); (self.work / "dst/second").mkdir()
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_MUTATION_FAULT="atomic_rollback")
        d = self.manifest("mv", "--json", "--atomic", "first", "second", "dst", rc=1)
        self.assertEqual(d["transaction"], "partial")
        self.assertEqual(d["items"][0]["code"], "rollback_failed")
        self.assertEqual(Path(d["items"][0]["recovery_path"]).read_text(), "first-old")
        self.assertEqual((self.work / "first").read_text(), "raced")
        self.assertEqual((self.work / "dst/first").read_text(), "first-new")

    def test_commit_sync_failure_retains_preimage(self):
        self.prepare()
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_MUTATION_FAULT="atomic_commit_sync")
        d = self.manifest("mv", "--json", "--atomic", "first", "second", "dst", rc=1)
        self.assertEqual(d["transaction"], "partial")
        self.assertEqual(d["counts"]["completed"], 0)
        self.assertEqual(d["items"][0]["code"], "commit_sync_failed")
        self.assertEqual(Path(d["items"][0]["recovery_path"]).read_text(), "first-old")
        self.assertEqual((self.work / "dst/second").read_text(), "second-new")

    def test_commit_cleanup_failure_reports_retained_preimage(self):
        self.prepare()
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_MUTATION_FAULT="atomic_cleanup")
        d = self.manifest("mv", "--json", "--atomic", "first", "second", "dst", rc=1)
        self.assertEqual(d["transaction"], "partial")
        self.assertEqual(d["items"][0]["code"], "commit_cleanup_failed")
        self.assertEqual(Path(d["items"][0]["recovery_path"]).read_text(), "first-old")

    def test_no_clobber_preview_and_apply(self):
        self.prepare()
        for extra in (("--dry-run",), ()):
            d = self.manifest("mv", "--json", "--atomic", "-n", *extra, "first", "second", "dst")
            self.assertEqual(d["items"][0]["status"], "skipped")
            self.assertEqual((self.work / "dst/first").read_text(), "first-old")
            self.assertTrue((self.work / "first").exists())

    def test_directory_symlink_and_duplicate_targets(self):
        self.prepare()
        d = self.manifest("mv", "--json", "--atomic", "first", "./first", "dst", rc=1)
        self.assertEqual(d["transaction"], "not_started")
        self.assertEqual(d["items"][1]["code"], "duplicate_destination")
        (self.work / "tree").mkdir(); (self.work / "tree/file").write_text("tree-data")
        (self.work / "link").symlink_to("missing")
        self.manifest("mv", "--json", "--atomic", "tree", "link", "dst")
        self.assertEqual((self.work / "dst/tree/file").read_text(), "tree-data")
        self.assertEqual(os.readlink(self.work / "dst/link"), "missing")


if __name__ == "__main__": unittest.main()
