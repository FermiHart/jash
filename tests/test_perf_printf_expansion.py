#!/usr/bin/env python3
"""Wave 3-4 correctness gates, not benchmarks or a shared shell build.

JASH selects the shell under test. PrintfWriter and ExpansionAPI use temporary
C harnesses; select PrintfExpansion alone to test a frozen baseline.
"""

import ctypes
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SHELL = Path(os.environ.get("JASH", ROOT / "build/jash")).resolve()
LONG_BITS = ctypes.sizeof(ctypes.c_long) * 8
LONG_MIN = str(-(1 << (LONG_BITS - 1)))
ULONG_MAX = str((1 << LONG_BITS) - 1)
PRINTF_CASES = (
    (("[%8.3s][%-5s][%.0s]", "abcdef", "xy", "hidden"), b"[     abc][xy   ][]"),
    (("[%*.*s]", "-8", "3", "abcdef"), b"[abc     ]"),
    (("[%.*s]", "-1", "abcdef"), b"[abcdef]"),
    (("%d|%i|%06d|%-6d", "123456789", "-42", "-12", "12"),
     b"123456789|-42|-00012|12    "),
    (("%d", LONG_MIN), LONG_MIN.encode()),
    (("%u|%x|%X|%o", ULONG_MAX, "0xff", "255", "010"),
     ULONG_MAX.encode() + b"|ff|FF|10"),
    (("[%8.2f][%-8.3f][%.2e][%.3g]", "3.14159", "-2.5", "12.5", "1234"),
     b"[    3.14][-2.500  ][1.25e+01][1.23e+03]"),
    (("[%08.2f][%.0f][%F][%.2E][%.3G]", "3.25", "2.25", "1.5", "12.5", "1234"),
     b"[00003.25][2][1.500000][1.25E+01][1.23E+03]"),
    ((r"literal\n\101\0X\x42\y%%end\\",), b"literal\nA\0XB\\y%end\\"),
    ((r"%b", r"text\n\0101\101\41\0X\x42\y\tend\\"),
     b"text\nAA!\0XB\\y\tend\\"),
    (("--", "-literal"), b"-literal"),
    (("literal", "%s", "ignored"), b"literal"),
    (("trailing%",), b"trailing%"),
    (("[%s]", "one", "two", ""), b"[one][two][]"),
    (("[%s]", "%d", "tail"), b"[%d][tail]"),
    (("[%s]", r"\n", "tail"), b"[\\n][tail]"),
    (("[%s]", "plain", "%s", "tail"), b"[plain][%s][tail]"),
    (("%s", "span" * 2000), b"span" * 2000),
    (("literal" * 500,), b"literal" * 500),
    (("%5000s", "x"), b" " * 4999 + b"x"),
)


