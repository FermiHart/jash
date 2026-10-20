#!/usr/bin/env python3
"""Discovery/page native values retain paths, units and bounded completion."""

import base64
import errno
import json
import os
import subprocess
import unittest

import native_data_fixture as fixture


class DiscoveryNative(fixture.NativeQueryCase):
    def file(self, name, payload=b"data"):
        path = os.fsencode(self.work) + b"/" + os.fsencode(name)
        with open(path, "wb") as out:
            out.write(payload)
        return path

    def test_glob_native_aliases_bytes_and_source_metadata_both_profiles(self):
        path = self.file(b"file space-\xff.c", b"bytes\x00")
        ns = 1700000000987654321
        os.utime(path, ns=(ns, ns))
        for strict in (False, True):
            for flag in ("--data", "--data-v2"):
                with self.subTest(strict=strict, flag=flag):
                    result = self.native(["glob", flag, b"file*\xff.c"], strict=strict)
                    self.assertEqual((result.count, result.errors, result.state), (1, 0, 0))
                    row = result.items[0]
                    self.scalar(row, "path", "path", b"file space-\xff.c")
                    self.scalar(row, "size_bytes", "filesize", 6)
                    self.scalar(row, "mtime", "timestamp", ns)
                    self.scalar(row, "mtime_nsec", "u64", ns % 10**9)
                    self.scalar(row, "type", "symbol", "file")

    def test_find_native_aliases_share_snapshot_and_do_not_follow_symlinks(self):
        (self.work / "tree").mkdir()
        self.file(b"tree/file-\xff", b"12345")
        (self.work / "outside").mkdir()
        self.file("outside/hidden", b"not traversed")
        os.symlink("../outside", self.work / "tree/link")
        for strict in (False, True):
            for flag in ("--data", "--data-v2"):
                with self.subTest(strict=strict, flag=flag):
                    result = self.native(["find", flag, "tree", "-mindepth", "1"], strict=strict)
                    self.assertEqual([item.value["path"].value for item in result.items],
                                     [b"tree/file-\xff", b"tree/link"])
                    self.scalar(result.items[0], "size_bytes", "filesize", 5)
                    self.scalar(result.items[1], "type", "symbol", "symlink")

    def test_discovery_pages_and_empty_results_are_honest(self):
        for name in ("a", "b", "c"):
            self.file(name)
        for command, operands in (("glob", ["*"]), ("find", [".", "-mindepth", "1", "-maxdepth", "1"])):
            with self.subTest(command=command):
                page = self.native([command, "--data", "--offset", "1", "--limit", "1", *operands])
                self.assertEqual((page.state, page.count, page.reason), (3, 1, "page_limit"))
                self.scalar(page.items[0], "match_index", "u64", 1)
                last = self.native([command, "--data", "--offset", "2", "--limit", "1", *operands])
                self.assertEqual(last.state, 0)
                empty = self.native([command, "--data", "--offset", "3", *operands])
                self.assertEqual((empty.state, empty.count), (0, 0))
        unmatched = self.native(["glob", "--data", "absent*"])
        self.assertEqual((unmatched.count, unmatched.state, unmatched.errors), (0, 0, 0))

    def test_find_errors_are_retained_separately_from_matching_rows(self):
        self.file("present")
        result = self.native(["find", "--data", "missing", "present"], rc=1)
        self.assertEqual((result.count, result.errors, result.state), (2, 1, 0))
        error = next(row for row in result.items if row.value["error"].kind != "nothing")
        self.scalar(error, "path", "path", b"missing")
        self.scalar(error, "errno", "u64", errno.ENOENT)
        self.scalar(error, "match_index", "nothing")
        self.scalar(error, "size_bytes", "nothing")

    def test_literal_option_paths_and_unsupported_predicates_never_execute(self):
        self.file("--json")
        for command in ("glob", "find"):
            with self.subTest(command=command):
                result = self.native([command, "--data", "--", "--json"])
                self.scalar(result.items[0], "path", "path", b"--json")
                failed = self.native([command, "--data", "--unexpected"], rc=2)
                self.assertEqual((failed.state, failed.count), (2, 0))
        rejected = self.native(["find", "--data", ".", "-exec", "touch", "should-not-exist", ";"], rc=2)
        self.assertEqual(rejected.state, 2)
        self.assertFalse((self.work / "should-not-exist").exists())

    def test_discovery_collection_limits_keep_a_partial_terminal(self):
        directory = self.work / "many"
        directory.mkdir()
        for index in range(4098):
            (directory / f"{index:05d}").touch()
        for command, operands in (("glob", ["many/*"]),
                                  ("find", ["many", "-mindepth", "1", "-maxdepth", "1"])):
            with self.subTest(command=command):
                result = self.native([command, "--data", "--limit", "256", *operands], rc=75)
                self.assertEqual((result.count, result.state, result.truncated), (256, 3, True))
                self.assertEqual(result.reason, "traversal_limit")

    def test_cat_native_page_matches_legacy_bytes_and_hash_both_profiles(self):
        name = b"page-\xff"
        payload = b"zero\x00\xfftail\n"
        self.file(name, payload)
        legacy = self.run_command(["cat", "--data", "--offset", "2", "--limit", "5", name])
        self.assertEqual(legacy.returncode, 0, legacy.stderr)
        old = json.loads(legacy.stdout)
        for strict in (False, True):
            with self.subTest(strict=strict):
                result = self.native(["cat", "--data-v2", "--offset", "2", "--limit", "5", name], strict=strict)
                self.assertEqual((result.count, result.state, result.reason), (1, 3, "page_limit"))
                row = result.items[0]
                self.scalar(row, "path", "path", name)
                self.scalar(row, "data", "bytes", payload[2:7])
                self.scalar(row, "bytes_returned", "filesize", 5)
                self.scalar(row, "total_bytes", "filesize", len(payload))
                self.scalar(row, "offset", "u64", 2)
                self.scalar(row, "eof", "bool", False)
                self.assertEqual(row.value["page_blake3"].value.hex(), old["page_blake3"])
                self.assertEqual(row.value["data"].value, base64.b64decode(old["data"]["data"]))

    def test_cat_eof_empty_file_input_errors_and_display_conflicts(self):
        self.file("empty", b"")
        empty = self.native(["cat", "--data-v2", "empty"])
        self.assertEqual(empty.state, 0)
        self.scalar(empty.items[0], "data", "bytes", b"")
        self.scalar(empty.items[0], "eof", "bool", True)
        for operands, status in ((["missing"], 1), (["."], 1),
                                 (["--offset", "1", "empty"], 2),
                                 (["--data", "empty"], 2), (["--json", "empty"], 2),
                                 (["-n", "empty"], 2), (["--limit", "0", "empty"], 2),
                                 (["--unknown", "empty"], 2)):
            with self.subTest(operands=operands):
                result = self.native(["cat", "--data-v2", *operands], rc=status)
                self.assertEqual(result.state, 2)
                self.assertEqual(result.errors, 1)

    def test_structured_budget_and_broken_sink(self):
        self.file("a")
        self.env["JASH_LLM_BUDGET"] = "1"
        for command, operands in (("glob", ["a"]), ("find", ["a"]), ("cat", ["a"])):
            with self.subTest(command=command):
                self.native([command, "--data-v2", *operands])
                result = subprocess.run([str(fixture.JASH), "-c", f"{command} --data-v2 a > /dev/full"],
                                        cwd=self.work, env=self.env, capture_output=True, timeout=20)
                self.assertEqual(result.returncode, 125, result.stderr)


if __name__ == "__main__":
    unittest.main()
