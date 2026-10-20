#!/usr/bin/env python3
"""Exact append preimages, deduplication and undo/redo; no shell build."""

import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SHELL = Path(os.environ.get("JASH", ROOT / "build/jash")).resolve()


class AppendUndo(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="jash-append-undo-")
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.home, self.work = self.root / "home", self.root / "work"
        self.home.mkdir()
        self.work.mkdir()
        (self.home / ".jashrc").touch()
        (self.home / "tmp").mkdir()
        self.env = {
            "HOME": str(self.home), "TMPDIR": str(self.home / "tmp"),
            "PATH": "/usr/bin:/bin", "LC_ALL": "C", "TERM": "dumb",
            "JASH_SIGIL": "0", "JASH_TIMELINE": "1", "JASH_CHANGES": "0",
            "JASH_MEMO": "0", "JASH_BRIEF": "0", "JASH_NUDGE": "0",
            "JASH_LOOPGUARD": "off", "JASH_GUARD": "0", "JASH_ABSORB": "0",
            "JASH_GUARD_BYPASS": "0", "JASH_GUARD_BYPASS_STRICT": "0",
            "JASH_SKIP_SELFCHECK": "1", "JASH_RECALL_GUARD": "0",
            "JASH_RISK_CHECKPOINT": "0", "JASH_LLM_BUDGET": "0",
            "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1", "UBSAN_OPTIONS": "halt_on_error=1",
        }

    def run_shell(self, code):
        proc = subprocess.run([str(SHELL), "-c", code], cwd=self.work, env=self.env,
                              input=b"", capture_output=True, timeout=20, umask=0o022)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertNotIn(b"AddressSanitizer", proc.stderr)
        self.assertNotIn(b"runtime error:", proc.stderr)
        self.assertEqual(proc.stderr, b"")
        backups = list(self.home.rglob("bk_*"))
        for backup in backups:
            self.assertNotIn(".tmp.", backup.name, "no incomplete publication")
            self.assertEqual(backup.name, "bk_" + hashlib.sha256(backup.read_bytes()).hexdigest())
        return proc.stdout, backups

    def test_append_undo_and_redo_chain(self):
        (self.work / "log").write_bytes(b"seed")
        out, _ = self.run_shell(
            "printf A >>log; printf B >>log; undo >/dev/null; cat log; printf '|'; "
            "undo >/dev/null; cat log; printf '|'; redo >/dev/null; cat log; printf '|'; "
            "redo >/dev/null; cat log")
        self.assertEqual(out, b"seedA|seed|seedA|seedAB")
        self.assertEqual((self.work / "log").read_bytes(), b"seedAB")

    def test_external_prefix_mutation_is_fully_restored(self):
        seed = b"original-prefix\0binary\xff-tail"
        (self.work / "log").write_bytes(seed)
        out, _ = self.run_shell(
            "printf APPEND >>log; /bin/sh -c 'printf REPLACED >log'; "
            "undo >/dev/null; cat log")
        self.assertEqual(out, seed)
        self.assertEqual((self.work / "log").read_bytes(), seed)

    def test_new_mutation_after_redo_invalidates_remaining_stack(self):
        (self.work / "log").write_bytes(b"seed")
        out, _ = self.run_shell(
            "printf A >>log; printf B >>log; undo 2 >/dev/null; redo >/dev/null; "
            "printf C >>log; redo >/dev/null; status=$?; printf '%s|' \"$status\"; cat log")
        self.assertEqual(out, b"1|seedAC")

    def test_multi_redo_and_failed_redo_hold(self):
        (self.work / "log").write_bytes(b"seed")
        out, _ = self.run_shell(
            "printf A >>log; printf B >>log; undo 2 >/dev/null; redo 2 >/dev/null; cat log")
        self.assertEqual(out, b"seedAB")
        (self.work / "log").write_bytes(b"seed")
        (self.work / "restore").write_bytes(b"seedA")
        missing = self.home / "tmp/shared" / ("bk_" + hashlib.sha256(b"seedA").hexdigest())
        quoted = shlex.quote(str(missing))
        out, _ = self.run_shell(
            f"printf A >>log; undo >/dev/null; /bin/rm {quoted}; "
            "redo >/dev/null; status=$?; printf '%s|' \"$status\"; "
            f"/bin/cp restore {quoted}; redo >/dev/null; cat log")
        self.assertEqual(out, b"1|seedA")

    def test_external_change_between_appends(self):
        (self.work / "log").write_bytes(b"seed")
        out, _ = self.run_shell(
            "printf A >>log; /bin/sh -c 'printf changed >log'; printf B >>log; "
            "undo >/dev/null; cat log; printf '|'; undo >/dev/null; cat log")
        self.assertEqual(out, b"changed|seed")

    def test_binary_preimages_at_capture_boundaries(self):
        for size in (0, 1, 63, 64, 65, 4096, 65535, 65536, 65537, 131072):
            with self.subTest(size=size):
                seed = (bytes(range(256)) * ((size + 255) // 256))[:size]
                name = f"file-{size}"
                (self.work / name).write_bytes(seed)
                out, backups = self.run_shell(f"printf '%b' 'A\\0B' >>{name}; undo >/dev/null")
                self.assertEqual(out, b"")
                self.assertEqual((self.work / name).read_bytes(), seed)
                self.assertTrue(any(p.read_bytes() == seed for p in backups))

    def test_unchanged_preimages_still_deduplicate(self):
        (self.work / "log").write_bytes(b"unchanged\n")
        _, backups = self.run_shell("for ((i=0;i<12;i++)); do printf '' >>log; done")
        self.assertEqual(len(backups), 1)

    def test_hardlink_and_quoted_path(self):
        name = "a 'quoted' log"
        (self.work / name).write_bytes(b"seed")
        (self.work / "alias").hardlink_to(self.work / name)
        out, _ = self.run_shell(
            f"printf more >>{shlex.quote(name)}; undo >/dev/null; cat alias")
        self.assertEqual(out, b"seed")
        self.assertEqual((self.work / name).stat().st_ino, (self.work / "alias").stat().st_ino)

    def test_new_file_and_checkpoint(self):
        (self.work / "log").write_bytes(b"seed")
        out, _ = self.run_shell(
            "checkpoint point >/dev/null; printf A >>log; printf B >>log; "
            "rewind point >/dev/null; cat log; "
            "printf new >>created; undo >/dev/null")
        self.assertEqual(out, b"seed")
        self.assertFalse((self.work / "created").exists())

    def test_recreated_store_is_used_by_next_append(self):
        (self.work / "log").write_bytes(b"seed")
        out, _ = self.run_shell(
            'printf A >>log; /bin/rm -r "$TMPDIR/shared"; '
            'printf B >>log; undo >/dev/null; cat log')
        self.assertEqual(out, b"seedA")

    def test_renamed_store_and_restored_history(self):
        (self.work / "log").write_bytes(b"seed")
        out, _ = self.run_shell(
            'printf A >>log; /bin/mv "$TMPDIR/shared" "$TMPDIR/old-store"; '
            '/bin/mkdir "$TMPDIR/shared"; printf B >>log; undo >/dev/null; cat log; '
            'printf "|"; /bin/mv "$TMPDIR/shared" "$TMPDIR/new-store"; '
            '/bin/mv "$TMPDIR/old-store" "$TMPDIR/shared"; undo >/dev/null; cat log')
        self.assertEqual(out, b"seedA|seed")

    def test_replaced_store_symlink_and_write_only_directory(self):
        (self.work / "log").write_bytes(b"seed")
        out, _ = self.run_shell(
            'printf A >>log; /bin/mv "$TMPDIR/shared" "$TMPDIR/old-store"; '
            '/bin/mkdir "$TMPDIR/replacement"; '
            '/bin/ln -s "$TMPDIR/replacement" "$TMPDIR/shared"; '
            '/bin/chmod 300 "$TMPDIR/replacement"; '
            'printf B >>log; undo >/dev/null; cat log; '
            '/bin/chmod 700 "$TMPDIR/replacement"')
        self.assertEqual(out, b"seedA")

    @unittest.skipUnless(shutil.which("strace"), "requires strace")
    def test_store_creation_is_not_repeated(self):
        (self.work / "log").write_bytes(b"seed")
        trace = self.root / "mkdir.log"
        proc = subprocess.run(
            ["strace", "-f", "-e", "trace=mkdir,mkdirat", "-o", str(trace), str(SHELL),
             "-c", "for ((i=0;i<100;i++)); do printf x >>log; done"],
            cwd=self.work, env=self.env, input=b"", capture_output=True, timeout=20)
        if b"Operation not permitted" in proc.stderr:
            self.skipTest("ptrace unavailable")
        self.assertEqual((proc.returncode, proc.stdout, proc.stderr), (0, b"", b""))
        self.assertEqual((self.work / "log").read_bytes(), b"seed" + b"x" * 100)
        calls = sum('/shared"' in line for line in trace.read_text().splitlines())
        self.assertEqual(calls, 1, f"{calls} store mkdir attempts")

    @unittest.skipUnless(shutil.which("strace"), "requires strace")
    def test_plain_names_do_not_reresolve_cwd_per_append(self):
        (self.work / "log").write_bytes(b"seed")
        trace = self.root / "trace.log"
        proc = subprocess.run(
            ["strace", "-f", "-e", "trace=readlink,readlinkat", "-o", str(trace),
             str(SHELL), "-c", "for ((i=0;i<100;i++)); do printf x >>log; done"],
            cwd=self.work, env=self.env, input=b"", capture_output=True, timeout=20)
        if b"Operation not permitted" in proc.stderr:
            self.skipTest("ptrace unavailable")
        self.assertEqual((proc.returncode, proc.stdout, proc.stderr), (0, b"", b""))
        self.assertEqual((self.work / "log").read_bytes(), b"seed" + b"x" * 100)
        calls = sum("readlink(" in line or "readlinkat(" in line for line in trace.read_text().splitlines())
        self.assertLess(calls, 100, f"{calls} path-resolution calls for 100 plain-name appends")


if __name__ == "__main__":
    unittest.main()
