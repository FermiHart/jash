#!/usr/bin/env python3
"""Bash-oracle read regressions; select an existing shell with JASH=/path/to/jash.

    JASH=build/jash python3 tests/test_read_regular.py -v
    JASH=build/jash python3 tests/test_read_regular.py ReadRegularBehavior -v

Input bytes live in fixtures, never in shell source/argv. Every shell uses -c,
so stdin is data, not a script. Each oracle/candidate invocation opens its own
file description; Python observes the kernel offset without buffered I/O.
Unqualified `cat` exercises Jash's builtin (Bash resolves the external utility).
The optional strace test counts syscalls, not elapsed time. No build is performed.
"""

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
from dataclasses import dataclass


ROOT = Path(__file__).resolve().parent.parent
JASH = Path(os.environ.get("JASH", ROOT / "build/jash")).resolve()
BASH = Path("/bin/bash")
LENGTHS = (511, 512, 1023, 1024, 8191, 8192, 32768, 128 * 1024)
PREFIX = b"ignored prefix\nnot part of the read: "
TAIL = b"next line\nlast\\line\r\n\x00\xfftail"


@dataclass
class Observation:
    rc: int
    stdout: bytes
    stderr: bytes
    offsets: tuple
    unread: bytes
    trace: str


def dump_read(options="-r", names=("value",), redirection="", consumer=None):
    """Save read's status before printing, and return it even after cat runs."""
    variables = names or ("REPLY",)
    initialize = "; ".join(f"{name}=stale" for name in variables)
    values = " ".join(f'"${name}"' for name in variables)
    code = (f"{initialize}; read {options} {' '.join(names)} {redirection}; "
            f"read_rc=$?; printf '<%s>' {values}; printf '\\nread=%s\\n' \"$read_rc\"; ")
    if consumer:
        code += f"{consumer}; cat_rc=$?; printf '\\ncat=%s\\n' \"$cat_rc\"; "
    return code + 'exit "$read_rc"'


