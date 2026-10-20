#!/usr/bin/env python3
"""Scoped assert regressions. Default: isolated real-C harness; --binary: shell.

The harness links the real builtin, tokenizer, JSON writer, BLAKE3 and process
supervisor, without touching the shared build directory.
Use --portable to exercise the non-Linux direct-process-group fallback on Linux.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = None
PORTABLE = False


def build_harness(directory, portable=False):
    source = directory / "assert_main.c"
    source.write_text(r'''
#include <builtins/builtin.h>
#include <string.h>
#include <unistd.h>
static long output(void *ctx, const char *p, size_t n) {
    (void)ctx; return write(1, p, n);
}
int main(int argc, char **argv) {
    if (argc != 3 || strcmp(argv[1], "-c") ||
        strncmp(argv[2], "assert", 6)) return 2;
    return jash_b_assert(NULL, output, NULL, argv[2] + 6);
}
''')
    binary = directory / "assert-harness"
    files = [source, ROOT / "c/builtins/builtin_assert.c",
             ROOT / "c/builtins/registry.c", ROOT / "c/core/argv.c",
             ROOT / "c/core/agent_json.c", ROOT / "c/theme/io.c",
             ROOT / "c/core/agent_process.c", ROOT / "c/core/agent_cgroup.c",
             ROOT / "c/core/execution_ir.c",
             ROOT / "c/module/blake3.c", ROOT / "c/parser/cmdsub.c"]
    defines = ["-DJASH_ASSERT_TEST_PORTABLE=1"] if portable else []
    subprocess.run(["cc", "-std=gnu11", "-O1", "-g", *defines, "-ffunction-sections",
                    "-fdata-sections", "-Wl,--gc-sections", "-I", str(ROOT / "c"),
                    *map(str, files), "-o", str(binary)], check=True)
    return binary


class AssertDependencies(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="assert-deps-")
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        self.repo = self.home / "repo 'one"
        self.other = self.home / "repo two"
        for repo in (self.repo, self.other):
            repo.mkdir()
            (repo / ".git").mkdir()
        self.env = dict(os.environ, HOME=str(self.home), JASH_SIGIL="0",
                        JASH_NUDGE="0", JASH_BRIEF="0", JASH_LOOPGUARD="off",
                        JASH_AGENT="0")

    def run_assert(self, *args, cwd=None, rc=None):
        result = subprocess.run([str(BINARY), "-c", "assert " + shlex.join(args)],
                                cwd=cwd or self.repo, env=self.env, text=True,
                                capture_output=True, timeout=12)
        if rc is not None:
            self.assertEqual(result.returncode, rc, result.stdout + result.stderr)
        return result

    def rows(self, *args, cwd=None, rc=None):
        result = self.run_assert(*args, "--json", cwd=cwd, rc=rc)
        return json.loads(result.stdout.strip().splitlines()[-1])["assertions"]

    def test_two_repos_and_recorded_cwd(self):
        sub = self.repo / "sub space"
        sub.mkdir()
        (sub / "input").write_text("yes")
        self.run_assert("local check", "--check", "test -f input", cwd=sub, rc=0)
        self.assertEqual(self.rows(cwd=self.other, rc=0), [])
        row, = self.rows(rc=0)
        self.assertEqual(row["cwd"], str(sub))
        self.assertEqual(row["scope"], str(self.repo))
        self.assertEqual(row["state"], "vale")
        self.run_assert("--forget", "local", cwd=self.other, rc=0)
        self.assertEqual(len(self.rows(rc=0)), 1)

    def test_status_does_not_execute_and_hashes_contents(self):
        dep = self.repo / "input"
        dep.write_text("one")
        self.run_assert("contents", "--dep", "input", "--check",
                        "printf x >> runs", rc=0)
        store = self.home / ".jash/assertions.tsv"
        saved = store.read_bytes()
        self.assertEqual(self.rows("--status", rc=0)[0]["state"], "vale")
        stamp = dep.stat()
        dep.write_text("two")
        os.utime(dep, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        row, = self.rows("--status", rc=1)
        self.assertEqual(row["state"], "invalidated")
        self.assertEqual((self.repo / "runs").read_text(), "x")
        self.assertEqual(store.read_bytes(), saved)
        self.assertEqual(self.rows(rc=0)[0]["state"], "vale")
        self.assertEqual((self.repo / "runs").read_text(), "xx")

    def test_missing_dep_and_change_during_verifier_never_green(self):
        self.run_assert("missing", "--dep", "absent", "--check", "true", rc=1)
        self.assertNotEqual(self.rows("--status", rc=1)[0]["state"], "vale")
        self.run_assert("--forget", "missing", rc=0)
        (self.repo / "dep").write_text("old")
        self.run_assert("racy", "--dep", "dep", "--check", "printf new > dep", rc=1)
        self.assertEqual(self.rows("--status", rc=1)[0]["state"], "invalidated")

    def test_repeated_deps_quote_paths_and_persistence(self):
        paths = ["one 'quoted'", 'two "quoted"\tline\nend']
        for name in paths:
            (self.repo / name).write_text("ok")
        claim = 'quoted "claim"\twith\nnewline'
        check = "test -f " + shlex.quote(paths[0])
        self.run_assert(claim, "--check", check, "--dep", paths[0],
                        "--dep", paths[1], rc=0)
        row, = self.rows("--status", rc=0)
        self.assertEqual((row["claim"], row["check"]), (claim, check))
        self.assertEqual(len(row["dependencies"]), 2)
        (self.repo / paths[1]).unlink()
        self.assertNotEqual(self.rows("--status", rc=1)[0]["state"], "vale")

    def test_timeout_is_persisted_unknown_and_cancels_descendants(self):
        start = time.monotonic()
        self.run_assert("slow", "--timeout-ms", "60", "--check",
                        "(sleep 1; touch escaped) & sleep 30", rc=1)
        self.assertLess(time.monotonic() - start, 4)
        row, = self.rows("--status", rc=1)
        self.assertEqual(row["state"], "unknown")
        self.assertEqual(row["reason"], "timeout")
        self.assertEqual(row["timeout_ms"], 60)
        time.sleep(1.1)
        self.assertFalse((self.repo / "escaped").exists())

    def test_legacy_is_preserved_unscoped_and_never_executed(self):
        store = self.home / ".jash"
        store.mkdir()
        legacy = "old\ttouch legacy-ran\t1\t1\t1\n"
        (store / "assertions.tsv").write_text(legacy)
        self.assertEqual(self.rows("--status", rc=0), [])
        self.run_assert("new", "--check", "true", rc=0)
        self.assertEqual(len(self.rows(rc=0)), 1)
        self.assertIn(legacy, (store / "assertions.tsv").read_text())
        self.assertFalse((self.repo / "legacy-ran").exists())

    def test_never_success_failure_has_nonzero_list_status(self):
        self.run_assert("never", "--check", "false", rc=1)
        self.assertEqual(self.rows(rc=1)[0]["state"], "nunca")
        self.assertEqual(self.rows("--status", rc=1)[0]["state"], "nunca")
        (self.repo / "dep").write_text("unchanged")
        self.run_assert("never with dep", "--dep", "dep", "--check", "false", rc=1)
        self.assertEqual([r["state"] for r in self.rows("--status", rc=1)],
                         ["nunca", "nunca"])

    def test_success_without_declared_dependencies_is_unknown_on_status(self):
        self.run_assert("dep is prose only", "--check", "printf x >> runs", rc=0)
        row, = self.rows("--status", rc=1)
        self.assertEqual((row["state"], row["reason"], row["dependencies"]),
                         ("unknown", "no_dependencies", []))
        self.assertEqual((self.repo / "runs").read_text(), "x")

    def test_git_file_and_nonrepo_fallback_scope(self):
        (self.other / ".git").rmdir()
        (self.other / ".git").write_text("gitdir: /not/consulted")
        sub = self.other / "sub"
        sub.mkdir()
        self.run_assert("worktree", "--check", "true", cwd=sub, rc=0)
        self.assertEqual(self.rows(cwd=self.other, rc=0)[0]["scope"], str(self.other))
        plain = self.home / "plain"
        plain.mkdir()
        child = plain / "child"
        child.mkdir()
        self.run_assert("plain", "--check", "true", cwd=plain, rc=0)
        self.assertEqual(self.rows(cwd=child, rc=0), [])
        self.assertEqual(self.rows(cwd=plain, rc=0)[0]["scope"], str(plain))

    def test_store_failure_after_verifier_never_prints_success(self):
        result = self.run_assert("write failure", "--check",
                                 'mkdir "$HOME/.jash/assertions.tsv"', rc=2)
        self.assertNotIn("\u2713", result.stdout)
        self.assertIn("persist", result.stderr)

    def test_nonregular_dependency_is_rejected_without_blocking(self):
        os.mkfifo(self.repo / "fifo")
        start = time.monotonic()
        self.run_assert("fifo", "--dep", "fifo", "--check", "touch ran", rc=1)
        self.assertLess(time.monotonic() - start, 2)
        self.assertFalse((self.repo / "ran").exists())

    def test_shipped_gate_unchanged(self):
        script = ROOT / "tests/test_jinv055_assert.sh"
        # Only override the gate's fixed build path in memory, not its tests.
        text = script.read_text().replace('JASH="$PWD/build/jash"',
                                           "JASH=" + shlex.quote(str(BINARY)))
        result = subprocess.run(["bash", "-c", text, str(script)], cwd=ROOT,
                                env=self.env, capture_output=True, text=True,
                                timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("10 ok / 0 fail", result.stdout)

    def test_bad_storage_and_overlong_inputs_do_not_execute(self):
        store = self.home / ".jash"
        store.mkdir()
        path = store / "assertions.tsv"
        path.write_text("corrupt row\n")
        self.run_assert("bad", "--check", "touch ran", rc=2)
        self.assertFalse((self.repo / "ran").exists())
        self.assertEqual(path.read_text(), "corrupt row\n")
        path.unlink()
        path.mkdir()
        self.run_assert("bad", "--check", "touch ran", rc=2)
        path.rmdir()
        for args in [("x" * 256, "--check", "touch ran"),
                     ("bad", "--check", "true " + "x" * 512),
                     ("bad", "--dep", "x" * 4096, "--check", "touch ran"),
                     ("bad", "--check", "touch ran", "--timeout-ms", "0")]:
            self.run_assert(*args, rc=2)
        self.assertFalse((self.repo / "ran").exists())

    def test_lock_contention_is_bounded(self):
        self.run_assert("initial", "--check", "true", rc=0)
        with (self.home / ".jash/assertions.lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            start = time.monotonic()
            self.run_assert("busy", "--check", "touch ran", rc=2)
            self.assertLess(time.monotonic() - start, 2)
        self.assertFalse((self.repo / "ran").exists())

    def test_active_verifier_rejects_concurrent_writer_without_lost_rows(self):
        command = "assert " + shlex.join(["slow writer", "--check", "touch started; sleep 1"])
        proc = subprocess.Popen([str(BINARY), "-c", command], cwd=self.repo,
                                env=self.env, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.monotonic() + 5
            while not (self.repo / "started").exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue((self.repo / "started").exists())
            self.run_assert("concurrent", "--check", "touch raced", rc=2)
            stdout, stderr = proc.communicate(timeout=8)
            self.assertEqual(proc.returncode, 0, stdout + stderr)
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.communicate()
        self.assertFalse((self.repo / "raced").exists())
        lock = self.home / ".jash/assertions.lock"
        inode = lock.stat().st_ino
        self.run_assert("later writer", "--check", "true", rc=0)
        self.assertEqual(lock.stat().st_ino, inode)
        self.assertEqual([r["claim"] for r in self.rows(rc=0)],
                         ["slow writer", "later writer"])

    def test_corrupt_versioned_storage_is_not_overwritten(self):
        self.run_assert("initial", "--check", "true", rc=0)
        path = self.home / ".jash/assertions.tsv"
        valid = path.read_bytes()
        for corrupt in (valid.replace(b"v2\t", b"v999\t", 1),
                        valid[:-1], valid + b"\x00\n",
                        valid.replace(b"\tblake3\t0", b"\tblake3\t999", 1)):
            with self.subTest(corrupt=corrupt[:30]):
                path.write_bytes(corrupt)
                self.run_assert("bad", "--check", "touch ran", rc=2)
                self.assertEqual(path.read_bytes(), corrupt)
                self.assertFalse((self.repo / "ran").exists())

    def test_verifier_output_cannot_corrupt_json_or_overflow_spool(self):
        self.run_assert("quiet", "--check", "echo output; echo error >&2", rc=0)
        self.assertEqual(self.rows(rc=0)[0]["state"], "vale")
        start = time.monotonic()
        result = self.run_assert("large", "--timeout-ms", "500", "--check",
                                 "yes output")
        self.assertLess(time.monotonic() - start, 4)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        rows = self.rows("--status", rc=1)
        self.assertNotEqual(rows[-1]["state"], "vale")
        if not PORTABLE:
            self.assertEqual(rows[-1]["reason"], "output_limit")

    def test_verifier_can_write_artifact_larger_than_output_budget(self):
        result = self.run_assert("build artifact", "--check",
                                 "dd if=/dev/zero of=artifact bs=1048576 count=2", rc=0)
        self.assertEqual((self.repo / "artifact").stat().st_size, 2 * 1024 * 1024,
                         result.stdout + result.stderr)

    def test_closed_output_does_not_bypass_timeout(self):
        start = time.monotonic()
        self.run_assert("closed output", "--timeout-ms", "100", "--check",
                        "exec 1>&- 2>&-; sleep 30", rc=1)
        self.assertLess(time.monotonic() - start, 4)
        row, = self.rows("--status", rc=1)
        self.assertEqual((row["state"], row["reason"]), ("unknown", "timeout"))

    def test_term_resistant_group_is_killed(self):
        start = time.monotonic()
        self.run_assert("resistant", "--timeout-ms", "300", "--check",
                        "trap '' TERM; (sleep 1; touch escaped) & wait", rc=1)
        self.assertLess(time.monotonic() - start, 4)
        row, = self.rows("--status", rc=1)
        self.assertEqual((row["state"], row["reason"]), ("unknown", "timeout"))
        time.sleep(1.1)
        self.assertFalse((self.repo / "escaped").exists())

    def test_successful_leader_with_live_group_never_passes(self):
        start = time.monotonic()
        self.run_assert("unfinished group", "--check",
                        "(sleep 1; touch escaped) & exit 0", rc=1)
        self.assertLess(time.monotonic() - start, 4)
        row, = self.rows("--status", rc=1)
        self.assertEqual(row["state"], "unknown")
        time.sleep(1.1)
        self.assertFalse((self.repo / "escaped").exists())

    def test_verifier_does_not_inherit_extra_descriptors(self):
        with (self.home / "private").open("w") as private:
            fd = fcntl.fcntl(private.fileno(), fcntl.F_DUPFD, 100)
            try:
                command = "assert " + shlex.join([
                    "private fd", "--check", f"test ! -e /dev/fd/{fd}"])
                result = subprocess.run([str(BINARY), "-c", command], cwd=self.repo,
                                        env=self.env, pass_fds=(fd,), capture_output=True,
                                        text=True, timeout=12)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            finally:
                os.close(fd)

    def test_portable_success_exposes_limited_supervision(self):
        # Exercise the real fallback on Linux even when the other cases use
        # the shipped binary. This is a local C control, not another-OS claim.
        global BINARY
        previous = BINARY
        with tempfile.TemporaryDirectory(prefix="assert-portable-", dir=self.home) as build:
            try:
                if not PORTABLE:
                    BINARY = build_harness(Path(build), portable=True)
                self.run_assert("portable", "--check", "true", rc=0)
                row, = self.rows(rc=0)
                self.assertEqual(row["state"], "vale")
                self.assertEqual(row["reason"], "verified_direct_process_group_only")
            finally:
                BINARY = previous


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--portable", action="store_true")
    args, remaining = parser.parse_known_args()
    if args.binary and args.portable:
        parser.error("--portable selects a compiled harness, not --binary")
    PORTABLE = args.portable
    with tempfile.TemporaryDirectory(prefix="assert-build-") as build:
        BINARY = args.binary.resolve() if args.binary else build_harness(Path(build), PORTABLE)
        unittest.main(argv=[__file__, *remaining])
