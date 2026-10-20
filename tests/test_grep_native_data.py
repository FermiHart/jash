#!/usr/bin/env python3
"""Native grep retains observed byte spans and explicit incomplete outcomes."""

import base64
import json
import os
import subprocess
import unittest

import native_data_fixture as fixture


def json_bytes(value):
    if isinstance(value, str):
        return value.encode()
    return base64.b64decode(value["data"] if "data" in value else value["b64"])


class GrepNative(fixture.NativeQueryCase):
    def file(self, name, contents):
        path = os.fsencode(self.work) + b"/" + os.fsencode(name)
        with open(path, "wb") as stream:
            stream.write(contents)
        return path

    def test_exact_spans_and_raw_bytes_in_both_profiles(self):
        name, pattern = b"space-\xff", b"hi-\xfe"
        self.file(name, b"before " + pattern + b" after\nnone\n" + pattern + b"\n")
        for strict in (False, True):
            for mode in ("--data", "--data-v2"):
                with self.subTest(strict=strict, mode=mode):
                    result = self.native(["grep", mode, "-F", pattern, name], strict=strict)
                    self.assertEqual((result.count, result.state, result.errors), (2, 0, 0))
                    for index, row in enumerate(result.items):
                        self.scalar(row, "file", "path", name)
                        self.scalar(row, "match", "bytes", pattern)
                        self.scalar(row, "line", "u64", 1 if index == 0 else 3)
                        self.scalar(row, "span_start", "u64", 7 if index == 0 else 0)
                        self.scalar(row, "span_end", "u64", 11 if index == 0 else 4)
                        self.scalar(row, "col", "u64", 8 if index == 0 else 1)
                        self.scalar(row, "unit", "symbol", "byte")
                        self.scalar(row, "complete", "bool", True)
                        self.scalar(row, "match_truncated", "bool", False)

    def test_long_match_preserves_all_native_bytes_and_json_cap(self):
        data = b"x" * 65535
        self.file("long", data + b"\n")
        result = self.native(["grep", "--data-v2", "-E", ".*", "long"])
        self.assertEqual(result.count, 1)
        self.scalar(result.items[0], "match", "bytes", data)
        self.scalar(result.items[0], "span_end", "u64", len(data))
        legacy = self.run_command(["grep", "--json", "-E", ".*", "long"])
        self.assertEqual(legacy.returncode, 0, legacy.stderr)
        rows = json.loads(legacy.stdout)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["match"], "x" * 4000)
        self.assertTrue(rows[0]["match_truncated"])
        self.assertEqual(rows[0]["span"], {"start": 0, "end": 65535})

    def test_zero_width_and_only_matching_spans(self):
        self.file("input", b"red red\nblue\n")
        result = self.native(["grep", "--data", "-o", "red", "input"])
        self.assertEqual(result.count, 2)
        self.assertEqual([row.value["span_start"].value for row in result.items], [0, 4])
        empty = self.native(["grep", "--data", "^", "input"])
        self.assertEqual(empty.count, 2)
        for row in empty.items:
            self.scalar(row, "match", "bytes", b"")
            self.scalar(row, "span_start", "u64", 0)
            self.scalar(row, "span_end", "u64", 0)
        at_end = self.native(["grep", "--data", "$", "input"])
        self.assertEqual([row.value["span_start"].value for row in at_end.items], [7, 4])

    def test_no_match_is_complete_predicate_status_one(self):
        self.file("input", b"red\n")
        for strict in (False, True):
            result = self.native(["grep", "--data", "blue", "input"], rc=1, strict=strict)
            self.assertEqual((result.count, result.state, result.errors, result.reason), (0, 0, 0, ""))

    def test_input_failures_retained_and_invalid_arguments_fail_before_output(self):
        self.file("binary", b"red\x00blue\n")
        self.file("overlong", b"x" * 65536)
        result = self.native(["grep", "--data", "red", "missing", "binary", "overlong"], rc=2)
        self.assertEqual((result.count, result.errors, result.state), (3, 3, 0))
        self.assertEqual({row.value["error"].value for row in result.items},
                         {"stat_failed", "binary_input", "line_exceeds_json_limit"})
        for row in result.items:
            self.scalar(row, "match", "nothing")
            self.scalar(row, "complete", "bool", False)
        for args in (["--bad", "red", "binary"], ["[", "binary"],
                     ["--offset", "bad", "red", "binary"], ["--json", "red", "binary"],
                     ["--include=" + "x" * 128, "red", "binary"]):
            with self.subTest(args=args):
                failed = self.native(["grep", "--data", *args], rc=2)
                self.assertEqual((failed.count, failed.state, failed.errors), (0, 2, 1))

    def test_page_and_match_limits_have_explicit_terminal(self):
        self.file("input", b"red\nred\nred\n")
        page = self.native(["grep", "--data", "--offset", "1", "--limit", "1", "red", "input"])
        self.assertEqual((page.count, page.state, page.reason), (1, 3, "page_limit"))
        self.scalar(page.items[0], "line", "u64", 2)
        limited = self.native(["grep", "--data", "-m1", "red", "input"])
        self.assertEqual((limited.count, limited.state, limited.reason), (1, 3, "match_limit"))
        self.file("many", b"red\n" * 4097)
        capped = self.native(["grep", "--data", "red", "many"], rc=75)
        self.assertEqual((capped.count, capped.state, capped.reason), (4096, 3, "query_collection_limit"))

    def test_recursive_search_handles_symlink_loops_and_filters(self):
        (self.work / "tree").mkdir()
        (self.work / "tree/sub").mkdir()
        self.file("tree/a.c", b"red\n")
        self.file("tree/b.py", b"red\n")
        self.file("tree/sub/c.c", b"red\n")
        os.symlink("..", self.work / "tree/sub/loop")
        result = self.native(["grep", "--data", "-r", "--include", "*.c", "red", "tree"])
        self.assertEqual({row.value["file"].value for row in result.items}, {b"tree/a.c", b"tree/sub/c.c"})
        self.assertEqual((result.state, result.errors), (0, 0))
        legacy = self.run_command(["grep", "--json", "-r", "--include", "*.c", "red", "tree"])
        self.assertEqual(legacy.returncode, 0, legacy.stderr)
        self.assertEqual({row["file"] for row in json.loads(legacy.stdout)}, {"tree/a.c", "tree/sub/c.c"})

    def test_literal_modes_pattern_options_and_checked_delivery(self):
        self.file("--json", b"red\n")
        literal = self.native(["grep", "--data", "red", "--", "--json"])
        self.scalar(literal.items[0], "file", "path", b"--json")
        self.file("input", b"--data\n--json\n")
        pattern = self.native(["grep", "--data", "-F", "-e", "--json", "input"])
        self.scalar(pattern.items[0], "match", "bytes", b"--json")
        plain = self.run_command(["grep", "-F", "-e", "--data", "input"])
        self.assertEqual((plain.returncode, plain.stdout), (0, b"--data\n"))
        self.env["JASH_LLM_BUDGET"] = "1"
        budget = self.native(["grep", "--data", "red", "--", "--json"])
        self.assertEqual(budget.count, 1)
        for mode in ("--data-v2", "--json"):
            failed = subprocess.run([str(fixture.JASH), "-c", f"grep {mode} -F -e --data input > /dev/full"],
                                    cwd=self.work, env=self.env, capture_output=True, timeout=20)
            self.assertEqual(failed.returncode, 125, failed.stderr)

    def test_zero_and_strict_numeric_match_limits(self):
        self.file("input", b"red\nred\n")
        for option in (("-m", "0"), ("-m0",), ("--max-count", "0"), ("--max-count=0",)):
            with self.subTest(option=option):
                result = self.native(["grep", "--data", *option, "red", "input"], rc=1)
                self.assertEqual((result.count, result.state, result.reason), (0, 3, "match_limit"))
        for args in (("-m", "1x"), ("-m", "-1"), ("-m", "2147483648"),
                     ("--max-count=garbage",), ("--max-count", ""),
                     ("--since", "1x"), ("--since=-1",), ("--since=18446744073709551616",)):
            with self.subTest(args=args):
                failed = self.native(["grep", "--data", *args, "red", "input"], rc=2)
                self.assertEqual((failed.count, failed.state, failed.reason), (0, 2, "invalid_numeric_option"))
                legacy = self.run_command(["grep", "--json", *args, "red", "input"])
                self.assertEqual(legacy.returncode, 2, legacy.stderr)
                self.assertEqual(json.loads(legacy.stdout)["error"]["code"], "invalid_numeric_option")

    def test_multiple_patterns_keep_original_grammar_and_leftmost_longest_spans(self):
        self.file("input", b"red BLUE\nblue red\nred|x\nnone\n")
        for strict in (False, True):
            with self.subTest(strict=strict):
                fixed = self.native(["grep", "--data", "-F", "-i", "-e", "red|x", "-e", "blue", "input"], strict=strict)
                self.assertEqual([row.value["match"].value for row in fixed.items], [b"BLUE", b"blue", b"red|x"])
                self.assertEqual([row.value["line"].value for row in fixed.items], [1, 2, 3])
                union = self.native(["grep", "--data", "-o", "-e", "re", "--regexp=red", "input"], strict=strict)
                self.assertEqual([row.value["match"].value for row in union.items], [b"red", b"red", b"red"])
                self.assertEqual([row.value["span_start"].value for row in union.items], [0, 5, 0])
        self.file("basic", b"aa\nbb\na\nb\n")
        basic = self.native(["grep", "--data", "-G", "-e", r"^\(a\)\1$", "-e", "^bb$", "basic"])
        self.assertEqual([row.value["match"].value for row in basic.items], [b"aa", b"bb"])
        newline = self.native(["grep", "--data", "-F", "-e", "aa\nbb", "basic"])
        self.assertEqual([row.value["match"].value for row in newline.items], [b"aa", b"bb"])
        legacy = self.run_command(["grep", "--json", "-F", "-i", "-e", "red|x", "-e", "blue", "input"])
        self.assertEqual(legacy.returncode, 0, legacy.stderr)
        self.assertEqual([row["match"] for row in json.loads(legacy.stdout)], ["BLUE", "blue", "red|x"])


if __name__ == "__main__":
    unittest.main()