class PrintfExpansion(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="jash-perf-printf-")
        self.addCleanup(self.tmp.cleanup)
        self.env = {
            "HOME": self.tmp.name, "TMPDIR": self.tmp.name,
            "PATH": "/usr/bin:/bin", "LC_ALL": "C", "TERM": "dumb",
            "JASH_SIGIL": "0", "JASH_CHANGES": "0", "CHANGES": "0",
            "JASH_SAFE": "0", "JASH_BRAIN": "0", "JASH_MEMO": "0",
            "JASH_BRIEF": "0", "JASH_NUDGE": "0", "JASH_LOOPGUARD": "off",
            "JASH_DRIFT": "0", "JASH_RECALL_GUARD": "0",
            "JASH_RISK_CHECKPOINT": "0", "JASH_GUARD": "0",
            "JASH_SKIP_SELFCHECK": "1", "JASH_WORLD": "0",
            "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
            "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
        }

    def run_shell(self, code):
        p = subprocess.run([str(SHELL), "-c", code], capture_output=True,
                           cwd=self.tmp.name, env=self.env, timeout=10)
        return p.returncode, p.stdout, p.stderr

    def test_printf_values(self):
        for args, expected in PRINTF_CASES:
            with self.subTest(args=tuple(a[:80] for a in args)):
                self.assertEqual(self.run_shell("printf " + shlex.join(args)),
                                 (0, expected, b""))

    def test_format_is_independent_of_previous_arguments(self):
        for first, second in (("%d", "plain"), ("plain", "%d"), (r"\n", "text")):
            with self.subTest(first=first, second=second):
                code = "; ".join("printf " + shlex.join(("[%s]", arg))
                                 for arg in (first, second))
                self.assertEqual(self.run_shell(code),
                                 (0, f"[{first}][{second}]".encode(), b""))

    def test_q_round_trip(self):
        for value in ("", "plain/path", "two words", "it's quoted", "a\nb\t\\c", "100%"):
            with self.subTest(value=value):
                rc, quoted, err = self.run_shell("printf " + shlex.join(("%q", value)))
                self.assertEqual((rc, err), (0, b""))
                self.assertEqual(self.run_shell("printf '%s' " + quoted.decode()),
                                 (0, value.encode(), b""))

    def test_variable_capture(self):
        code = "printf -v out '[%8.3s]:%d:%.2f' abcdef -42 2.25; printf '%s' \"$out\""
        self.assertEqual(self.run_shell(code), (0, b"[     abc]:-42:2.25", b""))
        self.assertEqual(self.run_shell(
            "printf -v out -- '%s' '100%'; printf '%s' \"$out\""),
            (0, b"100%", b""))
        self.assertEqual(self.run_shell(
            r"printf -v out '%b' 'A\0B'; printf '%s' \"$out\"".replace(r'\"', '"')),
            (0, b"A", b""))

    def test_no_format_after_option_terminator(self):
        for code in ("printf --", "printf -v out --"):
            with self.subTest(code=code):
                rc, out, err = self.run_shell(code)
                self.assertGreater(rc, 0, "must fail normally, not crash")
                self.assertEqual(out, b"")
                self.assertTrue(err)

    @unittest.skipUnless(Path("/dev/full").exists(), "requires /dev/full")
    def test_write_error_propagates_through_function(self):
        for code in ("printf '%s' payload > /dev/full",
                     "f(){ printf '%s' payload; }; f > /dev/full"):
            with self.subTest(code=code):
                rc, out, err = self.run_shell(code)
                self.assertGreater(rc, 0)
                self.assertEqual(out, b"")
                self.assertTrue(err)

    def test_parameter_split_and_nesting(self):
        cases = (
            ("IFS=:; v=a:b; printf '[%s]' $v \"$v\"", b"[a][b][a:b]"),
            ("IFS=:; v='a b::c'; for x in $v; do printf '[%s]' \"$x\"; done",
             b"[a b][][c]"),
            ("IFS=; v='a b:c'; for x in $v; do printf '[%s]' \"$x\"; done",
             b"[a b:c]"),
            ("v=deep; printf '[%s]' \"${missing:-${other:-$v}}\"", b"[deep]"),
            ("IFS=:; v=a:b; copy=$v; printf '[%s]' $v \"$copy\"", b"[a][b][a:b]"),
            ("v=outer; printf '[%s]' \"$(v=inner; printf '%s' \"$v\")\" \"$v\"",
             b"[inner][outer]"),
        )
        for code, expected in cases:
            with self.subTest(code=code):
                self.assertEqual(self.run_shell(code), (0, expected, b""))

    def test_arithmetic_quotes_nesting_and_error(self):
        self.assertEqual(self.run_shell(
            "v=4; printf '%s|%s|%s' \"$((v * (2 + 3)))\" '$((1+2))' \"$((1 + (2*3)))\""),
            (0, b"20|$((1+2))|7", b""))
        for code in ("printf '%s' \"$((1/0))\"", "printf '%s' \"${missing?required}\""):
            with self.subTest(code=code):
                rc, out, err = self.run_shell(code)
                self.assertGreater(rc, 0)
                self.assertEqual(out, b"")
                self.assertTrue(err)


