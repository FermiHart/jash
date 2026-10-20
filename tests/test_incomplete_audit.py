#!/usr/bin/env python3
"""Behavioral evidence for AUDIT_JASH_INCOMPLETE_FEATURES.md.

Fixtures and HOME are disposable; invoke the freshly built shell by absolute
path so neither PATH fallback nor the agent's own shell can supply the result.
"""
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
JASH = Path(os.environ.get("JASH", ROOT / "build/jash")).resolve()


class Audit(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.hook = None
        if sys.platform == "linux":
            tmp = tempfile.TemporaryDirectory(prefix="jash-audit-hook-")
            cls.addClassCleanup(tmp.cleanup)
            cls.hook = str(Path(tmp.name) / "faults.so")
            subprocess.run(["cc", "-shared", "-fPIC", str(ROOT / "tests/audit_cp_faults.c"),
                            "-ldl", "-o", cls.hook], check=True, capture_output=True)

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="jash-audit-")
        self.addCleanup(self.tmp.cleanup)
        self.cwd = Path(self.tmp.name)
        home = self.cwd / "home"
        home.mkdir()
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("JASH_")}
        self.env.update(HOME=str(home), PATH="/usr/bin:/bin", TZ="UTC",
                        JASH_SIGIL="0", JASH_ADOPT="0", JASH_MEMO="0",
                        JASH_NUDGE="0", JASH_BRIEF="0", JASH_LOOPGUARD="off")

    def runj(self, *args, stdin="", ok=True):
        return self.runscript(shlex.join(args), stdin=stdin, ok=ok)

    def runscript(self, command, stdin="", ok=True):
        p = subprocess.run([str(JASH), "-c", command], cwd=self.cwd,
                           env=self.env, input=stdin, text=True,
                           capture_output=True, timeout=10)
        if ok:
            self.assertEqual(p.returncode, 0, (command, p.stdout, p.stderr))
        else:
            self.assertNotEqual(p.returncode, 0, (command, p.stdout, p.stderr))
        return p

    def put(self, name, data="payload"):
        p = self.cwd / name
        p.write_text(data)
        return p

    def test_cp_multi_source_and_verbose(self):
        self.put("a.c", "a")
        self.put("b.c", "b")
        (self.cwd / "dst").mkdir()
        out = self.runj("cp", "-v", "a.c", "b.c", "dst").stdout
        self.assertEqual((self.cwd / "dst/a.c").read_text(), "a")
        self.assertEqual((self.cwd / "dst/b.c").read_text(), "b")
        self.assertIn("b.c", out)
        (self.cwd / "glob").mkdir()
        self.runscript("cp *.c glob/")
        self.assertEqual((self.cwd / "glob/a.c").read_text(), "a")
        self.assertEqual((self.cwd / "glob/b.c").read_text(), "b")

    def test_cp_noclobber(self):
        self.put("src", "new")
        dst = self.put("dst", "old")
        self.runj("cp", "-n", "src", "dst")
        self.assertEqual(dst.read_text(), "old")

    def test_cp_preserve_file_and_directory_times(self):
        src = self.cwd / "src"
        src.mkdir()
        f = self.put("src/file")
        stamp = 1_600_000_000_123456789
        os.utime(f, ns=(stamp, stamp))
        os.utime(src, ns=(stamp, stamp))
        self.runj("cp", "-a", "src", "dst")
        self.assertEqual((self.cwd / "dst/file").stat().st_mtime_ns, stamp)
        self.assertEqual((self.cwd / "dst").stat().st_mtime_ns, stamp)

    def test_cp_archive_symlinks_and_cycle(self):
        (self.cwd / "src").mkdir()
        self.put("src/file")
        (self.cwd / "src/loop").symlink_to(".")
        (self.cwd / "src/dangling").symlink_to("missing")
        self.runj("cp", "-a", "src", "dst")
        self.assertEqual(os.readlink(self.cwd / "dst/loop"), ".")
        self.assertEqual(os.readlink(self.cwd / "dst/dangling"), "missing")

    def test_cp_rejects_own_subtree(self):
        (self.cwd / "src").mkdir()
        self.put("src/file")
        self.runj("cp", "-r", "src", "src/child", ok=False)
        self.assertFalse((self.cwd / "src/child").exists())

    def test_cp_same_inode_never_truncates(self):
        src = self.put("src")
        (self.cwd / "alias").hardlink_to(src)
        self.runj("cp", "src", "alias", ok=False)
        self.assertEqual(src.read_text(), "payload")

    def test_cp_dedup_djb2_collision(self):
        # Equal length and equal djb2 for any word size; different bytes.
        src = self.put("src", "Ab")
        dst = self.put("dst", "BA")
        self.runj("cp", "--dedup", "--verify", "src", "dst")
        self.assertEqual(dst.read_bytes(), src.read_bytes())
        self.assertNotEqual(dst.stat().st_ino, src.stat().st_ino)

    def test_cp_verify_rejects_corruption_and_read_error(self):
        if not self.hook:
            self.skipTest("fault injection requires Linux LD_PRELOAD")
        self.put("src", "Ab")
        for mode in ("AUDIT_CP_CORRUPT", "AUDIT_CP_READ_ERROR"):
            marker = self.cwd / mode
            self.env.update(LD_PRELOAD=self.hook, AUDIT_CP_TARGET=str(self.cwd / "dst"),
                            AUDIT_CP_MARKER=str(marker))
            self.env[mode] = "1"
            result = self.runj("cp", "--verify", "src", "dst", ok=False)
            self.assertTrue(marker.exists(), "fault injector did not execute")
            self.assertIn("verification FAILED", result.stdout + result.stderr)
            del self.env[mode]

    def test_df_path_type_total_k(self):
        self.put("file")
        data = json.loads(self.runj("df", "--json", "file").stdout)
        self.assertTrue(str(self.cwd).startswith(data["mountpoint"]))
        out = self.runj("df", "-k", "-T", "--total", "file").stdout
        self.assertIn("Type", out)
        self.assertIn("total", out)

    def test_df_all_operands_and_spaces(self):
        self.put("one file")
        self.put("two")
        out = self.runj("df", "--json", "one file", "two").stdout
        self.assertEqual(len([json.loads(s) for s in out.splitlines()]), 2)

    def test_df_missing_absolute_path_is_error(self):
        self.runj("df", "--json", str(self.cwd / "missing"), ok=False)

    def test_df_mixed_valid_invalid_operands(self):
        self.put("valid")
        p = self.runj("df", "--json", "missing", "valid", ok=False)
        self.assertEqual(len([json.loads(s) for s in p.stdout.splitlines()]), 1)

    def test_dispatch_modes_keep_quoted_paths(self):
        # Fast -c, compound evaluator, script and stdin must agree on argv.
        for mode in ("fast", "compound", "script", "stdin"):
            name = mode + " file"
            command = shlex.join(("touch", name))
            args, data = [str(JASH)], None
            if mode == "script":
                script = self.put("input.sh", command + "\n")
                args += [str(script)]
            elif mode == "stdin":
                data = command + "\n"
            else:
                args += ["-c", command + ("; true" if mode == "compound" else "")]
            p = subprocess.run(args, input=data, text=True, capture_output=True,
                               env=self.env, cwd=self.cwd, timeout=10)
            self.assertEqual(p.returncode, 0, (mode, p.stderr))
            self.assertTrue((self.cwd / name).is_file())

    def test_current_backlog_rejects_only_unimplemented_options(self):
        self.put("src")
        self.runj("cp", "-u", "src", "dst")
        self.assertEqual((self.cwd / "dst").read_text(), "payload")
        for args in (("df", "--delta"), ("grep", "--delta", "x", "src"),
                     ("df", "-m"), ("df", "-B", "512"), ("df", "-a"),
                     ("df", "-t", "ext4"), ("df", "-x", "tmpfs"), ("df", "-l")):
            self.runj(*args, ok=False)
        self.assertEqual((self.cwd / "src").read_text(), "payload")
        self.assertEqual((self.cwd / "dst").read_text(), "payload")
        for command in ("whatif", "budget", "gc"):
            # `type` could find an unrelated executable with the same name.
            self.runj("builtin", command, ok=False)

    def test_sense_offline_risk_is_implemented(self):
        self.put("keep")
        data = json.loads(self.runj("explain", "--risk", "--json", "rm keep").stdout)
        self.assertIn("risk", data)
        self.assertTrue((self.cwd / "keep").exists())
        self.assertIn("risk", self.runj("sense", "rm keep").stdout)

    def test_mv_flags_spaces_and_dash(self):
        self.put("-src", "new")
        dst = self.put("dst file", "old")
        self.runj("mv", "-n", "--", "-src", "dst file")
        self.assertEqual(dst.read_text(), "old")
        out = self.runj("mv", "-fv", "--", "-src", "dst file").stdout
        self.assertEqual(dst.read_text(), "new")
        self.assertIn("dst file", out)

    def test_mv_interactive_pipe_and_eof(self):
        src = self.put("src", "new")
        dst = self.put("dst", "old")
        for answer in ("n\n", ""):
            self.runj("mv", "-i", "src", "dst", stdin=answer)
            self.assertEqual(dst.read_text(), "old")
            self.assertTrue(src.exists())
        self.runj("mv", "-i", "src", "dst", stdin="y\n")
        self.assertEqual(dst.read_text(), "new")

    def test_rm_interactive_spaces_dash_and_symlink(self):
        p = self.put("my file")
        self.runj("rm", "-i", "my file", stdin="n\n")
        self.assertTrue(p.exists())
        self.runj("rm", "-i", "my file", stdin="y\n")
        self.assertFalse(p.exists())
        p = self.put("-name")
        self.runj("rm", "--", "-name")
        self.assertFalse(p.exists())
        (self.cwd / "target").mkdir()
        p = self.put("target/keep")
        (self.cwd / "link").symlink_to("target")
        self.runj("rm", "-r", "link")
        self.assertTrue(p.exists())

    def test_rm_once_decline(self):
        for n in "abcd":
            self.put(n)
        self.runj("rm", "-I", *"abcd", stdin="n\n")
        self.assertTrue(all((self.cwd / n).exists() for n in "abcd"))

    def test_forge_reports_scaffold_and_escapes_json(self):
        description = 'test "quoted" \\ data'
        obj = json.loads(self.runj("forge", "--json", description).stdout)
        self.assertEqual(obj["description"], description)
        self.assertEqual(obj["status"], "scaffold")
        self.runj("forge", "--lang", "python", "example", ok=False)

    def test_gossip_local_message_json(self):
        message = 'hello "quoted" \\ message'
        command = shlex.join(("gossip", "send", message))
        out = self.runscript(command + " > /dev/null && gossip receive --json").stdout
        self.assertEqual(json.loads(out)[0]["message"], message)

    def test_mirror_rejects_unimplemented_execution(self):
        self.runj("mirror", "touch", "should-not-exist", ok=False)
        self.assertFalse((self.cwd / "should-not-exist").exists())
        availability = json.loads(self.runj("mirror", "--json").stdout)
        self.assertFalse(availability["available"])
        self.assertFalse(availability["recording"])
        self.assertEqual(availability["entries"], [])
        self.assertEqual(availability["error"], "execution_recording_unavailable")

    def test_touch_quote_no_create_and_dates(self):
        self.runj("touch", "my file.txt")
        self.assertTrue((self.cwd / "my file.txt").is_file())
        self.assertFalse((self.cwd / "my").exists())
        self.runj("touch", "-c", "missing")
        self.assertFalse((self.cwd / "missing").exists())
        self.runj("touch", "-t", "202001020304.05", "stamp")
        self.assertEqual((self.cwd / "stamp").stat().st_mtime, 1577934245)
        self.runj("touch", "-d", "@1600000000", "epoch")
        self.assertEqual((self.cwd / "epoch").stat().st_mtime, 1600000000)

    def test_touch_reference_preserves_both_times(self):
        ref = self.put("ref")
        times = (1_500_000_000_123456789, 1_600_000_000_987654321)
        os.utime(ref, ns=times)
        self.runj("touch", "-r", "ref", "dst")
        st = (self.cwd / "dst").stat()
        self.assertEqual((st.st_atime_ns, st.st_mtime_ns), times)

    def test_touch_rejects_invalid_dates(self):
        for flag, date in (("-d", "@oops"), ("-d", "2020-01-01junk"),
                           ("-t", "202013010000"), ("-t", "202001010000.xx")):
            self.runj("touch", flag, date, "dst", ok=False)
            self.assertFalse((self.cwd / "dst").exists())

    def test_touch_dangling_link_never_creates_target(self):
        (self.cwd / "link").symlink_to("missing")
        self.runj("touch", "-h", "link")
        self.assertFalse((self.cwd / "missing").exists())

    def test_cross_device_mv_and_rm_trash_regular(self):
        shm = Path("/dev/shm")
        if not shm.is_dir() or shm.stat().st_dev == self.cwd.stat().st_dev:
            self.skipTest("requires a writable second filesystem")
        with tempfile.TemporaryDirectory(prefix="jash-audit-", dir=shm) as other:
            p = Path(other) / "move"
            p.write_text("move")
            self.runj("mv", str(p), "moved")
            self.assertFalse(p.exists())
            self.assertEqual((self.cwd / "moved").read_text(), "move")
            p = Path(other) / "remove"
            p.write_text("recover")
            self.runj("rm", "--undo", str(p))
            self.assertFalse(p.exists())
            trash = self.cwd / "home/.jash/trash"
            self.assertTrue(any(f.read_text() == "recover" for f in trash.iterdir()
                                if f.is_file() and not f.name.endswith(".meta")))
            # Outside HOME, explicit caller cwd is the restore authority.
            self.cwd = Path(other)
            self.runj("trash", "restore", "remove")
            self.assertEqual(p.read_text(), "recover")


if __name__ == "__main__":
    unittest.main()
