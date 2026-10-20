#!/usr/bin/env python3
"""W03: durable undo references checked against real postimages and inverses."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
import test_agent_mutations_json as mutations
JASH = mutations.JASH


class Recovery(unittest.TestCase):
    runj = mutations.Mutations.runj

    def setUp(self):
        mutations.Mutations.setUp(self)
        self.tmpdir = self.root / "tmp"
        self.tmpdir.mkdir()
        self.env["TMPDIR"] = str(self.tmpdir)

    def mutate(self, *argv, expected=True):
        self.env["JASH_TIMELINE"] = "1"
        p = self.runj(shlex.join([argv[0], "--json", *argv[1:]]))
        self.assertEqual(p.returncode, 0, p.stderr)
        data = json.loads(p.stdout)
        reference = data["items"][-1]["undo"]
        if expected:
            self.assertIsNotNone(reference, data)
            self.assertEqual(reference["scope"], "local-user-store")
            self.assertGreater(int(reference["entry_id"]), 2**32)
            self.assertRegex(reference["ref"], r"^[0-9a-f]{32}$")
        else: self.assertIsNone(reference, data)
        return reference

    def undo(self, ref, dry=False, rc=0):
        argv = ["undo", "--json", "--ref", ref["ref"]]
        if dry: argv.append("--dry-run")
        p = self.runj(shlex.join(argv))
        self.assertEqual(p.returncode, rc, p.stderr)
        data = json.loads(p.stdout)
        self.assertEqual(data["schema"], "jash.undo-result.v1")
        self.assertEqual(data["exit_code"], rc)
        return data

    def test_copy_create_reference_works_after_origin_process_exits(self):
        (self.work / "source").write_text("value")
        ref = self.mutate("cp", "source", "created")
        self.assertEqual(self.undo(ref, dry=True)["status"], "ready")
        self.assertTrue((self.work / "created").exists())
        self.assertEqual(self.undo(ref)["status"], "completed")
        self.assertFalse((self.work / "created").exists())
        self.assertEqual(self.undo(ref, rc=1)["code"], "consumed")
        self.assertEqual((self.work / "source").read_text(), "value")

    def test_content_preimage_is_independent_of_legacy_backup_retention(self):
        (self.work / "source").write_bytes(b"new\x00bytes")
        (self.work / "target").write_bytes(b"old\x00bytes")
        ref = self.mutate("cp", "source", "target")
        self.assertEqual(ref["coverage"], "file_content")
        # The standalone reference owns its preimage; remove the legacy backup
        # belonging to this unique fixture rather than relying on its lifetime.
        import hashlib
        backup = self.tmpdir / "shared" / ("bk_" + hashlib.sha256(b"old\x00bytes").hexdigest())
        self.assertTrue(backup.exists())
        backup.unlink()
        self.assertEqual(self.undo(ref)["status"], "completed")
        self.assertEqual((self.work / "target").read_bytes(), b"old\x00bytes")

    def test_sed_content_reference_and_byte_path(self):
        name = os.fsdecode(b"target-\xff")
        (self.work / name).write_text("old\n")
        ref = self.mutate("sed", "-i", "s/old/new/", name)
        self.assertEqual((self.work / name).read_text(), "new\n")
        self.undo(ref)
        self.assertEqual((self.work / name).read_text(), "old\n")

    def test_mkdir_and_rmdir_references(self):
        ref = self.mutate("mkdir", "-m", "0750", "dir")
        (self.work / "dir/child").write_text("keep")
        self.assertEqual(self.undo(ref, rc=1)["code"], "directory_not_empty")
        (self.work / "dir/child").unlink()
        self.undo(ref)
        self.assertFalse((self.work / "dir").exists())
        (self.work / "dir").mkdir(mode=0o751)
        ref = self.mutate("rmdir", "dir")
        self.undo(ref)
        self.assertEqual((self.work / "dir").stat().st_mode & 0o777, 0o751)

    def test_link_reference_removes_only_the_link(self):
        (self.work / "source").write_text("keep")
        for flags, target in (([], "source"), (["-s"], "absent")):
            ref = self.mutate("ln", *flags, target, "link")
            self.undo(ref)
            self.assertFalse(os.path.lexists(self.work / "link"))
            self.assertEqual((self.work / "source").read_text(), "keep")

    def test_metadata_reference_restores_mode(self):
        (self.work / "target").write_text("data")
        (self.work / "target").chmod(0o640)
        ref = self.mutate("chmod", "0750", "target")
        self.undo(ref)
        self.assertEqual((self.work / "target").stat().st_mode & 0o777, 0o640)

    def test_no_clobber_move_reference_restores_original_name(self):
        (self.work / "source").write_text("data")
        ref = self.mutate("mv", "-n", "source", "target")
        self.assertEqual(ref["coverage"], "moved_entry")
        self.undo(ref, dry=True)
        self.undo(ref)
        self.assertEqual((self.work / "source").read_text(), "data")
        self.assertFalse((self.work / "target").exists())

    def test_move_reference_preserves_recreated_source(self):
        (self.work / "source").write_text("data")
        ref = self.mutate("mv", "-n", "source", "target")
        (self.work / "source").write_text("new")
        self.assertEqual(self.undo(ref, rc=1)["code"], "destination_exists")
        self.assertEqual((self.work / "source").read_text(), "new")
        self.assertEqual((self.work / "target").read_text(), "data")

    def test_move_directory_and_dangling_link_references(self):
        (self.work / "dir").mkdir(); (self.work / "dir/file").write_text("data")
        ref = self.mutate("mv", "-n", "dir", "moved")
        self.undo(ref)
        self.assertEqual((self.work / "dir/file").read_text(), "data")
        (self.work / "link").symlink_to("absent")
        ref = self.mutate("mv", "-n", "link", "moved-link")
        self.undo(ref)
        self.assertEqual(os.readlink(self.work / "link"), "absent")

    def test_trash_reference_returns_the_exact_entry(self):
        name = os.fsdecode(b"trash-\xff")
        (self.work / name).write_text("data")
        ref = self.mutate("rm", "--undo", name)
        self.assertEqual(ref["coverage"], "trashed_entry")
        self.assertFalse((self.work / name).exists())
        self.undo(ref)
        self.assertEqual((self.work / name).read_text(), "data")

    def test_changed_content_and_replaced_inode_are_refused(self):
        (self.work / "source").write_text("original")
        for replace in (False, True):
            ref = self.mutate("cp", "source", "target")
            if replace: (self.work / "target").unlink()
            (self.work / "target").write_text("new-user-data")
            self.assertEqual(self.undo(ref, rc=1)["code"], "target_changed")
            self.assertEqual((self.work / "target").read_text(), "new-user-data")
            (self.work / "target").unlink()

    def test_replaced_parent_is_refused(self):
        (self.work / "dir").mkdir()
        (self.work / "source").write_text("old")
        ref = self.mutate("cp", "source", "dir/target")
        (self.work / "dir").rename(self.work / "original-dir")
        (self.work / "dir").mkdir()
        (self.work / "dir/target").write_text("new")
        self.assertEqual(self.undo(ref, rc=1)["code"], "parent_changed")
        self.assertEqual((self.work / "original-dir/target").read_text(), "old")
        self.assertEqual((self.work / "dir/target").read_text(), "new")

    def test_absent_and_damaged_references(self):
        (self.work / "source").write_text("data")
        ref = self.mutate("cp", "source", "target")
        record = self.home / ".jash/recovery-v1" / ref["ref"]
        raw = bytearray(record.read_bytes())
        raw[-1] ^= 1
        record.write_bytes(raw)
        self.assertEqual(self.undo(ref, rc=1)["code"], "damaged_reference")
        record.unlink()
        self.assertEqual(self.undo(ref, rc=1)["code"], "reference_unavailable")
        self.assertEqual((self.work / "target").read_text(), "data")

    def test_reference_cannot_escape_store_or_use_another_home(self):
        (self.work / "source").write_text("data")
        ref = self.mutate("cp", "source", "target")
        for token in ("../timeline.bin", "f" * 33, "0" * 31, ""):
            self.assertEqual(self.undo({"ref": token}, rc=2)["code"], "invalid_arguments")
        other = self.root / "other-home"; other.mkdir()
        self.env["HOME"] = str(other)
        self.assertEqual(self.undo(ref, rc=1)["code"], "reference_unavailable")
        self.assertTrue((self.work / "target").exists())

    def test_no_ref_without_recording_or_for_unsupported_inverse(self):
        (self.work / "source").write_text("data")
        p = self.runj("cp --json source target")
        self.assertIsNone(json.loads(p.stdout)["items"][0]["undo"])
        self.mutate("touch", "target")
        self.mutate("mv", "source", "moved", expected=False)

    def test_touch_recovery_preserves_nanosecond_preimage(self):
        target = self.work / "target"; target.write_text("data")
        old = (1700000000123456789, 1700000000987654321)
        os.utime(target, ns=old)
        ref = self.mutate("touch", "-m", "-d", "@42", "target")
        self.assertEqual(ref["coverage"], "timestamps")
        self.undo(ref)
        st = target.stat()
        self.assertEqual((st.st_atime_ns, st.st_mtime_ns), old)

    def test_touch_timestamps_are_visible_in_agent_envelope(self):
        (self.work / "target").write_text("data")
        self.env.update(JASH_TIMELINE="1", JASH_AGENT="1")
        p = self.runj("touch --json -d @42 target")
        self.assertEqual(p.returncode, 0, p.stderr)
        envelope = next(json.loads(line) for line in p.stderr.splitlines() if line.startswith(b'{"jash_agent":'))
        self.assertIn(str(self.work / "target"), envelope["metadata_changed"])

    def test_touch_created_file_and_symlink_recovery(self):
        ref = self.mutate("touch", "new")
        self.undo(ref)
        self.assertFalse((self.work / "new").exists())
        target = self.work / "link"; target.symlink_to("missing")
        os.utime(target, ns=(1000000001, 2000000002), follow_symlinks=False)
        ref = self.mutate("touch", "-h", "-d", "@50", "link")
        self.undo(ref)
        self.assertEqual(target.lstat().st_mtime_ns, 2000000002)
        self.assertFalse((self.work / "missing").exists())

    def test_store_symlink_and_overlong_journal_path_have_no_reference(self):
        (self.work / "source").write_text("data")
        outside = self.root / "outside"; outside.mkdir()
        (self.home / ".jash").mkdir(exist_ok=True)
        (self.home / ".jash/recovery-v1").symlink_to(outside)
        self.mutate("cp", "source", "target", expected=False)
        self.assertEqual(list(outside.iterdir()), [])
        (self.home / ".jash/recovery-v1").unlink()
        directory = self.work
        for _ in range(3): directory = directory / ("p" * 180); directory.mkdir()
        self.mutate("cp", "source", str(directory / "target"), expected=False)
        self.assertEqual((directory / "target").read_text(), "data")

    def test_corrupt_legacy_dedup_hit_is_not_exported(self):
        import hashlib
        (self.work / "source").write_text("new")
        (self.work / "target").write_text("old")
        shared = self.tmpdir / "shared"; shared.mkdir()
        (shared / ("bk_" + hashlib.sha256(b"old").hexdigest())).write_text("BAD")
        self.mutate("cp", "source", "target", expected=False)
        self.assertEqual((self.work / "target").read_text(), "new")

    def test_hardlinked_overwrite_does_not_claim_complete_content_recovery(self):
        (self.work / "source").write_text("new")
        (self.work / "target").write_text("old")
        os.link(self.work / "target", self.work / "alias")
        self.mutate("cp", "source", "target", expected=False)
        self.assertEqual((self.work / "alias").read_text(), "new")

    def test_fifo_or_symlink_reference_cannot_block_or_read_other_data(self):
        (self.work / "source").write_text("data")
        ref = self.mutate("cp", "source", "target")
        record = self.home / ".jash/recovery-v1" / ref["ref"]
        record.unlink(); os.mkfifo(record)
        self.assertEqual(self.undo(ref, rc=1)["code"], "invalid_reference")
        record.unlink(); record.symlink_to(self.work / "source")
        self.assertEqual(self.undo(ref, rc=1)["code"], "reference_unavailable")
        self.assertEqual((self.work / "target").read_text(), "data")

    def test_output_failure_precedes_undo(self):
        (self.work / "source").write_text("data")
        ref = self.mutate("cp", "source", "target")
        p = self.runj(f"undo --json --ref {ref['ref']} > /dev/full")
        self.assertEqual(p.returncode, 125, p.stderr)
        self.assertEqual((self.work / "target").read_text(), "data")
        self.undo(ref)

    def test_private_capture_is_usable_under_restrictive_umask(self):
        (self.work / "source").write_text("data")
        ref = self.mutate("cp", "source", "target")
        p = self.runj(f"umask 777; undo --json --ref {ref['ref']}")
        self.assertEqual(p.returncode, 0, (p.stdout, p.stderr))
        self.assertFalse((self.work / "target").exists())

    def test_concurrent_application_consumes_only_once(self):
        (self.work / "source").write_text("data")
        ref = self.mutate("cp", "source", "target")
        processes = [subprocess.Popen([str(JASH), "-c", f"undo --json --ref {ref['ref']}"],
                     cwd=self.work, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE) for _ in range(2)]
        results = [p.communicate(timeout=20) for p in processes]
        self.assertEqual(sorted(p.returncode for p in processes), [0, 1], results)
        self.assertEqual({json.loads(out)["status"] for out, err in results}, {"completed", "failed"})


class RecoveryCore(unittest.TestCase):
    def test_partial_restore_conflict_and_record_integrity(self):
        with tempfile.TemporaryDirectory(prefix="jash-recovery-core-") as tmp:
            root = mutations.ROOT
            binary = str(Path(tmp) / "recovery-core")
            work = Path(tmp) / "work"; work.mkdir()
            sources = [root / source for source in (
                "tests/test_recovery_core.c", "c/core/argv.c", "c/parser/cmdsub.c",
                "c/core/json_writer.c", "c/module/jash_crypto.c", "c/builtins/discovery_output.c",
                "c/core/structured_output.c", "c/core/data_json.c", "c/core/data_value.c",
                "c/core/data_stream.c", "c/core/data_wire.c", "c/core/data_wire_decode.c",
                "c/core/data_wire_import.c", "c/core/data_emit.c", "c/module/blake3.c", "c/theme/io.c")]
            command = [os.environ.get("CC", "cc"), "-std=gnu11", "-D_GNU_SOURCE", "-DJASH_TARGET_POSIX", "-O1", "-g",
                       "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                       "-I" + str(root / "c"), *map(str, sources), "-lm", "-o", binary]
            if os.environ.get("SANITIZE") == "1":
                command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
            built = subprocess.run(command, capture_output=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([binary, str(work)], capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
