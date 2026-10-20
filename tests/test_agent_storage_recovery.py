#!/usr/bin/env python3
"""W08: consumed-only reclaim, explicit pins, corrupt records and dry-run."""
import json
import unittest
import subprocess
import os
import tempfile
import time
from pathlib import Path
import test_agent_recovery_refs as fixture


class Storage(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temp = tempfile.TemporaryDirectory(prefix="jash-storage-hooks-"); cls.addClassCleanup(temp.cleanup)
        cls.hook = str(Path(temp.name) / "faults.so")
        subprocess.run(["cc", "-shared", "-fPIC", str(fixture.mutations.ROOT / "tests/mutation_metadata_faults.c"), "-ldl", "-o", cls.hook],
                       check=True, capture_output=True, timeout=30)

    def setUp(self):
        fixture.Recovery.setUp(self)
        (self.home / ".jash").mkdir(mode=0o700)
    runj = fixture.Recovery.runj
    mutate = fixture.Recovery.mutate
    undo = fixture.Recovery.undo

    def store(self, args, rc=0):
        p = self.runj("undo --json " + args)
        self.assertEqual(p.returncode, rc, p.stderr)
        d = json.loads(p.stdout)
        self.assertEqual(d["schema"], "jash.recovery-store.v1")
        return d

    def test_ready_refs_survive_gc_and_consumed_refs_are_reclaimed(self):
        (self.work / "source").write_text("data")
        ready = self.mutate("cp", "source", "a")
        consumed = self.mutate("cp", "source", "b"); self.undo(consumed)
        d = self.store("--gc --dry-run")
        actions = {item["ref"]: item["action"] for item in d["items"]}
        self.assertEqual(actions[ready["ref"]], "kept")
        self.assertEqual(actions[consumed["ref"]], "would_remove")
        self.assertEqual(d["affected"], 0)
        self.assertEqual(self.store("--gc")["affected"], 1)
        self.undo(ready)

    def test_explicit_pin_prevents_reclaim(self):
        (self.work / "source").write_text("data")
        ref = self.mutate("cp", "source", "a"); self.undo(ref)
        pinned = self.store("--pin --ref " + ref["ref"])
        self.assertTrue(pinned["items"][0]["pinned"])
        self.assertEqual(self.store("--gc")["affected"], 0)
        self.store("--unpin --ref " + ref["ref"])
        self.assertEqual(self.store("--gc")["affected"], 1)

    def test_age_limit_and_corruption(self):
        (self.work / "source").write_text("data")
        ref = self.mutate("cp", "source", "a"); self.undo(ref)
        self.assertEqual(self.store("--gc --older-than 3600")["affected"], 0)
        record = self.home / ".jash/recovery-v1" / ref["ref"]
        record.write_bytes(b"damaged")
        d = self.store("--gc", rc=1)
        self.assertEqual(d["items"][0]["action"], "invalid")
        self.assertTrue(record.exists())

    def test_list_limit_and_failed_sink_are_non_destructive(self):
        (self.work / "source").write_text("data")
        refs = [self.mutate("cp", "source", name) for name in ("a", "b")]
        for ref in refs: self.undo(ref)
        d = self.store("--store --limit 1")
        self.assertEqual(d["scanned"], 1); self.assertTrue(d["truncated"])
        p = self.runj("undo --json --gc > /dev/full")
        self.assertEqual(p.returncode, 125)
        self.assertEqual(len(self.store("--store")["items"]), 2)

    def quota(self, args, rc=0):
        p = self.runj("undo --json " + args)
        self.assertEqual(p.returncode, rc, p.stderr)
        result = json.loads(p.stdout)
        self.assertEqual(result["schema"], "jash.recovery-quota.v1")
        return result

    def test_quota_refuses_new_reference_without_deleting_pins(self):
        (self.work / "source").write_text("data")
        ref = self.mutate("cp", "source", "a")
        used = self.quota("--usage")["used_bytes"]
        self.quota("--quota " + str(used))
        self.mutate("cp", "source", "b", expected=False)
        self.assertEqual(self.quota("--usage")["used_bytes"], used)
        self.undo(ref)
        self.assertEqual((self.work / "b").read_text(), "data")

    def test_quota_dry_run_and_below_usage_preserve_policy(self):
        (self.work / "source").write_text("data")
        self.mutate("cp", "source", "a")
        result = self.quota("--quota 1 --dry-run", rc=1)
        self.assertFalse(result["fits"]); self.assertFalse(result["applied"])
        self.assertEqual(self.quota("--usage")["limit_bytes"], 0)
        self.quota("--quota 1", rc=1)
        self.assertEqual(self.quota("--usage")["limit_bytes"], 0)
        self.assertFalse((self.home / ".jash/recovery-v1/.quota").exists())

    def test_quota_failed_output_precedes_configuration(self):
        p = self.runj("undo --json --quota 1024 > /dev/full")
        self.assertEqual(p.returncode, 125)
        self.assertFalse((self.home / ".jash/recovery-v1/.quota").exists())

    def test_malformed_quota_is_not_treated_as_unlimited(self):
        self.quota("--quota 100000")
        (self.home / ".jash/recovery-v1/.quota").write_bytes(b"123\0ignored")
        result = self.quota("--usage", rc=1)
        self.assertIsNone(result["limit_bytes"])
        self.assertIsNone(result["used_bytes"])
        self.assertIsNone(result["fits"])

    def test_concurrent_reference_admission_respects_quota(self):
        (self.work / "source").write_text("data")
        sample = self.mutate("cp", "source", "sample")
        size = self.quota("--usage")["used_bytes"]
        self.quota("--quota " + str(size)); self.undo(sample); self.store("--gc")
        processes = [subprocess.Popen([str(fixture.JASH), "-c", "cp --json source " + name], cwd=self.work,
                     env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE) for name in ("one", "two")]
        results = [p.communicate(timeout=20) for p in processes]
        self.assertTrue(all(p.returncode == 0 for p in processes), results)
        references = [json.loads(stdout)["items"][0]["undo"] for stdout, _ in results]
        self.assertEqual(sum(ref is not None for ref in references), 1)
        self.assertLessEqual(self.quota("--usage")["used_bytes"], size)

    def test_automatic_trash_expiry_preserves_live_reference(self):
        original = self.work / "original"; original.write_text("keep")
        os.utime(original, (1, 1))
        ref = self.mutate("rm", "--undo", "original")
        (self.work / "other").write_text("other")
        result = self.runj("rm --undo other")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.undo(ref)
        self.assertEqual(original.read_text(), "keep")

    def test_trash_gc_preserves_pinned_descendants_and_nearby_names(self):
        trash = self.home / ".jash/trash"; (trash / "dir").mkdir(parents=True)
        (self.work / "source").write_text("keep")
        child = self.mutate("cp", "source", str(trash / "dir/file"))
        nearby = self.mutate("cp", "source", str(trash / "dir-aux"))
        (trash / "unrelated").write_text("discard")
        result = self.runj("trash gc --json")
        self.assertEqual(result.returncode, 1, result.stderr)
        report = json.loads(result.stdout)
        self.assertTrue(report["pins_complete"])
        self.assertGreaterEqual(report["retained"], 2)
        self.assertFalse((trash / "unrelated").exists())
        self.assertEqual((trash / "dir/file").read_text(), "keep")
        self.undo(child); self.undo(nearby)

    def test_trash_gc_dry_run_and_failed_sink_have_no_effect(self):
        trash = self.home / ".jash/trash"; trash.mkdir(parents=True)
        (trash / "data").write_text("keep")
        result = self.runj("trash gc --json --dry-run")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["planned"], 1)
        self.assertEqual((trash / "data").read_text(), "keep")
        self.assertEqual(self.runj("trash gc --json > /dev/full").returncode, 125)
        self.assertEqual((trash / "data").read_text(), "keep")

    def test_invalid_pin_catalog_prevents_trash_cleanup(self):
        (self.work / "source").write_text("data")
        ref = self.mutate("cp", "source", "target")
        (self.home / ".jash/recovery-v1" / ref["ref"]).write_bytes(b"corrupt")
        trash = self.home / ".jash/trash"; trash.mkdir()
        (trash / "data").write_text("keep")
        result = self.runj("trash empty --json")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertFalse(json.loads(result.stdout)["pins_complete"])
        self.assertEqual((trash / "data").read_text(), "keep")

    def test_pinned_trash_root_protects_descendants(self):
        trash = self.home / ".jash/trash"
        ref = self.mutate("mkdir", str(trash))
        (trash / "child").write_text("keep")
        result = self.runj("trash gc --json")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual((trash / "child").read_text(), "keep")
        (trash / "child").unlink()
        self.undo(ref)

    def test_failed_reference_publication_exports_no_token(self):
        (self.work / "source").write_text("data")
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_MUTATION_FAULT="recovery_publish")
        self.mutate("cp", "source", "target", expected=False)
        self.assertEqual((self.work / "target").read_text(), "data")
        store = self.home / ".jash/recovery-v1"
        self.assertEqual([p for p in store.iterdir() if len(p.name) == 32 or p.name.startswith(".pending-")], [])

    def test_gc_reconciles_retained_consumed_record_after_unlink_failure(self):
        (self.work / "source").write_text("data")
        ref = self.mutate("cp", "source", "target"); self.undo(ref)
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_MUTATION_FAULT="recovery_gc_unlink")
        report = self.store("--gc", rc=1)
        self.assertEqual(report["items"][0]["action"], "remove_failed")
        self.env.pop("LD_PRELOAD")
        item, = self.store("--store")["items"]
        self.assertEqual(item["storage_state"], "gc_hold")
        self.assertEqual(item["ref"], ref["ref"])
        self.assertGreater(self.quota("--usage")["used_bytes"], 0)
        self.assertEqual(self.store("--gc")["affected"], 1)
        self.assertEqual(self.quota("--usage")["used_bytes"], 0)

    def test_killed_capture_leaves_visible_pending_debt_not_a_public_reference(self):
        (self.work / "source").write_text("data")
        marker = self.work / "paused"
        env = dict(self.env, LD_PRELOAD=self.hook, JASH_TEST_MUTATION_FAULT="recovery_pause",
                   JASH_TEST_MUTATION_PATH=str(marker), JASH_TIMELINE="1")
        p = subprocess.Popen([str(fixture.JASH), "-c", "cp --json source target"], cwd=self.work, env=env,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 5
            while not marker.exists() and p.poll() is None and time.monotonic() < deadline: time.sleep(.01)
            self.assertTrue(marker.exists())
        finally:
            p.kill(); p.communicate(timeout=10)
        store = self.home / ".jash/recovery-v1"
        self.assertEqual([p for p in store.iterdir() if len(p.name) == 32], [])
        pending, = store.glob(".pending-*")
        item, = self.store("--store", rc=1)["items"]
        self.assertEqual(item["storage_state"], "pending")
        self.assertGreater(self.quota("--usage")["used_bytes"], 0)
        self.store("--gc", rc=1); self.assertTrue(pending.exists())
        trash = self.home / ".jash/trash"; trash.mkdir(); (trash / "retained").write_text("keep")
        result = self.runj("trash gc --json")
        self.assertEqual(result.returncode, 1)
        self.assertFalse(json.loads(result.stdout)["pins_complete"])
        self.assertEqual((trash / "retained").read_text(), "keep")


if __name__ == "__main__": unittest.main()
