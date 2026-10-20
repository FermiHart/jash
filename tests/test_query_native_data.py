#!/usr/bin/env python3
"""Native query values, source observations, and terminal/framing contracts."""

import errno
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

import native_data_fixture as fixture


class NativeQueries(fixture.NativeQueryCase):
    @classmethod
    def setUpClass(cls):
        cls.hooks = tempfile.TemporaryDirectory(prefix="jash-stat-hook-")
        cls.addClassCleanup(cls.hooks.cleanup)
        cls.stat_hook = str(Path(cls.hooks.name) / "stat.so")
        subprocess.run(["cc", "-std=gnu11", "-shared", "-fPIC", "-Wall", "-Wextra",
                        "-Wpedantic", "-Werror", str(fixture.ROOT / "tests/query_stat_faults.c"),
                        "-ldl", "-o", cls.stat_hook], check=True, capture_output=True, timeout=30)

    def file(self, name, payload=b"a b\n"):
        path = os.fsencode(self.work) + b"/" + os.fsencode(name)
        with open(path, "wb") as out:
            out.write(payload)
        return path

    def test_stat_bytes_exact_quantities_and_times_both_profiles(self):
        name = b"with space-\xff"
        path = self.file(name, b"abc\0xyz\n")
        ns = 1700000000123456789
        os.utime(path, ns=(ns, ns))
        observed = os.lstat(path)
        for strict in (False, True):
            for flag in ("--data", "--data-v2"):
                with self.subTest(strict=strict, flag=flag):
                    result = self.native(["stat", flag, name], strict=strict)
                    self.assertEqual((result.count, result.errors, result.state), (1, 0, 0))
                    row = result.items[0]
                    self.scalar(row, "name", "path", name)
                    self.scalar(row, "size", "filesize", 8)
                    self.scalar(row, "inode", "u64", observed.st_ino)
                    self.scalar(row, "uid", "u64", observed.st_uid)
                    self.scalar(row, "gid", "u64", observed.st_gid)
                    self.scalar(row, "mode", "u64", observed.st_mode & 0o7777)
                    self.scalar(row, "mtime", "timestamp", ns)
                    self.scalar(row, "mtime_seconds", "i64", ns // 10**9)
                    self.scalar(row, "mtime_nsec", "u64", ns % 10**9)
                    self.scalar(row, "time_overflow", "bool", False)
                    self.scalar(row, "complete", "bool", True)

    def test_stat_symlinks_missing_rows_and_follow(self):
        target = b"target-\xff"
        self.file(target, b"payload")
        os.symlink(target, os.fsencode(self.work / "link"))
        rows = self.native(["stat", "--data", "link", "absent"], rc=1)
        self.assertEqual((rows.count, rows.errors, rows.state), (2, 1, 0))
        self.scalar(rows.items[0], "type", "symbol", "symlink")
        self.scalar(rows.items[0], "target", "path", target)
        self.scalar(rows.items[1], "error", "symbol", "stat_failed")
        self.scalar(rows.items[1], "errno", "u64", errno.ENOENT)
        self.scalar(rows.items[1], "size", "nothing")
        followed = self.native(["stat", "--data-v2", "-L", "link"])
        self.scalar(followed.items[0], "type", "symbol", "file")
        self.scalar(followed.items[0], "size", "filesize", 7)
        self.scalar(followed.items[0], "target", "nothing")

    def test_stat_large_integers_and_timestamp_boundaries(self):
        for name in ("synthetic-large", "timestamp-min", "timestamp-max", "negative-size"):
            self.file(name)
        self.env["LD_PRELOAD"] = self.stat_hook
        data = self.native(["stat", "--data-v2", "synthetic-large", "timestamp-min", "timestamp-max"])
        row = data.items[0]
        self.scalar(row, "size", "filesize", 9007199254740993)
        self.scalar(row, "inode", "u64", 2**64 - 1)
        self.scalar(row, "mtime", "nothing")
        self.scalar(row, "mtime_seconds", "i64", (2**63 - 1) // 10**9 + 1)
        self.scalar(row, "mtime_nsec", "u64", 987654321)
        self.scalar(row, "time_overflow", "bool", True)
        self.scalar(data.items[1], "mtime", "timestamp", -(2**63))
        self.scalar(data.items[2], "mtime", "timestamp", 2**63 - 1)
        negative = self.native(["stat", "--data", "negative-size"], rc=1)
        self.scalar(negative.items[0], "size", "nothing")
        self.scalar(negative.items[0], "error", "symbol", "negative_file_size")
        self.scalar(negative.items[0], "complete", "bool", False)
        self.assertEqual(negative.errors, 1)

    def test_stat_pages_disclose_incomplete_and_absolute_indices(self):
        for name in ("a", "b", "c"):
            self.file(name)
        page = self.native(["stat", "--data", "--offset", "1", "--limit", "1", "a", "b", "c"])
        self.assertEqual((page.count, page.state, page.reason, page.truncated), (1, 3, "page_limit", True))
        self.scalar(page.items[0], "name", "path", b"b")
        self.scalar(page.items[0], "operand_index", "u64", 1)
        last = self.native(["stat", "--data", "--offset", "2", "--limit", "1", "a", "b", "c"])
        self.assertEqual(last.state, 0)
        empty = self.native(["stat", "--data", "--offset", "3", "a", "b", "c"])
        self.assertEqual((empty.count, empty.state), (0, 0))

    def test_wc_stdin_bytes_and_counting_policy_both_profiles(self):
        payload = b"a\0b c\nlast"
        for strict in (False, True):
            with self.subTest(strict=strict):
                result = self.native(["wc", "--data"], strict=strict, stdin=payload)
                row = result.items[0]
                self.scalar(row, "bytes", "filesize", len(payload))
                self.scalar(row, "lines", "u64", 1)
                self.scalar(row, "words", "u64", 3)
                self.scalar(row, "source", "symbol", "stdin")
                self.scalar(row, "name", "nothing")
                self.scalar(row, "word_policy", "symbol", "byte-isspace")
        result = self.native(["wc", "--data-v2", "-mL"], stdin="é x\n".encode())
        row = result.items[0]
        self.scalar(row, "chars", "u64", 4)
        self.scalar(row, "max_line_length", "u64", 3)
        self.scalar(row, "bytes", "nothing")
        self.scalar(row, "lines", "nothing")
        self.scalar(row, "words", "nothing")
        self.scalar(row, "char_policy", "symbol", "utf8-leading-bytes")
        self.scalar(row, "max_line_unit", "symbol", "utf8-leading-bytes")

    def test_wc_operand_errors_and_total_are_explicit(self):
        self.file(b"a-\xff", b"a b\n")
        self.file("b", b"c")
        result = self.native(["wc", "--data-v2", b"a-\xff", "missing", "b"], rc=1)
        self.assertEqual((result.count, result.errors, result.state), (4, 1, 0))
        self.scalar(result.items[0], "name", "path", b"a-\xff")
        self.scalar(result.items[0], "bytes", "filesize", 4)
        self.scalar(result.items[1], "complete", "bool", False)
        self.scalar(result.items[1], "errno", "u64", errno.ENOENT)
        self.scalar(result.items[3], "source", "symbol", "total")
        self.scalar(result.items[3], "bytes", "filesize", 5)
        self.scalar(result.items[3], "words", "u64", 3)
        self.scalar(result.items[3], "complete", "bool", False)
        self.scalar(result.items[3], "error", "symbol", "partial")
        self.scalar(result.items[3], "name", "nothing")

    def test_wc_page_total_scope_is_observed_page(self):
        for name, payload in (("a", b"123"), ("b", b"12345"), ("c", b"1234567")):
            self.file(name, payload)
        result = self.native(["wc", "--data", "--offset", "1", "--limit", "1", "a", "b", "c"])
        self.assertEqual((result.count, result.state, result.reason), (2, 3, "page_limit"))
        self.scalar(result.items[0], "operand_index", "u64", 1)
        self.scalar(result.items[1], "total_scope", "symbol", "observed-page")
        self.scalar(result.items[1], "bytes", "filesize", 5)

    def test_df_quantities_native_inodes_and_total_both_profiles(self):
        observed = os.statvfs(self.work)
        for strict in (False, True):
            for flag in ("--data", "--data-v2"):
                with self.subTest(strict=strict, flag=flag):
                    result = self.native(["df", flag, "-i", "--total", ".", "."], strict=strict)
                    self.assertEqual((result.count, result.errors, result.state), (3, 0, 0))
                    first, second, total = result.items
                    self.scalar(first, "operand", "path", b".")
                    self.scalar(first, "device", "bytes")
                    self.scalar(first, "mountpoint", "path")
                    self.scalar(first, "filesystem_id", "u64")
                    self.scalar(first, "total_bytes", "filesize", observed.f_blocks * observed.f_frsize)
                    inode = first.value["inodes"]
                    self.assertEqual(inode.kind, "record")
                    self.scalar(inode, "total", "u64", observed.f_files)
                    for key in ("total_bytes", "used_bytes", "avail_bytes"):
                        self.scalar(total, key, "filesize", first.value[key].value + second.value[key].value)
                    self.scalar(total, "is_total", "bool", True)
                    self.scalar(total, "mountpoint", "nothing")

    def test_df_failed_lookup_is_a_retained_error_row(self):
        result = self.native(["df", "--data", "missing", "."], rc=1)
        self.assertEqual((result.count, result.errors, result.state), (2, 1, 0))
        self.scalar(result.items[0], "operand", "path", b"missing")
        self.scalar(result.items[0], "error", "symbol", "filesystem_lookup_failed")
        self.scalar(result.items[0], "errno", "u64", errno.ENOENT)
        self.scalar(result.items[0], "total_bytes", "nothing")
        self.scalar(result.items[1], "total_bytes", "filesize")

    def test_df_native_delta_preserves_signed_full_width_difference(self):
        result = self.run_command(["df", "--json-report", "--baseline", "baseline.bin", "."])
        self.assertEqual(result.returncode, 0, result.stderr)
        before = json.loads(result.stdout)["items"][0]
        original = (self.work / "baseline.bin").read_bytes()
        after = self.native(["df", "--data", "--delta", "baseline.bin", "."]).items[0]
        for key in ("total_bytes", "used_bytes", "avail_bytes"):
            difference = after.value[key].value - int(before[key])
            delta = after.value["delta"].value[key]
            self.scalar(delta, "magnitude", "filesize", abs(difference))
            self.scalar(delta, "negative", "bool", difference < 0)
        self.assertEqual((self.work / "baseline.bin").read_bytes(), original)

    def test_df_native_baseline_has_verified_publication_receipt(self):
        result = self.native(["df", "--data-v2", "--baseline", "new-baseline", "."])
        self.assertEqual((result.count, result.state), (2, 0))
        self.scalar(result.items[-1], "row_kind", "symbol", "baseline")
        receipt = result.items[-1].value["baseline"]
        self.scalar(receipt, "status", "symbol", "completed")
        self.scalar(receipt, "effect", "symbol", "changed")
        self.scalar(receipt, "verified", "bool", True)
        self.assertTrue((self.work / "new-baseline").is_file())

    def test_unknown_options_mode_conflicts_and_invalid_pages_are_terminals(self):
        self.file("a")
        for command in ("df", "stat", "wc"):
            for bad in (("--unexpected",), ("--json",), ("--offset", "-1"), ("--limit", "0")):
                with self.subTest(command=command, bad=bad):
                    result = self.native([command, "--data", *bad, "a"], rc=2)
                    self.assertEqual((result.count, result.state, result.errors), (0, 2, 1))

    def test_literal_option_paths_and_aliases(self):
        self.file("--data", b"bytes")
        for command, field in (("stat", "name"), ("wc", "name"), ("df", "operand")):
            for flag in ("--data", "--data-v2"):
                with self.subTest(command=command, flag=flag):
                    result = self.native([command, flag, "--", "--data"])
                    self.scalar(result.items[0], field, "path", b"--data")

    def test_budget_does_not_truncate_frames_and_sink_failure_is_125(self):
        self.file("a")
        self.env["JASH_LLM_BUDGET"] = "1"
        for command in ("df", "stat", "wc"):
            with self.subTest(command=command):
                self.native([command, "--data-v2", "a"])
                failed = subprocess.run([str(fixture.JASH), "-c", f"{command} --data-v2 a > /dev/full"],
                                        cwd=self.work, env=self.env, capture_output=True, timeout=20)
                self.assertEqual(failed.returncode, 125, failed.stderr)


if __name__ == "__main__":
    unittest.main()
