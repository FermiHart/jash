#!/usr/bin/env python3
"""The owned awk grammar emits native observations, exact bytes and honest limits."""

import base64
import json
import math
import os
import struct
import subprocess
import unittest

import native_data_fixture as fixture


def json_bytes(value):
    if isinstance(value, str):
        return value.encode()
    return base64.b64decode(value["data"] if "data" in value else value["b64"])


def number(value):
    if value.kind in ("i64", "u64"):
        return value.value
    assert value.kind == "record", value
    fields = value.value
    assert fields["representation"].value == "ieee754-binary64"
    binary = struct.unpack(">d", fields["bits"].value)[0]
    decimal = float(fields["decimal"].value)
    assert struct.pack(">d", binary) == struct.pack(">d", decimal)
    return binary


class AwkNative(fixture.NativeQueryCase):
    def file(self, name, data):
        path = os.fsencode(self.work) + b"/" + os.fsencode(name)
        with open(path, "wb") as stream:
            stream.write(data)
        return path

    def test_fields_raw_paths_and_json_projection_in_both_profiles(self):
        path = b"with space-\xff"
        self.file(path, b"alice:raw-\xfe\nbob:end\n")
        program = "{print $1,$2,NR,NF}"
        for strict in (False, True):
            for mode in ("--data", "--data-v2"):
                with self.subTest(strict=strict, mode=mode):
                    result = self.native(["awk", mode, "-F:", program, path], strict=strict)
                    self.assertEqual((result.count, result.state, result.errors), (2, 0, 0))
                    for index, row in enumerate(result.items):
                        self.scalar(row, "file", "path", path)
                        self.scalar(row, "phase", "symbol", "record")
                        self.scalar(row, "kind", "symbol", "print")
                        self.scalar(row, "numeric_model", "symbol", "binary64")
                        self.scalar(row, "complete", "bool", True)
                        self.scalar(row, "output_index", "u64", index)
                        fields = self.scalar(row, "fields", "list")
                        self.assertEqual([field.kind for field in fields], ["bytes", "bytes", "i64", "i64"])
                        self.assertEqual([field.value for field in fields],
                                         [b"alice", b"raw-\xfe", 1, 2] if index == 0 else [b"bob", b"end", 2, 2])
            legacy = self.run_command(["awk", "--json", "-F:", program, path], strict=strict)
            self.assertEqual(legacy.returncode, 0, legacy.stderr)
            rows = json.loads(legacy.stdout)
            self.assertEqual(json_bytes(rows[0]["fields"][1]), b"raw-\xfe")
            self.assertEqual(rows[1], {"fields": ["bob", "end", 2, 2]})

    def test_aggregates_preserve_binary64_without_repeated_decimal_rounding(self):
        self.file("precise", b"1000000\n1\n1\n1\n1\n1\n")
        program = "{s+=$1} END{print s}"
        exact = self.native(["awk", "--data", program, "precise"])
        self.assertEqual(number(exact.items[0].value["result"]), 1000005)
        self.scalar(exact.items[0], "kind", "symbol", "aggregate")
        legacy = self.run_command(["awk", "--json", program, "precise"])
        self.assertEqual(json.loads(legacy.stdout), {"result": 1000005})
        self.file("fraction", b"0.1\n0.2\n")
        fraction = self.native(["awk", "--data", program, "fraction"])
        self.assertEqual(number(fraction.items[0].value["result"]), 0.1 + 0.2)
        legacy = self.run_command(["awk", "--json", program, "fraction"])
        self.assertEqual(json.loads(legacy.stdout)["result"], 0.1 + 0.2)
        for value in ("-0.0", "18446744073709549568", "18446744073709551616", "-9223372036854775808"):
            with self.subTest(value=value):
                result = self.native(["awk", "--data", "-v", "n=" + value, "BEGIN{print n}"])
                actual = number(result.items[0].value["fields"].value[0])
                self.assertEqual(float(actual), float(value))
                if value == "-0.0":
                    self.assertEqual(math.copysign(1, actual), -1)

    def test_every_end_output_and_begin_printf_are_retained(self):
        self.file("input", b"5\n7\n")
        program = 'BEGIN{printf "start\\n"}{s+=$1} END{print s;printf "done:%d",NR;print "end"}'
        result = self.native(["awk", "--data", program, "input"])
        self.assertEqual(result.count, 4)
        self.assertEqual([row.value["phase"].value for row in result.items], ["begin", "end", "end", "end"])
        self.scalar(result.items[0], "text", "bytes", b"start\n")
        self.scalar(result.items[2], "text", "bytes", b"done:2")
        legacy = self.run_command(["awk", "--json", program, "input"])
        self.assertEqual(json.loads(legacy.stdout), [{"text": "start\n"}, {"fields": [12]},
                                                     {"text": "done:2"}, {"fields": ["end"]}])
        multi = self.run_command(["awk", "--json", 'END{print "one";print "two"}', "input"])
        self.assertEqual(json.loads(multi.stdout), [{"fields": ["one"]}, {"fields": ["two"]}])

    def test_long_print_printf_and_initial_value_are_not_silently_truncated(self):
        data = b"x" * 65535
        self.file("long", data + b"\n")
        result = self.native(["awk", "--data", '{printf "%s:%s",$0,$0}', "long"])
        self.scalar(result.items[0], "text", "bytes", data + b":" + data)
        legacy = self.run_command(["awk", "--json", '{printf "%s",$0}', "long"])
        self.assertEqual(json.loads(legacy.stdout), [{"text": data.decode()}])
        value = "v" * 8192
        initial = self.native(["awk", "--data", "-v", "message=" + value, "BEGIN{print message}"])
        self.assertEqual(initial.items[0].value["fields"].value[0].value, value.encode())

    def test_binary_overlong_fields_and_special_files_are_explicit_errors(self):
        self.file("binary", b"ok\x00hidden\n")
        self.file("long", b"x" * 65536)
        self.file("fields", b"x " * 1025 + b"\n")
        os.mkfifo(self.work / "fifo")
        for path, error in (("binary", "binary_input"), ("long", "line_limit"),
                            ("fields", "field_limit"), ("fifo", "not_regular"), ("missing", "open_failed")):
            with self.subTest(path=path):
                result = self.native(["awk", "--data", "{print $0}", path], rc=2)
                self.assertEqual((result.state, result.errors, result.reason), (2, 1, error))
                self.assertTrue(any(row.value["error"].value == error for row in result.items))
                legacy = self.run_command(["awk", "--json", "{print $0}", path])
                self.assertEqual(legacy.returncode, 2, legacy.stderr)
                self.assertFalse(json.loads(legacy.stdout)["complete"])

    def test_failed_aggregation_never_claims_partial_sum_as_complete(self):
        self.file("good", b"1\n2\n")
        result = self.native(["awk", "--data", "{s+=$1} END{print s}", "good", "missing"], rc=2)
        self.assertFalse(any(row.value["kind"].value == "aggregate" for row in result.items))
        self.assertEqual(result.state, 2)
        for value in ("inf", "nan", "1e999"):
            failed = self.native(["awk", "--data", "-v", "n=" + value, "BEGIN{print n}"], rc=2)
            self.assertEqual((failed.state, failed.reason), (2, "numeric_overflow"))
        huge = self.native(["awk", "--data", "-v", "n=1e100", 'BEGIN{printf "%d",n}'], rc=2)
        self.assertEqual(huge.reason, "integer_format_overflow")

    def test_pages_limits_and_no_match_have_honest_terminals(self):
        self.file("input", b"one\ntwo\nthree\n")
        page = self.native(["awk", "--data", "--offset", "1", "--limit", "1", "{print}", "input"])
        self.assertEqual((page.count, page.state, page.reason), (1, 3, "page_limit"))
        self.scalar(page.items[0], "output_index", "u64", 1)
        self.assertEqual(page.items[0].value["fields"].value[0].value, b"two")
        empty = self.native(["awk", "--data", "/absent/{print}", "input"])
        self.assertEqual((empty.count, empty.state, empty.errors), (0, 0, 0))
        self.file("many", b"row\n" * 4097)
        capped = self.native(["awk", "--data", "{print}", "many"], rc=75)
        self.assertEqual((capped.count, capped.state, capped.reason), (4096, 3, "query_collection_limit"))

    def test_invalid_grammar_modes_arguments_and_literal_option_files(self):
        self.file("--data", b"literal\n")
        plain = self.run_command(["awk", "{print}", "--data"])
        self.assertEqual((plain.returncode, plain.stdout), (0, b"literal\n"))
        typed = self.native(["awk", "--data-v2", "--", "{print}", "--data"])
        self.assertEqual(typed.items[0].value["fields"].value[0].value, b"literal")
        for args in (["--json", "{print}"], ["--offset", "bad", "{print}"],
                     ["--limit", "0", "{print}"], ["-f", "script"],
                     ["-v", "a=1", "-v", "b=2", "BEGIN{print a}"],
                     ["BEGIN{print 1.2.3}"], ["{if ($1) print}"], ["{print $99999999999999999999999}"],
                     ["-v", "bad name=1", "BEGIN{print 1}"]):
            with self.subTest(args=args):
                failed = self.native(["awk", "--data", *args], rc=2)
                self.assertEqual((failed.count, failed.state, failed.errors), (0, 2, 1))

    def test_stdin_empty_records_budget_and_sink_failure(self):
        result = self.native(["awk", "--data", "{print $0,NF,NR}"], stdin=b"\nhello\nlast")
        self.assertEqual([row.value["fields"].value[0].value for row in result.items], [b"", b"hello", b"last"])
        self.assertEqual([number(row.value["fields"].value[1]) for row in result.items], [0, 1, 1])
        self.env["JASH_LLM_BUDGET"] = "1"
        budget = self.native(["awk", "--data", 'BEGIN{print "bytes"}'])
        self.assertEqual(budget.count, 1)
        for mode in ("--data-v2", "--json"):
            failed = subprocess.run([str(fixture.JASH), "-c", f"awk {mode} 'BEGIN{{print 1}}' > /dev/full"],
                                    cwd=self.work, env=self.env, capture_output=True, timeout=20)
            self.assertEqual(failed.returncode, 125, failed.stderr)


if __name__ == "__main__":
    unittest.main()
