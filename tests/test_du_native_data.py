#!/usr/bin/env python3
"""du observations share JSON/JDV units, bounded traversal and current inputs."""

import json
import os
import subprocess
import unittest

import native_data_fixture as fixture


class DuNative(fixture.NativeQueryCase):
    def file(self, name, data=b"bytes"):
        path = os.fsencode(self.work) + b"/" + os.fsencode(name)
        with open(path, "wb") as stream:
            stream.write(data)
        return path

    def test_text_depth_consumes_attached_or_separate_value_once(self):
        (self.work / "tree with space/sub/deep").mkdir(parents=True)
        (self.work / "other/sub").mkdir(parents=True)
        self.file("tree with space/sub/deep/file", b"x" * 4097)
        self.file("other/sub/file", b"y")

        def allocated_kib(relative):
            total = 0
            for directory, _, files in os.walk(self.work / relative):
                for path in [directory, *(os.path.join(directory, name) for name in files)]:
                    total += (os.lstat(path).st_blocks * 512 + 1023) // 1024
            return total

        shallow = {"tree with space", "tree with space/sub", "other", "other/sub"}
        cases = [(('-d1',), shallow), (('-d', '1'), shallow),
                 (('-kd1',), shallow), (('-kd', '1'), shallow),
                 (('-d1000',), shallow | {"tree with space/sub/deep"}),
                 (('-d', '1000'), shallow | {"tree with space/sub/deep"})]
        for strict in (False, True):
            for options, expected in cases:
                with self.subTest(strict=strict, options=options):
                    result = self.run_command(["du", *options, "--", "tree with space", "other"],
                                              strict=strict)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    rows = [line.split(b"\t", 1) for line in result.stdout.splitlines()]
                    self.assertEqual(len(rows), len(expected), rows)
                    self.assertTrue(all(len(row) == 2 for row in rows), rows)
                    self.assertEqual({os.fsdecode(row[1]) for row in rows}, expected)
                    for size, path in rows:
                        self.assertEqual(int(size), allocated_kib(os.fsdecode(path)))
            for options in (('-d',), ('-kd',)):
                with self.subTest(strict=strict, missing_depth=options):
                    result = self.run_command(["du", *options], strict=strict)
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertEqual(result.stdout, b"")
                    self.assertIn(b"requires a depth", result.stderr)

    def test_sparse_file_exact_bytes_and_raw_path_both_profiles(self):
        name = b"with space-\xff"
        path = self.file(name)
        with open(path, "r+b") as stream:
            stream.truncate(1024 * 1024 + 3)
        observed = os.lstat(path)
        for strict in (False, True):
            for flag in ("--data", "--data-v2"):
                with self.subTest(strict=strict, flag=flag):
                    result = self.native(["du", flag, name], strict=strict)
                    self.assertEqual((result.count, result.state, result.errors), (1, 0, 0))
                    row = result.items[0]
                    self.scalar(row, "path", "path", name)
                    self.scalar(row, "name", "path", name)
                    self.scalar(row, "apparent_bytes", "filesize", observed.st_size)
                    self.scalar(row, "allocated_bytes", "filesize", observed.st_blocks * 512)
                    self.scalar(row, "size_kib", "u64", (observed.st_size + 1023) // 1024)
                    self.scalar(row, "complete", "bool", True)

    def test_recursive_children_and_legacy_json_share_observations(self):
        (self.work / "tree").mkdir()
        (self.work / "tree/nested").mkdir()
        self.file("tree/small", b"abc")
        self.file("tree/nested/large", b"x" * 5000)
        os.symlink("../tree", self.work / "tree/loop")
        result = self.native(["du", "--data", "tree"])
        self.assertEqual({row.value["name"].value for row in result.items}, {b"small", b"nested", b"loop"})
        source = {row.value["path"].value: row for row in result.items}
        nested = source[b"tree/nested"]
        directory = os.lstat(self.work / "tree/nested")
        child = os.lstat(self.work / "tree/nested/large")
        self.scalar(nested, "apparent_bytes", "filesize", directory.st_size + child.st_size)
        self.scalar(nested, "allocated_bytes", "filesize", (directory.st_blocks + child.st_blocks) * 512)
        self.scalar(source[b"tree/loop"], "type", "symbol", "symlink")
        self.scalar(source[b"tree/loop"], "symlink_policy", "symbol", "physical")
        legacy = self.run_command(["du", "--json", "tree"])
        self.assertEqual(legacy.returncode, 0, legacy.stderr)
        rows = [json.loads(line) for line in legacy.stdout.splitlines()]
        for row in rows:
            native = source[row["path"].encode()]
            self.assertEqual(row["size_kb"], native.value["size_kib"].value)
            self.assertEqual(row["apparent_bytes"], native.value["apparent_bytes"].value)
            self.assertEqual(row["allocated_bytes"], native.value["allocated_bytes"].value)

    def test_summary_depth_and_totals_measure_full_subtrees(self):
        (self.work / "tree").mkdir()
        (self.work / "tree/sub").mkdir()
        self.file("tree/sub/a", b"a" * 2000)
        summary = self.native(["du", "--data", "-s", "tree"])
        depth = self.native(["du", "--data-v2", "-d0", "tree"])
        self.assertEqual((summary.count, depth.count), (1, 1))
        for key in ("apparent_bytes", "allocated_bytes", "size_kib"):
            self.assertEqual(summary.items[0].value[key].value, depth.items[0].value[key].value)
        total = self.native(["du", "--data", "-sc", "tree", "tree"])
        self.assertEqual(total.count, 3)
        self.scalar(total.items[-1], "is_total", "bool", True)
        self.scalar(total.items[-1], "apparent_bytes", "filesize", 2 * summary.items[0].value["apparent_bytes"].value)
        self.scalar(total.items[-1], "path", "nothing")

    def test_missing_paths_and_partial_totals_are_retained(self):
        self.file("present")
        result = self.native(["du", "--data", "-c", "missing", "present"], rc=1)
        self.assertEqual((result.count, result.errors, result.state), (3, 1, 0))
        failed = next(row for row in result.items if row.value["path"].value == b"missing")
        self.scalar(failed, "error", "symbol", "stat_failed")
        self.scalar(failed, "allocated_bytes", "nothing")
        self.scalar(result.items[-1], "complete", "bool", False)
        self.scalar(result.items[-1], "error", "symbol", "partial")

    def test_top_and_page_disclose_remaining_observations(self):
        (self.work / "tree").mkdir()
        for index in range(3):
            self.file(f"tree/{index}", b"x" * (index + 1) * 1024)
        top = self.native(["du", "--data", "--top", "1", "tree"])
        self.assertEqual((top.count, top.state, top.reason), (1, 3, "top_limit"))
        self.scalar(top.items[0], "path", "path", b"tree/2")
        page = self.native(["du", "--data", "--offset", "1", "--limit", "1", "tree"])
        self.assertEqual((page.count, page.state, page.reason), (1, 3, "page_limit"))
        self.scalar(page.items[0], "path", "path", b"tree/1")

    def test_hardlink_counting_policy_is_explicit_and_per_path(self):
        (self.work / "tree").mkdir()
        path = self.file("tree/a", b"x" * 2000)
        os.link(path, self.work / "tree/b")
        result = self.native(["du", "--data", "-s", "tree"])
        self.scalar(result.items[0], "hardlink_policy", "symbol", "per-path")
        self.scalar(result.items[0], "size_kib", "u64", 4)

    def test_cache_validates_current_leaf_metadata_in_one_session(self):
        self.file("input", b"short")
        proc = subprocess.Popen([str(fixture.AGENT)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, cwd=self.work, env=self.env, text=True, bufsize=1)
        try:
            json.loads(proc.stdout.readline())
            def request(ident):
                proc.stdin.write(json.dumps({"id": ident, "argv": ["du", "--json", "--cache", "input"],
                                            "caps": ["effect:read"]}) + "\n")
                proc.stdin.flush()
                response = json.loads(proc.stdout.readline())
                self.assertEqual(response["rc"], 0, response)
                return json.loads(response["stdout"])
            first = request("du-cache-a")
            self.file("input", b"longer" * 2000)
            second = request("du-cache-b")
            self.assertEqual(first["apparent_bytes"], 5)
            self.assertEqual(second["apparent_bytes"], 12000)
        finally:
            if proc.poll() is None: proc.kill()
            proc.communicate(timeout=10)

    def test_literal_option_paths_invalid_options_and_broken_sink(self):
        self.file("--json")
        self.scalar(self.native(["du", "--data", "--", "--json"]).items[0], "path", "path", b"--json")
        for bad in (("--json",), ("--unknown",), ("-d",), ("--limit", "0"), ("--top", "-1")):
            with self.subTest(bad=bad):
                failed = self.native(["du", "--data", *bad], rc=2)
                self.assertEqual((failed.count, failed.state), (0, 2))
        self.env["JASH_LLM_BUDGET"] = "1"
        self.native(["du", "--data", "--", "--json"])
        for flag in ("--data", "--json"):
            result = subprocess.run([str(fixture.JASH), "-c", f"du {flag} -- --json > /dev/full"],
                                    cwd=self.work, env=self.env, capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 125, result.stderr)


if __name__ == "__main__":
    unittest.main()
