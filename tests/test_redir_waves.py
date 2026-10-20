#!/usr/bin/env python3
"""Focused redirection lifetime/scanner regressions; does not build the shell."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
SHELL = Path(os.environ.get("JASH", ROOT / "build/jash")).resolve()
IS_BASH = SHELL.name == "bash"


class RedirWaves(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="jash-redir-waves-")
        self.addCleanup(tmp.cleanup)
        self.work = Path(tmp.name)
        self.env = {
            "HOME": str(self.work), "TMPDIR": str(self.work),
            "PATH": "/usr/bin:/bin", "LC_ALL": "C", "TERM": "dumb",
            "JASH_SIGIL": "0", "JASH_CHANGES": "0", "CHANGES": "0",
            "JASH_SAFE": "0", "JASH_BRAIN": "0", "JASH_WORLD": "0",
            "JASH_MEMO": "0", "JASH_BRIEF": "0", "JASH_NUDGE": "0",
            "JASH_LOOPGUARD": "off", "JASH_DRIFT": "0",
            "JASH_RECALL_GUARD": "0", "JASH_RISK_CHECKPOINT": "0",
            "JASH_GUARD": "0", "JASH_GUARD_STRICT": "0",
            "JASH_GUARD_BYPASS": "0", "JASH_GUARD_BYPASS_STRICT": "0",
            "JASH_SKIP_SELFCHECK": "1",
            "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
            "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
        }

    def run_shell(self, code, stdin=b""):
        if IS_BASH:
            code = "shopt -s lastpipe; " + code
        result = subprocess.run([str(SHELL), "-c", code], input=stdin,
                                capture_output=True, cwd=self.work,
                                env=self.env, timeout=10)
        return result.returncode, result.stdout, result.stderr

    def test_nested_exec_preserves_pipeline_stdout(self):
        for fd in (10, 11, 12, 60):
            with self.subTest(fd=fd):
                self.assertEqual(self.run_shell(
                    f'printf X | builtin eval "exec {fd}>/dev/null" >/dev/null; '
                    'printf AFTER'), (0, b"AFTER", b""))

    def test_nested_exec_preserves_pipeline_stdin(self):
        for fd in (5, 10, 11, 12):
            with self.subTest(fd=fd):
                self.assertEqual(self.run_shell(
                    f'printf X | builtin eval "exec {fd}</dev/null"; '
                    'read value; printf "%s" "$value"', b"ORIGINAL\n"),
                    (0, b"ORIGINAL", b""))

    def test_command_not_found_preserves_user_fd(self):
        rc, out, err = self.run_shell(
            'exec 60>saved; missing-redir-wave-command >missing-out; '
            'printf AFTER >&60; exec 60>&-')
        self.assertEqual((rc, out), (0, b""), err)
        self.assertIn(b"command not found", err)
        self.assertEqual((self.work / "saved").read_bytes(), b"AFTER")
        self.assertEqual((self.work / "missing-out").read_bytes(), b"")

    @unittest.skipIf(IS_BASH, "Jash xargs dispatches shell functions in process")
    def test_xargs_function_stderr_is_not_buffered_as_stdout(self):
        self.assertEqual(self.run_shell(
            'g(){ printf BODY; }; f(){ g >&2; }; '
            'xargs -n1 f <<<arg >/dev/stdout'), (0, b"", b"BODY"))

    @unittest.skipIf(IS_BASH, "Jash supports holes created by unset N")
    def test_sparse_caller_positionals_survive_functions(self):
        for redirect in ("", " >/dev/null"):
            for gap in (1, 2):
                with self.subTest(redirect=redirect, gap=gap):
                    self.assertEqual(self.run_shell(
                        f"set -- A '' C; unset {gap}; f(){{ set -- inner; }}; f{redirect}; "
                        "printf '<%s|%s|%s>' \"${1-unset}\" \"${2-unset}\" \"$3\""),
                        (0, b"<unset||C>" if gap == 1 else b"<A|unset|C>", b""))

    def test_failed_write_is_not_success(self):
        if not Path("/dev/full").exists():
            self.skipTest("requires /dev/full")
        for code in ("printf BAD >/dev/full", "echo BAD >/dev/full",
                     "f(){ printf BAD; }; f >/dev/full"):
            with self.subTest(code=code):
                rc, out, err = self.run_shell(code)
                self.assertNotEqual(rc, 0)
                self.assertEqual(out, b"")
                self.assertTrue(err)

    def test_only_word_initial_digits_are_fd_prefixes(self):
        tokens = (
            ("x2", b"x2"), ("x22", b"x22"),
            (r"x\ 2", b"x 2"), ('"x "2', b"x 2"),
            ("'x '22", b"x 22"), ('x" "22', b"x 22"),
            (r"\ 2", b" 2"), ("''2", b"2"),
        )
        for command in ("echo", "/bin/echo"):
            for suffix in ("", " | cat"):
                for token, value in tokens:
                    with self.subTest(command=command, suffix=suffix, token=token):
                        self.assertEqual(self.run_shell(
                            f':; {command} PAYLOAD {token}>output{suffix}; printf AFTER'),
                            (0, b"AFTER", b""))
                        self.assertEqual((self.work / "output").read_bytes(),
                                         b"PAYLOAD " + value + b"\n")
                for fd in (2, 22):
                    with self.subTest(command=command, suffix=suffix, fd=fd):
                        self.assertEqual(self.run_shell(
                            f':; {command} PAYLOAD {fd}>output{suffix}; printf AFTER'),
                            (0, b"PAYLOAD\nAFTER", b""))
                        self.assertEqual((self.work / "output").read_bytes(), b"")


if __name__ == "__main__":
    unittest.main()
