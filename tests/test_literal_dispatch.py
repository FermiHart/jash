#!/usr/bin/env python3
"""Literal builtin dispatch must agree with the full parser and runtime hooks."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
JASH = Path(os.environ.get("JASH", ROOT / "build/jash")).resolve()


class LiteralDispatch(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="jash-literal-dispatch-")
        self.addCleanup(tmp.cleanup)
        self.work = Path(tmp.name)
        self.env = {
            "HOME": tmp.name, "TMPDIR": tmp.name, "PATH": "/usr/bin:/bin",
            "LC_ALL": "C", "TERM": "dumb", "JASH_SIGIL": "0",
            "JASH_TIMELINE": "0", "JASH_CHANGES": "0", "JASH_MEMO": "0",
            "JASH_BRIEF": "0", "JASH_NUDGE": "0", "JASH_WORLD": "0",
            "JASH_LOOPGUARD": "off", "JASH_DRIFT": "0", "JASH_GUARD": "0",
            "JASH_GUARD_BYPASS": "0", "JASH_GUARD_BYPASS_STRICT": "0",
            "JASH_RECALL_GUARD": "0", "JASH_RISK_CHECKPOINT": "0",
            "JASH_SKIP_SELFCHECK": "1", "JASH_LLM_BUDGET": "0",
        }

    def run_shell(self, code, **overrides):
        p = subprocess.run([str(JASH), "-c", code], cwd=self.work,
                           env={**self.env, **overrides}, input=b"",
                           capture_output=True, timeout=20)
        return p.returncode, p.stdout, p.stderr

    def compare_full_parser(self, template, word, expected=None, **env):
        # A quoted command word necessarily declines the literal certificate.
        # Only the command word changes; operands, control flow and hooks stay.
        full = self.run_shell(template.replace("@CMD@", '"' + word + '"'), **env)
        fast = self.run_shell(template.replace("@CMD@", word), **env)
        self.assertEqual(fast, full, template)
        if expected is not None:
            self.assertEqual(fast, expected, template)

    def test_literal_argv_and_legacy_builtins(self):
        for word, operands, expected in (
            ("test", "x = x", b"0\n"), ("test", "x = y", b"1\n"),
            ("test", "-n x", b"0\n"), ("test", "7 -gt 3", b"0\n"),
            ("test", "", b"1\n"), ("true", "", b"0\n"),
            ("false", "", b"1\n"), (":", "x = y", b"0\n"),
            ("echo", "-n plain:/=%+,_-  second", b"plain:/=%+,_- second0\n"),
            ("printf", "%s:%d value 42", b"value:420\n"),
        ):
            with self.subTest(word=word, operands=operands):
                self.compare_full_parser(f":; @CMD@ {operands}; echo $?", word,
                                         (0, expected, b""))
        self.compare_full_parser(":; @CMD@; echo $?", "true", (0, b"0\n", b""))
        self.compare_full_parser(":; @CMD@ A=hello; echo $A", "export",
                                 (0, b"hello\n", b""))
        self.compare_full_parser(":; @CMD@ -- a b; echo $#:$2", "set",
                                 (0, b"2:b\n", b""))
        self.compare_full_parser(":; @CMD@\tx\t=\tx\t; echo $?", "test",
                                 (0, b"0\n", b""))

    def test_function_shadowing_and_command_bypass(self):
        self.compare_full_parser(
            'test(){ echo function:$#; return 7; }; '
            '@CMD@ x = x; echo $?; command test x = x; echo $?; '
            'test(){ return 1; }; @CMD@ x = y; echo $?', "test",
            (0, b"function:3\n7\n0\n1\n", b""))

    def test_alias_changes_inside_loop(self):
        self.compare_full_parser(
            'alias test=true; for ((i=0;i<3;i++)); do @CMD@ x = x; '
            'echo $?; alias test=false; done; unalias test; @CMD@ x = x; echo $?',
            "test", (0, b"0\n1\n1\n0\n", b""))

    def test_enable_disable_uses_current_resolution(self):
        self.compare_full_parser(
            'enable -n test; @CMD@ x = y; echo $?; '
            'enable test; @CMD@ x = x; echo $?', "test", (0, b"1\n0\n", b""))

    def test_debug_err_and_signal_traps(self):
        self.compare_full_parser(
            "trap 'printf D' DEBUG; @CMD@ x = x; @CMD@ x = y; trap - DEBUG",
            "test")
        self.compare_full_parser(
            "trap 'printf E' ERR; @CMD@ x = y; echo done", "test",
            (0, b"Edone\n", b""))
        self.compare_full_parser(
            "trap 'printf S' USR1; /bin/kill -USR1 $$; @CMD@ x = x; echo done", "test",
            (0, b"Sdone\n", b""))
        # DEBUG can change resolution before the certificate is considered.
        self.compare_full_parser(
            "trap 'alias test=printf' DEBUG; @CMD@ marker", "test",
            (0, b"marker", b""))

    def test_control_flow_and_short_circuit(self):
        self.compare_full_parser(
            'set -e; @CMD@ x = y && echo BAD; @CMD@ x = x || echo BAD; '
            'echo OK; @CMD@ x = y; echo BAD', "test", (1, b"OK\n", b""))
        self.compare_full_parser(
            'f(){ @CMD@ 7; echo BAD; }; f; echo $?', "return", (0, b"7\n", b""))
        self.compare_full_parser(
            'for i in a b; do @CMD@; echo BAD; done; echo OK', "break",
            (0, b"OK\n", b""))
        self.compare_full_parser(
            'for i in a b; do @CMD@; echo BAD; done; echo OK', "continue",
            (0, b"OK\n", b""))

    def test_fallback_expansion_quotes_comments_and_operators(self):
        (self.work / "match-one").touch()
        cases = (
            'echo "a b"', 'echo \'\'', 'echo a\\ b', 'echo $v',
            'echo "${missing:-fallback}"', 'echo "$((3+4))"',
            'echo "$(printf sub)"', 'echo `printf sub`', 'echo $\'a\\tb\'',
            'echo match*', 'echo {a,b}', 'echo ~', 'echo x # comment',
            'echo a; echo b', 'echo a && echo b', 'echo a || echo b',
            'echo x >out; /bin/cat out', 'echo x | /bin/cat',
            'echo "a\nb"', 'echo "ç"', 'echo a\\\nb',
        )
        for code in cases:
            with self.subTest(code=code):
                self.compare_full_parser('v=payload; ' + code.replace('echo', '@CMD@'),
                                         "echo")
        self.compare_full_parser(':; @CMD@ "${absent:?expected refusal}"', "echo")

    def test_loopguard_and_output_governor(self):
        self.compare_full_parser('for ((i=0;i<8;i++)); do @CMD@; done', "false",
                                 JASH_LOOPGUARD="on")
        (self.work / "payload").write_bytes(b"x" * 4096)
        # Keep the budget wrapper and its side effects on the shared dispatch.
        for word in ("cat", '"cat"'):
            rc, out, err = self.run_shell(f":; {word} payload", JASH_LLM_BUDGET="512")
            self.assertEqual(rc, 0, err)
            self.assertTrue(out.startswith(b"x" * 512), out[:520])
            self.assertNotEqual(out[512:513], b"x")
            self.assertLess(len(out), 4096)
            self.assertIn(b"output capped", out + err)


if __name__ == "__main__":
    unittest.main()