WRITER_HARNESS = r'''
#include <builtins/builtin.h>
#include <core/env.h>
#include <stdio.h>
#include <string.h>

static const char *mode;
static size_t calls, total;
long jash_stderr_writer(void *ctx, const char *data, size_t len) {
    (void)ctx;
    return (long)fwrite(data, 1, len, stderr);
}
long jash_writes(jash_writer_t w, void *ctx, const char *text) {
    return w(ctx, text, strlen(text));
}
int jash_env_set(jash_state_t *s, const char *key, const char *value) {
    (void)s; (void)key;
    fwrite(value, 1, strlen(value), stdout);
    return 0;
}
static long writer(void *ctx, const char *data, size_t len) {
    (void)ctx;
    calls++;
    if (!strcmp(mode, "zero")) return 0;
    if (!strcmp(mode, "error")) return -1;
    if (!strcmp(mode, "oversize")) return (long)len + 1;
    if (!strcmp(mode, "late") && total >= 5) return -1;
    if (!strcmp(mode, "late") && len > 5 - total) len = 5 - total;
    if ((!strcmp(mode, "short") || !strcmp(mode, "late")) && len > 3) len = 3;
    total += len;
    return (long)fwrite(data, 1, len, stdout);
}
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    mode = argv[1];
    int rc = jash_b_printf((jash_state_t *)1, writer, NULL, argc - 2, argv + 2);
    if (!strcmp(mode, "count")) fprintf(stderr, "%zu\n", calls);
    return rc;
}
'''


class PrintfWriter(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="jash-printf-writer-")
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary = Path(cls.tmp.name) / "printf-writer"
        command = [os.environ.get("CC", "cc"), "-std=gnu11", "-O2", "-Wall", "-Wextra",
                   "-fsanitize=undefined", "-fno-sanitize-recover=all", "-I" + str(ROOT / "c"),
                   "-x", "c", "-", str(ROOT / "c/builtins/printf.c"), "-o", str(cls.binary)]
        subprocess.run(command, input=WRITER_HARNESS.encode(), check=True,
                       capture_output=True, timeout=60)

    def run_printf(self, mode, args):
        p = subprocess.run([str(self.binary), mode, *args], capture_output=True, timeout=10)
        return p.returncode, p.stdout, p.stderr

    def test_values_with_partial_writes(self):
        for args, expected in PRINTF_CASES:
            with self.subTest(args=tuple(a[:80] for a in args)):
                self.assertEqual(self.run_printf("short", args), (0, expected, b""))

    def test_callback_failures(self):
        for mode in ("zero", "error", "oversize", "late"):
            with self.subTest(mode=mode):
                rc, out, err = self.run_printf(mode, ("prefix:%s:tail", "payload"))
                self.assertEqual(rc, 1)
                self.assertEqual(out, b"prefi" if mode == "late" else b"")
                self.assertEqual(err, b"jash: printf: write error\n")

    def test_q_with_partial_writes(self):
        for value, expected in (("", b"''"), ("plain/path", b"plain/path"),
                                ("it's quoted", b"'it'\\''s quoted'"),
                                ("a\nb\t\\c", b"'a\nb\t\\c'"), ("100%", b"'100%'")):
            with self.subTest(value=value):
                self.assertEqual(self.run_printf("short", ("%q", value)),
                                 (0, expected, b""))

    def test_contiguous_callback_spans(self):
        for args, expected in ((("literal run",), b"literal run"),
                               (("%s", "string span"), b"string span"),
                               (("%d", "123456789"), b"123456789"),
                               (("%.6f", "12.25"), b"12.250000")):
            with self.subTest(args=args):
                self.assertEqual(self.run_printf("count", args), (0, expected, b"1\n"))

    def test_variable_capture_and_missing_format(self):
        self.assertEqual(self.run_printf("short", ("-v", "out", "%s:%d", "100%", "-42")),
                         (0, b"100%:-42", b""))
        self.assertEqual(self.run_printf("short", ("-v", "out", "%b", r"A\0B")),
                         (0, b"A", b""))
        for args in (("--",), ("-v", "out", "--")):
            with self.subTest(args=args):
                rc, out, err = self.run_printf("short", args)
                self.assertGreater(rc, 0)
                self.assertEqual(out, b"")
                self.assertTrue(err)

