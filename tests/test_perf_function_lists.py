#!/usr/bin/env python3
"""Function argument/list scanner regressions; never builds or benchmarks."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
SHELL = Path(os.environ.get("JASH", ROOT / "build/jash")).resolve()
IS_BASH = SHELL.name == "bash"


class FunctionLists(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="jash-function-lists-")
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
            "JASH_GUARD": "0", "JASH_SKIP_SELFCHECK": "1",
            "JASH_LLM_BUDGET": "0",
            "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
            "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
        }

    def run_shell(self, code):
        result = subprocess.run([str(SHELL), "-c", code], input=b"",
                                capture_output=True, cwd=self.work,
                                env=self.env, timeout=15)
        return result.returncode, result.stdout, result.stderr

    def test_literal_quoted_empty_and_escaped_arguments(self):
        self.assertEqual(self.run_shell(
            'f(){ printf "%s|" "$#"; printf "<%s>" "$@"; }; '
            'f plain "" \'\' "a b" a\\ b "q\\\"r" \'s"t\' a""b'),
            (0, b'8|<plain><><><a b><a b><q"r><s"t><ab>', b""))

    def test_expanded_field_boundaries_and_empty_positionals(self):
        self.assertEqual(self.run_shell(
            'f(){ printf "%s|" "$#"; printf "<%s>" "$@"; }; '
            'set -- "" "a b" "" c; f "$@"; '
            'v="a b"; f $v "$v" ""; f $(printf "x y")'),
            (0, b"4|<><a b><><c>4|<a><b><a b><>2|<x><y>", b""))

    def test_nested_calls_restore_caller_arguments(self):
        self.assertEqual(self.run_shell(
            'set -- outer "" tail; '
            'g(){ printf "G<%s|%s>" "$1" "$2"; set -- changed; }; '
            'f(){ g "$2" "$1"; printf "F<%s|%s>" "$1" "$2"; }; '
            'f "" "a b"; printf "O<%s|%s|%s>" "$1" "$2" "$3"'),
            (0, b"G<a b|>F<|a b>O<outer||tail>", b""))

    @unittest.skipIf(IS_BASH, "Jash supports sparse positionals via unset N")
    def test_sparse_positionals_survive_repeated_calls(self):
        for gap, expected in ((1, b"<unset||C>"), (2, b"<A|unset|C>")):
            with self.subTest(gap=gap):
                self.assertEqual(self.run_shell(
                    f"set -- A '' C; unset {gap}; "
                    "g(){ set -- inner ''; }; f(){ g x; }; "
                    + "f; f >/dev/null; " * 40
                    + 'printf "<%s|%s|%s>" "${1-unset}" "${2-unset}" "$3"'),
                    (0, expected, b""))

    @unittest.skipIf(IS_BASH, "Jash caps function positionals at 384")
    def test_argument_limit_refuses_without_running_function(self):
        for word in ("x", '""'):
            with self.subTest(word=word):
                self.assertEqual(self.run_shell(
                    'f(){ printf "%s" "$#"; }; f ' + " ".join([word] * 384)),
                    (0, b"384", b""))
                rc, out, err = self.run_shell(
                    'set -- outer; f(){ printf BAD; }; f '
                    + " ".join([word] * 385)
                    + '; printf "<%s|%s>" "$?" "$1"')
                self.assertEqual((rc, out), (0, b"<1|outer>"), err)
                self.assertIn(b"more than 384 positional parameters", err)

    def test_long_newline_free_lists_and_short_circuiting(self):
        for operator, command in ((";", ":"), ("&&", "true"), ("||", "false")):
            with self.subTest(operator=operator):
                code = (command + " " + operator + " ") * 1200 + "printf END"
                self.assertEqual(self.run_shell(code), (0, b"END", b""))
        self.assertEqual(self.run_shell(
            "false && " + "printf BAD && " * 300
            + "printf BAD || printf GOOD; "
            + "true || " + "printf BAD || " * 300 + "printf BAD; printf END"),
            (0, b"GOODEND", b""))

    def test_short_circuit_does_not_expand_skipped_commands(self):
        self.assertEqual(self.run_shell(
            'false && printf "%s" "$(printf BAD >&2)" || printf A; '
            'true || printf "%s" "${missing:?BAD}"; printf B'),
            (0, b"AB", b""))

    def test_multiline_quotes_comments_and_continuations(self):
        cases = (
            ("printf 'a\nb'; printf C; printf D", b"a\nbCD"),
            ('printf "a\nb"; printf C; printf D', b"a\nbCD"),
            ('printf A; # ) ; && || " ignored\nprintf B; printf C', b"ABC"),
            ('printf "%s" "$(printf A # ) ; ignored\n)"; printf B', b"AB"),
            ("printf A; \\\nprintf B; printf C", b"ABC"),
            ('printf "%s" "$(printf "a;b")"; printf C', b"a;bC"),
        )
        for code, expected in cases:
            with self.subTest(code=code):
                self.assertEqual(self.run_shell(code), (0, expected, b""))

    def test_heredoc_keeps_body_separators_inert(self):
        for delimiter in ("END", "'END'", '"END"'):
            with self.subTest(delimiter=delimiter):
                self.assertEqual(self.run_shell(
                    f"cat <<{delimiter}\nbody; && || # text\nEND\n"
                    "printf A; printf B; printf C"),
                    (0, b"body; && || # text\nABC", b""))

    def test_exit_return_and_loop_control_stop_remaining_segments(self):
        cases = (
            ("printf A; exit 7; printf BAD", 7, b"A"),
            ("f(){ printf A; true && return 9; printf BAD; }; f; printf '<%s>' $?",
             0, b"A<9>"),
            ("for i in 1 2; do printf A; true && break; printf BAD; done; printf B",
             0, b"AB"),
            ("for i in 1 2; do printf A; true && continue; printf BAD; done; printf B",
             0, b"AAB"),
            ("set -e; printf A; false; printf BAD", 1, b"A"),
        )
        for code, rc, expected in cases:
            with self.subTest(code=code):
                self.assertEqual(self.run_shell(code), (rc, expected, b""))


if __name__ == "__main__":
    unittest.main()
