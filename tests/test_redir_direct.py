#!/usr/bin/env python3
"""Output-redirection parity and deterministic descriptor-work regression tests."""

import os
from pathlib import Path
import shutil
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
JASH = Path(os.environ.get("JASH", ROOT / "build/jash")).resolve()


class DirectOutput(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="jash-redir-direct-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)

    def run_shell(self, code, *, bash=False, timeline="0", trace=False):
        root = self.root / str(len(list(self.root.iterdir())))
        root.mkdir()
        home, work = root / "home", root / "work"
        home.mkdir()
        work.mkdir()
        (home / ".jashrc").touch()
        env = {"HOME": str(home), "TMPDIR": str(home), "PATH": "/usr/bin:/bin",
               "LC_ALL": "C", "JASH_SIGIL": "0", "JASH_TIMELINE": timeline,
               "JASH_CHANGES": "0", "JASH_MEMO": "0", "JASH_BRIEF": "0",
               "JASH_NUDGE": "0", "JASH_LOOPGUARD": "off", "JASH_SKIP_SELFCHECK": "1",
               "JASH_GUARD": "0", "JASH_GUARD_BYPASS": "0", "JASH_GUARD_BYPASS_STRICT": "0",
               "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1", "UBSAN_OPTIONS": "halt_on_error=1"}
        argv = (["/bin/bash", "--noprofile", "--norc"] if bash else [str(JASH)]) + ["-c", code]
        if trace:
            argv = ["strace", "-f", "-e", "trace=dup2,dup3,fcntl", *argv]
        proc = subprocess.run(argv, env=env, cwd=work, input=b"", capture_output=True,
                              timeout=15, umask=0o022)
        files = {p.name: (p.stat().st_mode & 0o777, p.read_bytes()) for p in work.iterdir() if p.is_file()}
        self.assertNotIn(b"AddressSanitizer", proc.stderr)
        self.assertNotIn(b"runtime error:", proc.stderr)
        return proc, files

    def test_output_forms(self):
        cases = [
            "printf '%s' 'hello world' >output; printf AFTER",
            "printf old >output; printf new >>output; cat output",
            "umask 077; printf secret >output; printf end",
            "set -C; printf first >output; printf forced >|output; cat output",
            "p='quoted name'; echo 'a b' >\"$p\"; echo -n c >>\"$p\"",
            "printf '%b' 'A\\0B' >output; printf end",
            "printf -v value '%s' changed >output; printf '%s' \"$value\"",
            "printf -v value '%s' changed >|output; printf '%s' \"$value\"",
            "printf 'line\\n' >input; read value <input >|output; printf '%s' \"$value\"",
            "printf '%s:%d' 'data' 12 1>output; printf AFTER",
            "x=$(printf hidden >output; printf visible); printf '<%s>' \"$x\"",
            "f(){ printf '%s' \"$1\" >output; printf after; }; f payload",
            "printf old >output; ln output alias; printf new >alias; cat output",
            "printf x >output; ln -s output link; echo y >link; cat output",
            "mkfifo pipe; cat pipe >output & printf data >pipe; wait; cat output",
        ]
        for code in cases:
            with self.subTest(code=code):
                reference, files = self.run_shell(code, bash=True)
                actual, actual_files = self.run_shell(code)
                self.assertEqual((actual.returncode, actual.stdout, actual_files),
                                 (reference.returncode, reference.stdout, files))
                self.assertEqual(actual.stderr, b"")

    def test_fd_path_keeps_original_stderr(self):
        if not Path("/dev/fd/2").exists():
            self.skipTest("requires fd paths")
        actual, files = self.run_shell("printf ERR >/dev/fd/2; printf OUT")
        self.assertEqual((actual.returncode, actual.stdout, actual.stderr, files),
                         (0, b"OUT", b"ERR", {}))

    def test_large_payload(self):
        for size in (1023, 8191, 8192, 12288):
            with self.subTest(size=size):
                code = "p='" + "x" * size + "'; printf '%s' \"$p\" >output; echo AFTER"
                result, files = self.run_shell(code)
                self.assertEqual((result.returncode, result.stdout, result.stderr), (0, b"AFTER\n", b""))
                self.assertEqual(files["output"], (0o644, b"x" * size))
        code = "printf '%s:%s' '" + "a" * 700 + "' '" + "b" * 5000 + "' >output"
        result, files = self.run_shell(code)
        self.assertEqual((result.returncode, result.stdout, result.stderr), (0, b"", b""))
        self.assertEqual(files["output"][1], b"a" * 700 + b":" + b"b" * 5000)

    def test_expanded_filename_markers_are_restored(self):
        for name in ("O'Reilly.txt", 'a"b', "a>b", "a<b", "a|b", "a&b", "a;b",
                     "a\\b", "$name", "`name`", "two words"):
            for op in (">", ">>", ">|"):
                with self.subTest(name=name, op=op):
                    code = f"target={shlex.quote(name)}; printf PAYLOAD {op}\"$target\""
                    result, files = self.run_shell(code)
                    self.assertEqual((result.returncode, result.stdout, result.stderr), (0, b"", b""))
                    self.assertEqual(files, {name: (0o644, b"PAYLOAD")})

    def test_fallback_descriptor_ordering(self):
        for code in (
            "f(){ printf OUT; printf ERR >&2; }; f >output 2>&1; printf AFTER",
            "f(){ printf OUT; printf ERR >&2; }; f 2>&1 >output; printf AFTER",
            "builtin eval 'printf nested >&2' >output; printf AFTER",
            "exec 3>output; printf payload >&3; exec 3>&-; printf AFTER",
            "printf x >output; printf unchanged <output >second; printf AFTER",
            "printf x >output; printf BAD >first <missing; printf AFTER",
            "printf '%s' \\> | cat >output; printf AFTER",
            "printf '%s' '>' | cat >output; printf AFTER",
            "printf '%s' 'value' >|output | cat; printf AFTER",
        ):
            with self.subTest(code=code):
                reference, files = self.run_shell(code, bash=True)
                actual, actual_files = self.run_shell(code)
                self.assertEqual((actual.returncode, actual.stdout, actual_files),
                                 (reference.returncode, reference.stdout, files))

    def test_open_and_write_errors(self):
        for code in ("printf BAD >missing/output", "echo BAD >missing/output",
                     "set -C; printf kept >output; printf BAD >output"):
            with self.subTest(code=code):
                result, files = self.run_shell(code)
                self.assertEqual((result.returncode, result.stdout), (1, b""))
                self.assertTrue(result.stderr)
                if "output" in files:
                    self.assertEqual(files["output"][1], b"kept")
        if Path("/dev/full").exists():
            for builtin in ("printf", "echo"):
                result, _ = self.run_shell(f"{builtin} BAD >/dev/full")
                self.assertEqual((result.returncode, result.stdout), (1, b""))
                self.assertTrue(result.stderr)

    def test_closed_standard_descriptors(self):
        for close in ("0<&-", "1>&-", "2>&-", "0<&- 2>&-"):
            with self.subTest(close=close):
                code = f"exec {close}; printf '%s' value >output"
                actual, files = self.run_shell(code)
                self.assertEqual((actual.returncode, actual.stdout, actual.stderr), (0, b"", b""))
                self.assertEqual(files["output"][1], b"value")
        actual, files = self.run_shell("exec 2>&-; printf -- >output")
        self.assertNotEqual(actual.returncode, 0)
        self.assertEqual((actual.stdout, actual.stderr, files["output"][1]), (b"", b"", b""))

    def test_undo_still_restores_preimage(self):
        for op in (">", ">>", ">|"):
            with self.subTest(op=op):
                result, files = self.run_shell(
                    f"printf old >output; printf new {op}output; undo >/dev/null; cat output",
                    timeline="1")
                self.assertEqual((result.returncode, result.stdout), (0, b"old"), result.stderr)
                self.assertEqual(files["output"][1], b"old")

    @unittest.skipUnless(shutil.which("strace"), "requires strace")
    def test_simple_output_does_not_swap_stdout(self):
        for builtin in ("printf", "echo"):
            for op in (">", ">>", ">|"):
                with self.subTest(builtin=builtin, op=op):
                    result, _ = self.run_shell(
                        f"for ((i=0;i<8;i++)); do {builtin} value {op}output; done", trace=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertNotIn(b"dup2(", result.stderr)
                    self.assertNotIn(b"F_DUPFD", result.stderr)


if __name__ == "__main__":
    unittest.main()
