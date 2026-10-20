#!/usr/bin/env python3
"""Focused startup/EXIT/heredoc regressions; JASH may select a Bash oracle."""

import os
from pathlib import Path
import shlex
import signal
import stat
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
SHELL = Path(os.environ.get("JASH", ROOT / "build/jash")).resolve()
IS_BASH = SHELL.name == "bash"


class StartupWaves(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="jash-startup-waves-")
        self.addCleanup(self.tmp.cleanup)
        self.work = Path(self.tmp.name)
        home = self.work / "home"
        home.mkdir()
        (home / ".jashrc").touch()
        self.temp = self.work / "tmp"
        self.temp.mkdir()
        self.env = {
            "HOME": str(home), "TMPDIR": str(self.temp),
            "PATH": "/usr/bin:/bin", "LC_ALL": "C", "TERM": "dumb",
            "JASH_SIGIL": "0", "JASH_CHANGES": "0", "CHANGES": "0",
            "JASH_SAFE": "0", "JASH_BRAIN": "0",
            "JASH_MEMO": "0", "JASH_BRIEF": "0", "JASH_NUDGE": "0",
            "JASH_LOOPGUARD": "off", "JASH_DRIFT": "0",
            "JASH_RECALL_GUARD": "0", "JASH_RISK_CHECKPOINT": "0",
            "JASH_GUARD": "0", "JASH_GUARD_STRICT": "0",
            "JASH_GUARD_BYPASS": "0", "JASH_GUARD_BYPASS_STRICT": "0",
            "JASH_SKIP_SELFCHECK": "1", "JASH_WORLD": "0",
            "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
            "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
        }

    def run_shell(self, code, *, mode="c", args=(), flags=(), umask=0o022):
        argv = [str(SHELL), *flags]
        stdin = b""
        if mode == "c":
            argv += ["-c", code, *args]
        elif mode == "ec":
            argv += ["-ec", code, *args]
        elif mode == "script":
            (self.work / "script.sh").write_text(code)
            argv += ["script.sh", *args]
        elif mode == "stdin":
            stdin = code.encode()
        else:
            raise ValueError(mode)
        proc = subprocess.run(argv, input=stdin, capture_output=True,
                              env=self.env, cwd=self.work, timeout=10,
                              umask=umask)
        return proc.returncode, proc.stdout, proc.stderr

    def test_command_positionals(self):
        code = 'printf "%s|%s|%s\\n" "$0" "$1" "$#"'
        for mode in ("c", "ec"):
            for args, expected in (
                ((), f"{SHELL}||0\n".encode()),
                (("zero", "one"), b"zero|one|1\n"),
                (("", ""), b"||1\n"),
                (("zero", "a 'quoted' $value `literal`"),
                 b"zero|a 'quoted' $value `literal`|1\n"),
                (("zero", "--agent"), b"zero|--agent|1\n"),
                (("zero", "--bg"), b"zero|--bg|1\n"),
                (("--bg", "--agent"), b"--bg|--agent|1\n"),
                (("--agent", "--bg"), b"--agent|--bg|1\n"),
            ):
                with self.subTest(mode=mode, args=args):
                    self.assertEqual(self.run_shell(code, mode=mode, args=args),
                                     (0, expected, b""))

    @unittest.skipIf(IS_BASH, "Bash requires -- before dash-leading command strings")
    def test_flag_payload_is_not_startup_option(self):
        for flag in ("--bg", "--agent"):
            with self.subTest(flag=flag):
                rc, out, err = self.run_shell(flag)
                self.assertEqual((rc, out), (127, b""), err)
                self.assertIn(flag.encode(), err)

    def test_positional_limit(self):
        args = ("zero", *[str(i) for i in range(1, 385)])
        self.assertEqual(self.run_shell('printf "%s|%s\\n" "$#" "${384}"',
                                        args=args), (0, b"384|384\n", b""))

    @unittest.skipIf(IS_BASH, "Jash's bounded positional storage")
    def test_overflow_precedes_literal_fast_path(self):
        rc, out, err = self.run_shell("echo must-not-run > marker",
                                      args=("zero", *["arg"] * 385))
        self.assertEqual((rc, out), (2, b""), err)
        self.assertIn(b"positional", err)
        self.assertFalse((self.work / "marker").exists())

    @unittest.skipIf(IS_BASH, "Jash session restoration")
    def test_positionals_replace_restored_session(self):
        self.env["JASH_SESSION"] = "startup-positionals"
        self.assertEqual(self.run_shell("set -- stale extra; :"), (0, b"", b""))
        code = 'printf "%s|%s|%s|%s\\n" "$0" "$1" "$2" "$#"'
        self.assertEqual(self.run_shell(code, args=("zero", "fresh")),
                         (0, b"zero|fresh||1\n", b""))
        self.assertEqual(self.run_shell(code),
                          (0, b"zero|fresh||1\n", b""))
        self.assertEqual(self.run_shell(code, args=("empty",)),
                          (0, b"empty|||0\n", b""))

    def test_exit_traps(self):
        cases = (
            ("trap 'echo first; echo second' EXIT; exit 7", 7, b"first\nsecond\n"),
            ("trap 'echo status:$?; true' EXIT; false", 1, b"status:1\n"),
            ("trap 'false' EXIT; true", 0, b""),
            ("trap 'exit 9' EXIT; exit 7", 9, b""),
            ("trap 'echo first; exit 9; echo unreachable' EXIT; false", 9, b"first\n"),
            ("trap 'exit' EXIT; exit 7", 7, b""),
        )
        for mode in ("c", "script", "stdin"):
            for code, rc, out in cases:
                with self.subTest(mode=mode, code=code):
                    self.assertEqual(self.run_shell(code + "\n", mode=mode),
                                     (rc, out, b""))

    def test_trap_action_word(self):
        values = ("a  b", "$literal", "`literal`", "quote'and\"double", "back\\slash", "EXIT", "9")
        action = "printf '%s\\n' " + " ".join(shlex.quote(value) for value in values)
        self.assertEqual(self.run_shell(f"trap {shlex.quote(action)} EXIT; exit 7"),
                         (7, "\n".join((*values, "")).encode(), b""))
        self.assertEqual(self.run_shell(
            "v=before; trap 'printf \"%s\\n\" \"$v\"' EXIT; v=after; exit 7"),
            (7, b"after\n", b""))
        self.assertEqual(self.run_shell("trap 'echo 9' EXIT; exit 7"),
                         (7, b"9\n", b""))

    def test_trap_listing(self):
        action = "printf '%s\\n' 'a  b' \"$v\"; :"
        for opts in ("", "-p", "--", "-p EXIT TERM", "-p -- EXIT TERM"):
            with self.subTest(opts=opts):
                rc, out, err = self.run_shell(
                    f"trap -- {shlex.quote(action)} EXIT TERM; trap {opts}; trap - EXIT TERM")
                self.assertEqual((rc, err), (0, b""))
                self.assertEqual([shlex.split(line) for line in out.decode().splitlines()],
                                 [["trap", "--", action, "EXIT"],
                                  ["trap", "--", action, "SIGTERM"]])
                self.assertEqual(self.run_shell(out.decode() + "trap -p; trap - EXIT TERM"),
                                 (0, out, b""))
        self.assertEqual(self.run_shell(
            "trap 'echo one' EXIT TERM; trap -p TERM EXIT; trap - EXIT TERM"),
            (0, b"trap -- 'echo one' SIGTERM\ntrap -- 'echo one' EXIT\n", b""))
        rc, out, err = self.run_shell("trap -l")
        self.assertEqual((rc, err), (0, b""))
        self.assertIn(b"TERM", out)
        self.assertIn(b"HUP", out)

    def test_trap_resets_and_empty_action(self):
        for reset in ("trap - EXIT", "trap '-' EXIT", "trap EXIT", "trap 0"):
            with self.subTest(reset=reset):
                self.assertEqual(self.run_shell(
                    f"trap 'echo unwanted' EXIT; {reset}; trap -p EXIT; exit 7"),
                    (7, b"", b""))
        self.assertEqual(self.run_shell(
            "trap 'echo unwanted' EXIT; trap '' EXIT; trap -p EXIT; exit 7"),
            (7, b"trap -- '' EXIT\n", b""))
        self.assertEqual(self.run_shell("trap 'exit 9' 0; exit 7"), (9, b"", b""))
        self.assertEqual(self.run_shell("trap '' USR1; /bin/kill -USR1 $$; echo alive"),
                         (0, b"alive\n", b""))
        self.assertEqual(self.run_shell("trap '' USR1; trap - USR1; /bin/kill -USR1 $$"),
                         (-signal.SIGUSR1, b"", b""))
        self.assertEqual(self.run_shell(
            "trap 'trap - EXIT; echo once' EXIT; trap ':' TERM; exit 7"),
            (7, b"once\n", b""))

    def test_trap_dispatch_paths(self):
        for prefix in ("", "builtin ", "command "):
            for redirect in ("", " > trap-output"):
                with self.subTest(prefix=prefix, redirect=redirect):
                    self.assertEqual(self.run_shell(
                        f"{prefix}trap 'exit 9' EXIT{redirect}; exit 7"), (9, b"", b""))
        for name in ('"trap"', "'trap'", '"$b"'):
            for redirect in ("", " > trap-output"):
                with self.subTest(name=name, redirect=redirect):
                    self.assertEqual(self.run_shell(
                        f"b=trap; builtin {name} 'exit 9' EXIT{redirect}; exit 7"),
                        (9, b"", b""))
        for code in ("trap 'echo hi' TERM | /usr/bin/cat",
                     "/usr/bin/true | trap 'echo hi' TERM"):
            with self.subTest(code=code):
                self.assertEqual(self.run_shell("set -o pipefail; " + code), (0, b"", b""))

    def test_trap_invalid_operands(self):
        for code, status in (("trap -z", 2), ("trap echo", 2),
                             ("trap true 'EXIT TERM'", 1), ("trap -p NOT_A_SIGNAL", 1)):
            with self.subTest(code=code):
                rc, out, err = self.run_shell(code)
                self.assertEqual((rc, out), (status, b""))
                self.assertTrue(err)

    def test_redirect_creation_mode(self):
        for mask in (0, 0o002, 0o022, 0o077):
            for op in (">", ">>"):
                with self.subTest(mask=oct(mask), op=op):
                    path = self.work / "output"
                    path.unlink(missing_ok=True)
                    self.assertEqual(self.run_shell(f"echo payload {op} output", umask=mask),
                                     (0, b"", b""))
                    self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o666 & ~mask)
                    self.assertEqual(path.read_bytes(), b"payload\n")

    def test_fast_echo_fd_prefixes(self):
        for code in ("echo PAYLOAD 1>/dev/stdout", "echo PAYLOAD 2>/dev/null"):
            for suffix in ("", ";"):
                with self.subTest(code=code, suffix=suffix):
                    self.assertEqual(self.run_shell(code + suffix), (0, b"PAYLOAD\n", b""))
        cases = (
            ("PAYLOAD 1", b"", b"PAYLOAD\n"),
            ("PAYLOAD 2", b"PAYLOAD\n", b""),
            ("PAYLOAD x2", b"", b"PAYLOAD x2\n"),
            ("PAYLOAD 2 ", b"", b"PAYLOAD 2\n"),
            ("1", b"", b"\n"),
        )
        path = self.work / "fd-output"
        for args, out, contents in cases:
            for op in (">", ">>"):
                for suffix in ("", ";"):
                    for mask in (0, 0o077):
                        for existing in (False, True):
                            with self.subTest(args=args, op=op, suffix=suffix,
                                              mask=oct(mask), existing=existing):
                                path.unlink(missing_ok=True)
                                if existing:
                                    path.write_bytes(b"seed\n")
                                    path.chmod(0o604)
                                self.assertEqual(self.run_shell(
                                    f"echo {args}{op}fd-output{suffix}", umask=mask),
                                    (0, out, b""))
                                expected = (b"seed\n" if existing and op == ">>" else b"") + contents
                                self.assertEqual(path.read_bytes(), expected)
                                self.assertEqual(stat.S_IMODE(path.stat().st_mode),
                                                 0o604 if existing else 0o666 & ~mask)

    def test_heredoc_temporary_paths(self):
        cases = (
            ("cat <<EOF\nhello\nEOF\n", b"hello\n"),
            ("cat <<'EOF'\n$HOME `literal`\nEOF\n", b"$HOME `literal`\n"),
            ("for v in one two; do\ncat <<EOF\n$v\nEOF\ndone\n", b"one\ntwo\n"),
            ('v=expanded\ncat <<< "$v"\n', b"expanded\n"),
            ("cat <<EOF | /usr/bin/tr a-z A-Z\nhello\nEOF\n", b"HELLO\n"),
        )
        for name in ("plain", "with spaces", "quote'and\"double", "dollar$HOME",
                     "back`touch injected`tick", "back\\slash", "all ' $HOME `touch injected` \\"):
            temp = self.temp / name
            temp.mkdir()
            self.env["TMPDIR"] = str(temp)
            for mode in ("c", "script", "stdin"):
                for code, out in cases:
                    with self.subTest(path=name, mode=mode, code=code):
                        self.assertEqual(self.run_shell(code, mode=mode), (0, out, b""))
                        self.assertFalse((self.work / "injected").exists())

    def test_heredoc_cleanup(self):
        (self.temp / "unrelated").write_text("keep")
        cases = (
            ("cat <<EOF\none\nEOF\ncat <<'EOF'\n$literal\nEOF\ncat <<< 'three'\ncd home\n",
             0, b"one\n$literal\nthree\n"),
            ("cat <<'EOF'\nbody\nEOF\nfalse\n", 1, b"body\n"),
            ("exit 7\ncat <<'EOF'\nunused\nEOF\n", 7, b""),
        )
        for mode in ("c", "script", "stdin"):
            for code, rc, out in cases:
                with self.subTest(mode=mode, code=code):
                    self.assertEqual(self.run_shell(code, mode=mode), (rc, out, b""))
                    self.assertEqual(sorted(path.name for path in self.temp.iterdir()),
                                     ["unrelated"])

    def test_heredoc_deferred_functions_and_nested_source(self):
        (self.work / "inner.sh").write_text(
            "inner(){\ncat <<'EOF'\ninner $literal\nEOF\n}\n"
            "strings(){\ncat <<< 'here $literal'\n}\n"
            "cat <<'EOF'\nsource-now\nEOF\n")
        (self.work / "outer.sh").write_text(
            ". inner.sh\nouter(){\ncat <<'EOF'\nouter $literal\nEOF\n}\n")
        code = ("root(){\ncat <<'EOF'\nroot $literal\nEOF\n}\n"
                ". outer.sh\nouter\ninner\nstrings\nroot\ntrap root EXIT\n")
        for mode in ("c", "script", "stdin"):
            with self.subTest(mode=mode):
                self.assertEqual(self.run_shell(code, mode=mode),
                                 (0, b"source-now\nouter $literal\ninner $literal\n"
                                     b"here $literal\nroot $literal\nroot $literal\n", b""))
                # Stored functions may hand paths to descendants; retention is conservative.

    def test_heredoc_background_lifetime(self):
        for waited in (False, True):
            with self.subTest(waited=waited):
                temp = self.temp / str(waited)
                temp.mkdir()
                self.env["TMPDIR"] = str(temp)
                code = ("later(){\n/bin/sleep 0.1\ncat <<'EOF'\nlater\nEOF\n}\n"
                        "later &\n" + ("wait\n" if waited else ""))
                self.assertEqual(self.run_shell(code), (0, b"later\n", b""))
                # wait can reap a leader while its descendants still need the file.

    def test_heredoc_background_grandchild(self):
        definition = "later(){\n/bin/sleep 0.2\ncat <<'EOF'\nlater\nEOF\n}\n"
        (self.work / "later.sh").write_text(definition)
        for mode in ("c", "script", "stdin"):
            for launch in ("(later &)\n", "(later &)\nwait\n"):
                with self.subTest(mode=mode, launch=launch):
                    self.assertEqual(self.run_shell(definition + launch, mode=mode),
                                     (0, b"later\n", b""))
        self.assertEqual(self.run_shell(". later.sh; (later &)"), (0, b"later\n", b""))


if __name__ == "__main__":
    unittest.main()
