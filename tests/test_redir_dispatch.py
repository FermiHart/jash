#!/usr/bin/env python3
"""Evaluator redirection dispatch parity gates; never builds or benchmarks.

JASH selects the candidate; JASH_BASELINE selects the frozen pre-change shell.
"""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SHELL = Path(os.environ.get("JASH", ROOT / "build/jash")).resolve()
BASELINE = Path(os.environ["JASH_BASELINE"]).resolve() if os.environ.get("JASH_BASELINE") else None


class RedirDispatch(unittest.TestCase):
    def run_shell(self, shell, code, files, variables):
        with tempfile.TemporaryDirectory(prefix="jash-redir-dispatch-") as tmp:
            work = Path(tmp)
            (work / "input").write_bytes(b"INPUT\n")
            (work / "match-a").touch()
            (work / "match-b").touch()
            env = {
                "HOME": tmp, "TMPDIR": tmp, "PATH": "/usr/bin:/bin",
                "LC_ALL": "C", "TERM": "dumb", "JASH_SIGIL": "0",
                "JASH_CHANGES": "0", "CHANGES": "0", "JASH_SAFE": "0",
                "JASH_BRAIN": "0", "JASH_WORLD": "0", "JASH_MEMO": "0",
                "JASH_BRIEF": "0", "JASH_NUDGE": "0", "JASH_LOOPGUARD": "off",
                "JASH_DRIFT": "0", "JASH_RECALL_GUARD": "0",
                "JASH_RISK_CHECKPOINT": "0", "JASH_GUARD": "0",
                "JASH_GUARD_STRICT": "0", "JASH_GUARD_BYPASS": "0",
                "JASH_GUARD_BYPASS_STRICT": "0", "JASH_SKIP_SELFCHECK": "1",
                "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
                "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
                **variables,
            }
            command = [str(shell), "-c", code]
            if len(code.encode()) >= 128 * 1024:
                (work / "case.sh").write_text(code + "\n")
                command = [str(shell), "case.sh"]
            # The pre-existing large-script scanner is substantially slower
            # under sanitizers; keep a bounded deadline without timing-gating it.
            timeout = 120 if len(code.encode()) >= 128 * 1024 else 15
            result = subprocess.run(command, cwd=work,
                                    env=env, capture_output=True, timeout=timeout)
            # HOME differs per invocation; only normalize that temporary prefix.
            out = result.stdout.replace(os.fsencode(tmp), b"<WORK>")
            err = result.stderr.replace(os.fsencode(tmp), b"<WORK>")
            saved = {name: ((work / name).read_bytes().replace(
                os.fsencode(tmp), b"<WORK>") if (work / name).is_file() else None)
                     for name in files}
            return result.returncode, out, err, saved

    def compare(self, code, files=("output",), variables=None, bash=True):
        variables = variables or {}
        if not BASELINE and not bash:
            self.skipTest("Jash-specific differential case requires JASH_BASELINE")
        candidate = self.run_shell(SHELL, code, files, variables)
        self.assertGreaterEqual(candidate[0], 0, "shell must not die from a signal")
        self.assertNotIn(b"AddressSanitizer", candidate[2])
        self.assertNotIn(b"runtime error:", candidate[2])
        if BASELINE:
            baseline = self.run_shell(BASELINE, code, files, variables)
            self.assertEqual(candidate, baseline, "candidate differs from frozen baseline")
        if bash:
            reference = self.run_shell(Path("/bin/bash"), code, files, variables)
            self.assertEqual((candidate[0], candidate[1], candidate[3]),
                             (reference[0], reference[1], reference[3]),
                             "status/stdout/files differ from Bash")
        return candidate

    def test_repeated_expanded_payload(self):
        result = self.compare(
            'p=' + 'x' * 4096 + '; '
            'for i in 1 2 3; do printf \'%s\' "$p" > output; done; '
            '/bin/cat output; printf AFTER')
        self.assertEqual(result, (0, b"x" * 4096 + b"AFTER", b"",
                                  {"output": b"x" * 4096}))
        self.compare('echo "$p" > output; echo "$p" >> output; /bin/cat output',
                     variables={"p": "two words "})

    def test_expanded_command_word_not_original_prefix(self):
        for code, variables in (
            ('$cmd "%s" "$p" > output; /bin/cat output',
             {"cmd": "printf", "p": "expanded"}),
            ('$cmd "$p" > output; /bin/cat output',
             {"cmd": "echo", "p": "expanded"}),
            ('printf$suffix "%s" "$p" > output; /bin/cat output',
             {"suffix": "", "p": "expanded"}),
            ('printf$suffix "%s" "$p" > output',
             {"suffix": "-not-a-command", "p": "not native"}),
            ('"$cmd" "%s" "$p" > output; /bin/cat output',
             {"cmd": "printf", "p": "quoted name"}),
        ):
            with self.subTest(code=code):
                self.compare(code, variables=variables)

    def test_quoted_operators_are_data_without_redirects(self):
        for arg in ('">output"', "'<output'", r'\>output', r'"a\">output"',
                    "'|'", "'&'", "';'", "'('", "')'"):
            for command in ("printf '%s'", "echo"):
                with self.subTest(command=command, arg=arg):
                    result = self.compare(f':; {command} {arg}')
                    self.assertIsNone(result[3]["output"])

    def test_quote_paths_and_argument_boundaries(self):
        for name in ("two words", "a>b", "a<b", 'a"b', "a'b", "a\\b", "trailing "):
            for command in ("printf '%s'", "echo"):
                with self.subTest(command=command, path=name):
                    self.compare(f':; {command} "a > b" > {shlex.quote(name)}; '
                                 f'/bin/cat {shlex.quote(name)}', files=(name,))
        for code in (r':; printf "%s" value > "a\"b"',
                     r':; echo value > a\ b',
                     'target="two words"; printf "%s" value > "$target"'):
            with self.subTest(code=code):
                self.compare(code, files=('a"b', "a b", "two words"))

    def test_word_expansion_fallbacks(self):
        for argument in ("match-*", "match-?", "match-[ab]", "{one,two}",
                         "{1..3}", "~", "@(match-a|match-b)",
                         "'match-*'", "'${literal}'", "'`literal`'"):
            with self.subTest(argument=argument):
                self.compare(f':; printf "%s," {argument} > output; /bin/cat output',
                             bash=not argument.startswith("@("))
        self.compare('shopt -s nullglob; printf "%s" missing-* > output; /bin/cat output')
        self.compare('set -f; printf "%s" match-* > output; /bin/cat output')

    def test_protected_expanded_data_falls_back(self):
        values = ("$HOME", "`printf BAD`", 'a"b', "a'b", "a\\b", "*?[]{}~()",
                  ">output", "<input", "a|b&c;d", "a\nb", "a\tb", "caf\u00e9")
        for value in values:
            with self.subTest(value=value):
                self.compare('printf "%s" "$p" > output; /bin/cat output',
                             variables={"p": value})
        self.compare('printf "%s" "$(printf \'%s\' \'$HOME > literal\')" '
                     '> output; /bin/cat output')
        self.compare('set -- "one two" "" three; printf "%s," "$@" '
                     '> output; /bin/cat output')
        self.compare('IFS=:; p=a:b; printf "%s," $p > output; /bin/cat output')

    def test_control_bytes_always_fall_back(self):
        # Include DEL, all internal sentinels, and backslash-adjacent controls.
        # Raw sentinels have historical shell-specific decoding, not Bash parity.
        for value in (*range(1, 32), 127):
            for prefix in ("", "\\"):
                with self.subTest(value=value, prefix=prefix):
                    self.compare('printf "%s" "$p" > output; /bin/cat output',
                                 variables={"p": prefix + chr(value)}, bash=False)

    def test_nested_functions_and_capture_writer(self):
        self.compare('set -- outer; '
                     'g(){ printf "%s" "$1" > output; printf G; }; '
                     'f(){ g inner; printf "%s" "$1"; }; '
                     'f caller; printf "%s" "$1"; /bin/cat output')
        for redirect in ("> output", "2> output", "< input", "3> output", ""):
            with self.subTest(redirect=redirect):
                self.compare('f(){ printf "%s" captured ' + redirect + '; }; '
                             'value=$(f); printf "%s" "$value"')
        self.compare('f(){ printf "%s" nested > output; echo AFTER; }; '
                     'f > outer; /bin/cat output outer', files=("output", "outer"))

    def test_multiline_and_outer_control_flow(self):
        self.compare('p=first\nprintf "%s" "$p" > output\n'
                     'p=second\necho "$p" >> output\n/bin/cat output')
        self.compare('p="first\nsecond"; printf "%s" "$p" > output; /bin/cat output')
        self.compare('false && echo BAD > output; true && printf GOOD > output; '
                     'false || echo END >> output; /bin/cat output')
        self.compare('printf BODY > output | while read value; do echo BAD; done; '
                     '/bin/cat output')
        self.compare('printf "%s" word > output & wait; /bin/cat output')
        self.compare('f(){ printf BODY > output; return 7; echo BAD; }; '
                     'f; printf "%s" "$?"; /bin/cat output')

    def test_alias_disabled_builtin_and_lookup_flag(self):
        # Redirected builtins currently precede aliases/functions in Jash.
        for code in (
            'alias printf="echo ALIAS"; printf BODY > output; /bin/cat output',
            'printf(){ echo FUNCTION; }; printf BODY > output; /bin/cat output',
            'alias p="printf"; p BODY > output; /bin/cat output',
            'enable -n printf; printf "%s" external > output; /bin/cat output',
            'enable -n echo; echo external > output; /bin/cat output',
            'printf(){ echo FUNCTION; }; enable -n printf; '
            'printf BODY > output; /bin/cat output',
            'f(){ printf native > output; g; }; g(){ echo FUNCTION; }; '
            'command f; g; /bin/cat output',
            'g(){ echo FUNCTION; }; command printf native > output; '
            'g; g; /bin/cat output',
        ):
            with self.subTest(code=code):
                self.compare(code, bash=False)

    def test_traps_and_errors(self):
        self.compare('trap "echo DEBUG" DEBUG; printf BODY > output; '
                     'trap - DEBUG; /bin/cat output', bash=False)
        self.compare('trap "echo EXIT" EXIT; printf BODY > output; /bin/cat output')
        self.compare('trap "echo RETURN" RETURN; '
                     'f(){ printf BODY > output; }; f; /bin/cat output', bash=False)
        for code in ('set -u; printf "%s" "$missing" > output',
                     'printf "%s" "${missing?required}" > output',
                     'printf "%s" "$((1/0))" > output',
                     'printf BODY > missing-dir/output',
                     'echo BODY >',
                     'printf "%s" "unterminated > output'):
            with self.subTest(code=code):
                self.compare(code, bash=False)
        if Path("/dev/full").exists():
            self.compare('set -e; printf BODY > /dev/full; echo BAD > output')

    def test_word_layer_limit_keeps_diagnostic(self):
        # Use a script file to avoid execve's per-string cap. Include both
        # sides of the word layer's n + 1 >= cap boundary.
        for size in (262142, 262143, 262144):
            with self.subTest(size=size):
                overhead = len('printf "%s" "" > output')
                result = self.compare('printf "%s" "' + 'x' * (size - overhead) +
                                      '" > output', bash=False)
                if size >= 262143:
                    self.assertIn(b"excede o teto de expansao", result[2])


if __name__ == "__main__":
    unittest.main()