EXPANSION_HARNESS = r'''
#include <parser/param_expand.h>
#include <parser/arithmetic.h>
#include <parser/eval.h>
#include <core/env.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

int jash_get_lineno(void) { return 1; }

int main(void) {
    jash_state_t *s = calloc(1, sizeof(*s));
    assert(s);
    for (int i = 0; i < JASH_ENV_HASH_SIZE; i++) s->env_hash[i] = -1;
    jash_env_array_index_init(s);
    jash_var_cache_init(s);
    assert(jash_env_set(s, "IFS", ":") == 0);
    assert(jash_env_set(s, "v", "a:b") == 0);
    const char *params[] = {"$v", "\"$v\"", "${missing:-${other:-$v}}", "'$v' $v"};
    char expected[1024];
    for (size_t i = 0; i < sizeof(params) / sizeof(params[0]); i++) {
        assert(jash_has_param_expansion(params[i]));
        strcpy(expected, jash_param_expand_split(params[i], s));
        assert(!strcmp(expected, jash_param_expand_split_known(params[i], s)));
        assert(!jash_param_expand_error());
        assert(!strcmp(jash_param_expand("$v", s), "a:b"));
    }
    assert(!strcmp(jash_param_expand_split_known("$v", s), "a b"));
    strcpy(expected, jash_param_expand_split_fields("$v", s));
    assert(!jash_ifs_field_active());
    jash_param_expand_split_known("$v", s);
    assert(!strcmp(expected, jash_param_expand_split_fields("$v", s)));
    assert(!jash_ifs_field_active());

    jash_param_expand_split_known("${missing?required}${other:-$v}", s);
    assert(jash_param_expand_error());
    assert(strstr(jash_param_expand_error_msg(), "required"));
    assert(jash_param_expand_split_known(NULL, s) == NULL);
    assert(jash_param_expand_error());
    jash_param_expand_split("literal", s);
    assert(jash_param_expand_error()); /* Preserve the checked split fast return. */
    jash_param_expand_split_known("$v", s);
    assert(!jash_param_expand_error() && !*jash_param_expand_error_msg());
    jash_param_expand("${missing?required}", s);
    assert(jash_param_expand_error());
    jash_param_expand("literal", s);
    assert(!jash_param_expand_error()); /* Plain checked expansion clears at top level. */

    const char *arith[] = {"$((2*(3+4)))", "'$((1+2))' $((3+4))",
                           "\\$((1+2)) \"$((3+4))\""};
    for (size_t i = 0; i < sizeof(arith) / sizeof(arith[0]); i++) {
        assert(jash_has_arithmetic(arith[i]));
        strcpy(expected, jash_expand_arithmetic(arith[i], s));
        assert(!strcmp(expected, jash_expand_arithmetic_known(arith[i], s)));
    }
    jash_arith_clear_error();
    assert(!strcmp(jash_expand_arithmetic_known("$((2*(3+4)))", s), "14"));
    jash_expand_arithmetic_known("$((1/0))", s);
    assert(jash_arith_error());
    jash_expand_arithmetic_known("$((1+2))", s);
    assert(jash_arith_error()); /* Neither entry owns the arithmetic error reset. */
    jash_expand_arithmetic("literal", s);
    assert(jash_arith_error());
    assert(jash_expand_arithmetic_known(NULL, s) == NULL);
    jash_arith_clear_error();
    assert(!jash_arith_error());
    free(s->var_cache);
    free(s);
    return 0;
}
'''


@unittest.skipUnless(sys.platform.startswith("linux"), "isolated GNU linker unit")
class ExpansionAPI(unittest.TestCase):
    def test_known_entry_contracts(self):
        with tempfile.TemporaryDirectory(prefix="jash-expansion-api-") as tmp:
            binary = Path(tmp) / "expansion-api"
            sources = ("parser/param_expand.c", "parser/arithmetic.c", "parser/quotes.c",
                       "parser/cmdsub.c", "core/env.c", "core/var_cache.c", "theme/io.c")
            command = [os.environ.get("CC", "cc"), "-std=gnu11", "-O2",
                       "-fsanitize=undefined", "-fno-sanitize-recover=all",
                       "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                       "-I" + str(ROOT / "c"), "-include", "sys/syscalls.h", "-x", "c", "-",
                       *[str(ROOT / "c" / source) for source in sources], "-o", str(binary)]
            p = subprocess.run(command, input=EXPANSION_HARNESS.encode(),
                               capture_output=True, timeout=60)
            self.assertEqual(p.returncode, 0, p.stderr.decode())
            p = subprocess.run([str(binary)], capture_output=True, timeout=10)
            self.assertEqual((p.returncode, p.stdout, p.stderr), (0, b"", b""))


if __name__ == "__main__":
    unittest.main()