class ReadHarness(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="jash-read-regular-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.invocations = 0

    def run_shell(self, shell, code, data, *, start=0, passed=False,
                  duplicate=False, pipe=False, files=None, trace=False):
        self.invocations += 1
        root = self.root / str(self.invocations)
        root.mkdir()
        home, work = root / "home", root / "work"
        home.mkdir()
        work.mkdir()
        (home / ".jashrc").touch()
        for name, contents in (files or {}).items():
            (work / name).write_bytes(contents)
        env = {
            "HOME": str(home), "TMPDIR": str(home),
            "PATH": "/usr/bin:/bin", "LC_ALL": "C", "TERM": "dumb",
            "JASH_SIGIL": "0", "JASH_TIMELINE": "0", "JASH_TRASH": "off",
            "JASH_CHANGES": "0", "CHANGES": "0", "JASH_SAFE": "0",
            "JASH_BRAIN": "0", "JASH_WORLD": "0", "JASH_MEMO": "0",
            "JASH_BRIEF": "0", "JASH_NUDGE": "0", "JASH_DRIFT": "0",
            "JASH_LOOPGUARD": "off", "JASH_RECALL_GUARD": "0",
            "JASH_RISK_CHECKPOINT": "0", "JASH_GUARD": "0",
            "JASH_GUARD_BYPASS": "0", "JASH_GUARD_BYPASS_STRICT": "0",
            "JASH_SKIP_SELFCHECK": "1", "JASH_LLM_BUDGET": "0",
            "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
            "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
        }
        if pipe:
            self.assertEqual(start, 0)
            self.assertLess(len(data), 4096, "prefilled pipe must not block")
            fd, writer = os.pipe()
            try:
                self.assertEqual(os.write(writer, data), len(data))
            finally:
                os.close(writer)
        else:
            path = root / "input.bin"
            path.write_bytes(data)
            fd = os.open(path, os.O_RDONLY)
            os.lseek(fd, start, os.SEEK_SET)
        fds = [fd]
        try:
            if duplicate:
                fds.append(os.dup(fd))
            source = code(tuple(fds)) if callable(code) else code
            argv = [str(shell)]
            if shell.resolve() == BASH.resolve():
                argv += ["--noprofile", "--norc"]
            argv += ["-c", source]
            trace_path = root / "syscalls.log"
            if trace:
                argv = ["strace", "-qq", "-f", "-o", str(trace_path),
                        "-e", "trace=read,fstat,newfstatat,lseek", *argv]
            result = subprocess.run(
                argv, stdin=subprocess.DEVNULL if passed else fd,
                pass_fds=tuple(fds) if passed else (), capture_output=True,
                env=env, cwd=work, timeout=20,
            )
            offsets = () if pipe else tuple(os.lseek(f, 0, os.SEEK_CUR) for f in fds)
            unread = b""
            if pipe:
                while True:
                    chunk = os.read(fd, 4096)
                    if not chunk:
                        break
                    unread += chunk
            trace_text = trace_path.read_text() if trace and trace_path.exists() else ""
            if (trace and result.returncode != 0 and b"strace:" in result.stderr
                    and b"Operation not permitted" in result.stderr):
                self.skipTest("strace/ptrace is not permitted in this environment")
            self.assertNotIn(b"AddressSanitizer", result.stderr)
            self.assertNotIn(b"runtime error:", result.stderr)
            return Observation(result.returncode, result.stdout, result.stderr,
                               offsets, unread, trace_text)
        finally:
            for opened in fds:
                os.close(opened)

    def assert_parity(self, code, data, *, offset=None, unread=None,
                      rc=None, stdout=None, **kwargs):
        reference = self.run_shell(BASH, code, data, **kwargs)
        # Explicit fixture expectations also validate the oracle/harness setup.
        self.assertEqual(reference.stderr, b"")
        if offset is not None:
            self.assertEqual(reference.offsets, (offset,) * len(reference.offsets))
        if unread is not None:
            self.assertEqual(reference.unread, unread)
        if rc is not None:
            self.assertEqual(reference.rc, rc)
        if stdout is not None:
            self.assertEqual(reference.stdout, stdout)
        actual = self.run_shell(JASH, code, data, **kwargs)
        self.assertEqual(actual.stderr, b"")
        self.assertEqual(actual.rc, reference.rc, "read exit status")
        self.assertEqual(actual.offsets, reference.offsets, "parent-observed kernel offsets")
        self.assertEqual(actual.unread, reference.unread, "bytes left in pipe")
        self.assertEqual(actual.stdout, reference.stdout, "byte-exact read/cat output")
        return actual


class ReadRegularBehavior(ReadHarness):
    def test_read_then_external_and_builtin_cat(self):
        for consumer in ("/bin/cat", "cat"):
            for first in (b"first line\\literal", b"", b"x" * 8192):
                with self.subTest(consumer=consumer, length=len(first)):
                    data = first + b"\n" + TAIL
                    self.assert_parity(
                        "IFS=; " + dump_read(consumer=consumer), data,
                        offset=len(data), rc=0,
                        stdout=b"<" + first + b">\nread=0\n" + TAIL + b"\ncat=0\n",
                    )

    def test_empty_line_file_and_partial_eof(self):
        for data in (b"", b"\n", b"partial", b"\x00", b"\x00\n", b"part\x00ial"):
            for names in (("value",), ()):
                with self.subTest(data=data, names=names or "REPLY"):
                    rc = 0 if data.endswith(b"\n") else 1
                    value = data.removesuffix(b"\n").replace(b"\x00", b"")
                    self.assert_parity(
                        "IFS=; " + dump_read(names=names), data,
                        offset=len(data), rc=rc,
                        stdout=b"<" + value + b">\nread=" + str(rc).encode() + b"\n",
                    )

    def test_raw_backslashes_crlf_and_nul(self):
        for line in (b"one\\two\\\\three", b"trailing\\", b"a\rb\r",
                     b"\r", b"a\x00b\x00c", b"\x00"):
            for consumer in ("/bin/cat", "cat"):
                with self.subTest(line=line, consumer=consumer):
                    data = line + b"\n" + TAIL
                    self.assert_parity(
                        "IFS=; " + dump_read(consumer=consumer), data,
                        offset=len(data), rc=0,
                        stdout=b"<" + line.replace(b"\x00", b"")
                        + b">\nread=0\n" + TAIL + b"\ncat=0\n",
                    )

    def test_line_lengths_and_nonzero_passed_fd_offsets(self):
        for length in LENGTHS:
            line = (b"0123456789abcdef" * ((length + 15) // 16))[:length]
            data = PREFIX + line + b"\n" + TAIL
            for names in (("value",), ()):
                with self.subTest(length=length, names=names or "REPLY"):
                    self.assert_parity(
                        lambda fds: "IFS=; " + dump_read(
                            names=names, redirection=f"<&{fds[0]}"),
                        data, start=len(PREFIX), passed=True,
                        offset=len(PREFIX) + length + 1, rc=0,
                        stdout=b"<" + line + b">\nread=0\n",
                    )

    def test_shared_offset_at_partial_and_empty_eof(self):
        for suffix in (b"partial", b""):
            with self.subTest(suffix=suffix):
                data = PREFIX + suffix
                self.assert_parity(
                    lambda fds: "IFS=; " + dump_read(redirection=f"<&{fds[0]}"),
                    data, start=len(PREFIX), passed=True,
                    offset=len(data), rc=1,
                    stdout=b"<" + suffix + b">\nread=1\n",
                )

    def test_parent_duplicated_descriptors_share_consumption(self):
        data = PREFIX + b"first\nsecond\n" + TAIL
        for count in (1, 2):
            with self.subTest(reads=count):
                def code(fds):
                    source = "IFS=; "
                    for fd in fds[:count]:
                        source += (f"read -r value <&{fd}; read_rc=$?; "
                                   "printf '<%s:%s>' \"$read_rc\" \"$value\"; ")
                    return source + 'exit "$read_rc"'
                self.assert_parity(
                    code, data, start=len(PREFIX), passed=True, duplicate=True,
                    offset=len(PREFIX) + (6 if count == 1 else 13), rc=0,
                    stdout=b"<0:first>" + (b"<0:second>" if count == 2 else b""),
                )

    def test_shell_alias_of_passed_fd(self):
        data = PREFIX + b"first\nsecond\n" + TAIL
        for consumer in (None, "/bin/cat", "cat"):
            with self.subTest(consumer=consumer):
                def code(fds):
                    alias = next(n for n in range(9, 20) if n not in fds)
                    source = (f"IFS=; exec {alias}<&{fds[0]}; "
                              f"read -r first <&{alias}; first_rc=$?; "
                              f"read -r second <&{fds[0]}; second_rc=$?; "
                              "printf '<%s:%s><%s:%s>' \"$first_rc\" \"$first\" "
                              '"$second_rc" "$second"; ')
                    if consumer:
                        source += f"{consumer} <&{alias}; "
                    return source + f'exec {alias}<&-; exit "$second_rc"'
                self.assert_parity(
                    code, data, start=len(PREFIX), passed=True, rc=0,
                    offset=len(data) if consumer else len(PREFIX) + 13,
                    stdout=b"<0:first><0:second>" + (TAIL if consumer else b""),
                )

    def test_repeated_file_redirects_reopen_and_restore_stdin(self):
        code = ("IFS=; for file in one two one two one two; do "
                'read -r value <"$file"; read_rc=$?; '
                "printf '<%s:%s>' \"$read_rc\" \"$value\"; done; "
                "read -r value; read_rc=$?; "
                "printf '<%s:%s>' \"$read_rc\" \"$value\"; exit \"$read_rc\"")
        self.assert_parity(
            code, b"original\n" + TAIL,
            files={"one": b"one\nstale-one\n", "two": b"two\nstale-two\n"},
            offset=9, rc=0,
            stdout=b"<0:one><0:two>" * 3 + b"<0:original>",
        )

    def test_direct_file_read_with_closed_standard_fd(self):
        for closed in ("0<&-", "2>&-", "0<&- 2>&-"):
            with self.subTest(closed=closed):
                self.assert_parity(
                    f"exec {closed}; " + dump_read(redirection="<one"),
                    b"original\n", files={"one": b"value\nremaining\n"},
                    offset=0, rc=0, stdout=b"<value>\nread=0\n")

    def test_direct_pipe_file_path_does_not_overread(self):
        if not Path("/dev/fd").is_dir():
            self.skipTest("requires /dev/fd")
        self.assert_parity(
            lambda fds: dump_read(redirection=f"</dev/fd/{fds[0]}"),
            b"value\n" + TAIL, passed=True, pipe=True, rc=0,
            unread=TAIL, stdout=b"<value>\nread=0\n")

    def test_private_file_read_preserves_original_stdio_buffer(self):
        data = b"first\noriginal-tail\n"
        control = self.run_shell(JASH, "grep -q first; cat", data)
        self.assertEqual((control.rc, control.stdout, control.stderr), (0, b"original-tail\n", b""))
        actual = self.run_shell(
            JASH, "grep -q first; read -r value <one; printf '<%s>' \"$value\"; cat",
            data, files={"one": b"value\nother\n"})
        self.assertEqual((actual.rc, actual.stdout, actual.stderr),
                         (0, b"<value>original-tail\n", b""))

    def test_ifs_and_reply(self):
        cases = (
            ("IFS=", b"  alpha\tbeta  ", ("a",)),
            ("IFS=", b"  alpha\tbeta  ", ("a", "b")),
            ("unset IFS", b" \talpha\t beta  gamma delta  \t", ("a", "b", "c")),
            ("unset IFS", b"alpha", ("a", "b", "c")),
            ("IFS=:", b"alpha:beta:gamma:delta", ("a", "b", "c")),
            ("IFS=:", b"alpha::gamma", ("a", "b", "c")),
            ("IFS=:", b":beta:gamma", ("a", "b", "c")),
            ("IFS=:", b"alpha:beta:", ("a", "b", "c")),
        )
        for setup, line, names in cases:
            with self.subTest(setup=setup, line=line, names=names):
                self.assert_parity(setup + "; " + dump_read(names=names),
                                   line + b"\n" + TAIL, offset=len(line) + 1, rc=0)
        for setup in ("IFS=", "unset IFS", "IFS=:"):
            with self.subTest(setup=setup, names="REPLY"):
                line = b" \talpha:beta  \t"
                self.assert_parity(
                    setup + "; " + dump_read(names=()), line + b"\n" + TAIL,
                    offset=len(line) + 1, rc=0, stdout=b"<" + line + b">\nread=0\n",
                )

    def test_long_first_and_final_fields(self):
        for length in (511, 512, 1023, 1024):
            for setup, separator in (("unset IFS", b" "), ("IFS=:", b":")):
                for long_first in (True, False):
                    with self.subTest(length=length, setup=setup, long_first=long_first):
                        fields = (b"x" * length, b"last") if long_first else (b"first", b"x" * length)
                        line = separator.join(fields)
                        self.assert_parity(
                            setup + "; " + dump_read(names=("a", "b")),
                            line + b"\n" + TAIL, offset=len(line) + 1, rc=0,
                            stdout=b"<" + fields[0] + b"><" + fields[1] + b">\nread=0\n",
                        )

    # Ready, prefilled inputs make -t deterministic: this checks its acquisition
    # fallback, without testing timeout scheduling or extending option grammar.
    FALLBACKS = (
        ("raw_pipe", "-r", b"one\\two\n", b"one\\two"),
        ("escapes", "", b"one\\q\\\\two\n", b"oneq\\two"),
        ("continuation", "", b"one\\\ntwo\n", b"onetwo"),
        ("n_count", "-r -n 3", b"abc", b"abc"),
        ("n_zero", "-r -n 0", b"", b""),
        ("n_newline", "-r -n 8", b"ab\n", b"ab"),
        ("delimiter", "-r -d :", b"one\ntwo:", b"one\ntwo"),
        ("timeout_ready", "-r -t 1", b"ready\n", b"ready"),
    )

    def test_fallback_regular_file_offsets(self):
        for label, options, consumed, value in self.FALLBACKS:
            with self.subTest(mode=label):
                self.assert_parity(
                    lambda fds: "IFS=; " + dump_read(options, redirection=f"<&{fds[0]}"),
                    PREFIX + consumed + TAIL, passed=True, start=len(PREFIX),
                    offset=len(PREFIX) + len(consumed), rc=0,
                    stdout=b"<" + value + b">\nread=0\n",
                )

    def test_fallback_pipe_leaves_exact_remaining_bytes(self):
        for label, options, consumed, value in self.FALLBACKS:
            for consumer in (None, "/bin/cat", "cat"):
                with self.subTest(mode=label, consumer=consumer):
                    self.assert_parity(
                        "IFS=; " + dump_read(options, consumer=consumer),
                        consumed + TAIL, pipe=True, rc=0,
                        unread=b"" if consumer else TAIL,
                        stdout=b"<" + value + b">\nread=0\n"
                        + (TAIL + b"\ncat=0\n" if consumer else b""),
                    )


class ReadRegularSyscalls(ReadHarness):
    @unittest.skipUnless(shutil.which("strace"), "optional: requires strace")
    def test_regular_loop_uses_bounded_read_calls(self):
        # 200 newline-terminated records of 35 bytes: a byte-at-a-time reader
        # needs >=7001 calls. Per-invocation block reads fit comfortably below
        # 500, with slack for EOF/probing and different buffer sizes.
        data = (b"x" * 34 + b"\n") * 200
        code = ("IFS=; count=0; while read -r line; do count=$((count+1)); done; "
                "printf '%s\\n' \"$count\"")
        reference = self.run_shell(BASH, code, data)
        self.assertEqual((reference.rc, reference.stdout, reference.stderr,
                          reference.offsets), (0, b"200\n", b"", (len(data),)))
        actual = self.run_shell(JASH, code, data, trace=True)
        self.assertEqual((actual.rc, actual.stdout, actual.stderr, actual.offsets),
                         (reference.rc, reference.stdout, reference.stderr, reference.offsets))
        calls = len(re.findall(r"^\s*(?:\d+\s+)?read\(0,", actual.trace, re.MULTILINE))
        self.assertGreater(calls, 0, "trace must contain stdin reads (no vacuous pass)")
        self.assertLess(calls, 500, f"{calls} stdin read syscalls for 200 short regular-file lines")

    @unittest.skipUnless(shutil.which("strace"), "optional: requires strace")
    def test_repeated_redirect_uses_bounded_read_calls(self):
        code = ("for ((i=0;i<200;i++)); do read -r line <one || exit; done; "
                "printf '%s:%s' \"$i\" \"$line\"")
        actual = self.run_shell(JASH, code, b"original\n",
                                files={"one": b"value\nremaining\n"}, trace=True)
        self.assertEqual((actual.rc, actual.stdout, actual.stderr, actual.offsets),
                         (0, b"200:value", b"", (0,)))
        calls = len(re.findall(r"^\s*(?:\d+\s+)?read\(\d+,", actual.trace, re.MULTILINE))
        self.assertGreaterEqual(calls, 200)
        self.assertLess(calls, 500, actual.trace)


if __name__ == "__main__":
    unittest.main()
