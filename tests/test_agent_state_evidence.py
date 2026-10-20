#!/usr/bin/env python3
"""W05: exact bounded timeline state, identity and pagination."""
import json
import os
import shlex
import unittest
import subprocess
import tempfile
from pathlib import Path
import fcntl
import test_agent_mutations_json as fixture
import test_agent_recovery_refs as recovery_fixture


class State(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temp = tempfile.TemporaryDirectory(prefix="jash-checkpoint-hooks-"); cls.addClassCleanup(temp.cleanup)
        cls.hook = str(Path(temp.name) / "faults.so")
        subprocess.run(["cc", "-shared", "-fPIC", str(fixture.ROOT / "tests/mutation_metadata_faults.c"), "-ldl", "-o", cls.hook],
                       check=True, capture_output=True, timeout=30)

    setUp = fixture.Mutations.setUp
    runj = fixture.Mutations.runj

    def test_live_timeline_ids_and_page(self):
        self.env["JASH_TIMELINE"] = "1"
        p = self.runj("mkdir first second third; timeline --json --offset 1 --limit 1")
        self.assertEqual(p.returncode, 0, p.stderr)
        d = json.loads(p.stdout)
        self.assertEqual(d["schema"], "jash.timeline.v1")
        self.assertEqual(d["mutations"], 3)
        self.assertEqual(d["count"], 1)
        self.assertEqual(d["next_offset"], 2)
        self.assertEqual(d["entries"][0]["a"], str(self.work / "second"))
        self.assertEqual(str(d["entries"][0]["id"]), d["entries"][0]["entry_id"])

    def test_byte_paths_and_small_budget(self):
        self.env.update(JASH_TIMELINE="1", JASH_LLM_BUDGET="64")
        name = os.fsdecode(b"raw-\xff")
        p = self.runj(shlex.join(["mkdir", name]) + "; timeline --json")
        self.assertEqual(p.returncode, 0, p.stderr)
        d = json.loads(p.stdout)
        self.assertEqual(fixture.path_bytes(d["entries"][0]["a"]), os.fsencode(self.work / name))
        self.assertTrue(d["entries"][0]["undoes"])

    def test_empty_sink_failure_and_invalid_arguments(self):
        p = self.runj("timeline --json")
        self.assertEqual(json.loads(p.stdout)["entries"], [])
        self.assertEqual(self.runj("timeline --json > /dev/full").returncode, 125)
        for args in ("--limit 0", "--offset -1", "--bogus"):
            self.assertEqual(self.runj("timeline --json " + args).returncode, 2)

    def test_checkpoint_json_preserves_bytes_and_reports_bad_rows(self):
        state = self.home / ".jash"; state.mkdir(exist_ok=True)
        (state / "checkpoints.tsv").write_bytes(b'quote"-\xff\t0\t123\t/tmp/path-\xfe\nmalformed\n')
        self.env["JASH_LLM_BUDGET"] = "32"
        p = self.runj("checkpoint --json")
        self.assertEqual(p.returncode, 1)
        result = json.loads(p.stdout)
        self.assertEqual(result["schema"], "jash.checkpoints.v1")
        self.assertEqual(result["invalid_rows"], 1)
        self.assertFalse(result["scan_complete"])
        self.assertEqual(fixture.path_bytes(result["checkpoints"][0]["name"]), b'quote"-\xff')
        self.assertEqual(fixture.path_bytes(result["checkpoints"][0]["cwd"]), b'/tmp/path-\xfe')

    def test_checkpoint_failed_sink_and_fifo_do_not_succeed(self):
        self.assertEqual(self.runj("checkpoint --json > /dev/full").returncode, 125)
        state = self.home / ".jash"; state.mkdir(exist_ok=True)
        os.mkfifo(state / "checkpoints.tsv")
        p = self.runj("checkpoint --json")
        self.assertEqual(p.returncode, 1)
        self.assertFalse(json.loads(p.stdout)["scan_complete"])

    def test_checkpoint_corruption_is_preserved_by_writers_and_rewind(self):
        state = self.home / ".jash"; state.mkdir(exist_ok=True)
        path = state / "checkpoints.tsv"
        for data in (b"malformed\n", b"partial\t0\t123\t/tmp", b"nul\0\t0\t123\t/tmp\n"):
            path.write_bytes(data)
            for command in ("checkpoint fresh", "checkpoint list", "rewind --session"):
                p = self.runj(command)
                self.assertEqual(p.returncode, 1, (command, p.stdout, p.stderr))
                self.assertEqual(path.read_bytes(), data)

    def test_checkpoint_file_sync_failure_preserves_previous_snapshot(self):
        self.assertEqual(self.runj("checkpoint first").returncode, 0)
        path = self.home / ".jash/checkpoints.tsv"; before = path.read_bytes(); inode = path.stat().st_ino
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_MUTATION_FAULT="checkpoint_file_sync")
        self.assertEqual(self.runj("checkpoint second").returncode, 1)
        self.assertEqual(path.read_bytes(), before)
        self.assertEqual(path.stat().st_ino, inode)
        self.assertEqual(list(path.parent.glob(".jash-checkpoint-*.tmp")), [])

    def test_checkpoint_directory_sync_failure_is_reported_after_publication(self):
        self.assertEqual(self.runj("checkpoint first").returncode, 0)
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_MUTATION_FAULT="sed_parent_sync", JASH_TEST_MUTATION_PATH=str(self.home / ".jash"))
        p = self.runj("checkpoint second")
        self.assertEqual(p.returncode, 1)
        d = json.loads(self.runj("checkpoint --json").stdout)
        self.assertEqual({c["name"] for c in d["checkpoints"]}, {"first", "second"})

    def test_checkpoint_stable_lock_covers_read_modify_publish(self):
        self.assertEqual(self.runj("checkpoint first").returncode, 0)
        state = self.home / ".jash"
        with (state / "checkpoints.tsv.lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            p = subprocess.Popen([str(fixture.JASH), "-c", "checkpoint second"], cwd=self.work, env=self.env,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                with self.assertRaises(subprocess.TimeoutExpired): p.wait(timeout=.3)
            finally:
                fcntl.flock(lock, fcntl.LOCK_UN)
                output = p.communicate(timeout=10)
            self.assertEqual(p.returncode, 0, output)
        d = json.loads(self.runj("checkpoint --json").stdout)
        self.assertEqual({c["name"] for c in d["checkpoints"]}, {"first", "second"})

    def test_checkpoint_parallel_creates_do_not_lose_unrelated_names(self):
        processes = [subprocess.Popen([str(fixture.JASH), "-c", f"checkpoint marker{i}"], cwd=self.work, env=self.env,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE) for i in range(20)]
        results = [p.communicate(timeout=20) for p in processes]
        self.assertTrue(all(p.returncode == 0 for p in processes), results)
        d = json.loads(self.runj("checkpoint --json").stdout)
        self.assertTrue(d["scan_complete"])
        self.assertEqual({c["name"] for c in d["checkpoints"]}, {f"marker{i}" for i in range(20)})

    def test_checkpoint_refuses_long_names_and_symlink_store(self):
        self.assertEqual(self.runj("checkpoint " + "a" * 65).returncode, 2)
        state = self.home / ".jash"; state.mkdir(exist_ok=True)
        victim = self.work / "victim"; victim.write_text("unchanged")
        (state / "checkpoints.tsv").symlink_to(victim)
        self.assertEqual(self.runj("checkpoint fresh").returncode, 1)
        self.assertEqual(victim.read_text(), "unchanged")

    def test_checkpoint_private_state_remains_readable_with_restrictive_umask(self):
        self.assertEqual(self.runj("umask 0777; checkpoint private").returncode, 0)
        path = self.home / ".jash/checkpoints.tsv"
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        self.assertEqual(json.loads(self.runj("checkpoint --json").stdout)["checkpoints"][0]["name"], "private")

    def test_rewind_preserves_changed_snapshot_and_reports_failed_pruning(self):
        (self.work / "source").write_text("data")
        self.env.update(JASH_TIMELINE="1", LD_PRELOAD=self.hook, JASH_TEST_MUTATION_FAULT="checkpoint_prune_race",
                        JASH_TEST_MUTATION_PATH=str(self.home / ".jash/checkpoints.tsv"))
        p = self.runj("checkpoint base > /dev/null; cp --json source created > /dev/null; checkpoint newer > /dev/null; rewind base")
        self.assertEqual(p.returncode, 1, (p.stdout, p.stderr))
        self.assertIn(b"checkpoint pruning failed", p.stderr)
        self.assertFalse((self.work / "created").exists())
        d = json.loads(self.runj("checkpoint --json").stdout)
        self.assertTrue(d["scan_complete"])
        self.assertEqual({c["name"] for c in d["checkpoints"]}, {"base", "newer", "concurrent"})

    def test_full_id_selects_exact_entry_and_invalid_undo_has_no_effect(self):
        self.env["JASH_TIMELINE"] = "1"
        p = subprocess.Popen([str(fixture.JASH), "--serve"], cwd=self.work, env=self.env,
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(lambda: p.poll() is None and p.kill())
        p.stdout.readline()
        def request(argv):
            p.stdin.write((json.dumps({"argv": argv}) + "\n").encode()); p.stdin.flush()
            return json.loads(p.stdout.readline())
        self.assertEqual(request(["mkdir", "first", "second"])["rc"], 0)
        timeline = json.loads(request(["timeline", "--json"])["stdout"])
        identifier = timeline["entries"][0]["entry_id"]
        self.assertGreater(int(identifier), 2**32)
        preview = request(["undo", "--dry", "id", identifier])
        self.assertEqual(preview["rc"], 0)
        self.assertIn(str(self.work / "first"), preview["stdout"])
        self.assertEqual(request(["undo", "id", identifier])["rc"], 0)
        self.assertFalse((self.work / "first").exists())
        for argv in (["undo", "id"], ["undo", "--bogus"], ["undo", "--quota", "1"]):
            self.assertEqual(request(argv)["rc"], 2)
            self.assertTrue((self.work / "second").exists())
        p.stdin.close(); p.wait(timeout=10); p.stdout.close(); p.stderr.close()

    def test_ambiguous_short_id_is_not_silently_selected(self):
        self.env.update(JASH_TIMELINE="1", JASH_SIGIL="1")
        subprocess.run(["bash", str(fixture.ROOT / "tests/setup_sigil_modules.sh"), str(self.home)],
                       check=True, capture_output=True, timeout=30)
        self.assertEqual(self.runj("mkdir first").returncode, 0)
        self.assertEqual(self.runj("mkdir second").returncode, 0)
        result = self.runj("undo id 1")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(b"ambiguous", result.stdout)
        self.assertTrue((self.work / "first").exists())
        self.assertTrue((self.work / "second").exists())


class RecoveryState(unittest.TestCase):
    setUp = recovery_fixture.Recovery.setUp
    runj = recovery_fixture.Recovery.runj
    mutate = recovery_fixture.Recovery.mutate
    undo = recovery_fixture.Recovery.undo

    def test_inspection_preserves_consumed_identity(self):
        (self.work / "source").write_text("data")
        ref = self.mutate("cp", "source", "target")
        query = f"undo --json --inspect --ref {ref['ref']}"
        first = json.loads(self.runj(query).stdout)
        self.assertEqual(first["status"], "inspected")
        self.assertEqual(first["record"]["entry_id"], ref["entry_id"])
        self.assertEqual(first["record"]["state"], "ready")
        self.assertFalse(first["record"]["target_validated"])
        self.undo(ref)
        second = json.loads(self.runj(query).stdout)
        self.assertEqual(second["record"]["state"], "consumed")

    def test_inspection_does_not_claim_stale_target_is_valid(self):
        (self.work / "source").write_text("data")
        ref = self.mutate("cp", "source", "target")
        (self.work / "target").write_text("changed")
        p = self.runj(f"undo --json --inspect --ref {ref['ref']}")
        self.assertEqual(p.returncode, 0)
        self.assertFalse(json.loads(p.stdout)["record"]["target_validated"])
        self.assertEqual(self.undo(ref, dry=True, rc=1)["code"], "target_changed")


if __name__ == "__main__": unittest.main()
