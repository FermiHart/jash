#!/usr/bin/env python3
"""Time-travel queries expose immutable page/hunk observations without exec."""

import base64
import json
import os
import subprocess
import unittest

import native_data_fixture as fixture


class TreadNative(fixture.NativeQueryCase):
    def file(self, name, contents):
        path = os.fsencode(self.work) + b"/" + os.fsencode(name)
        with open(path, "wb") as stream:
            stream.write(contents)

    def test_current_page_exact_raw_path_bytes_in_both_profiles(self):
        name, contents = b"file \xff", b"binary\x00\xfe\n"
        self.file(name, contents)
        for strict in (False, True):
            for mode in ("--data", "--data-v2"):
                result = self.native(["tread", mode, name], strict=strict)
                self.assertEqual((result.count, result.state, result.errors), (1, 0, 0))
                self.scalar(result.items[0], "path", "path", name)
                self.scalar(result.items[0], "data", "bytes", contents)
                self.scalar(result.items[0], "checkpoint", "nothing")
                self.scalar(result.items[0], "snapshot", "bool", False)
                self.scalar(result.items[0], "total_bytes", "filesize", len(contents))
                self.scalar(result.items[0], "eof", "bool", True)

    def test_json_and_native_pages_share_snapshot_bytes_and_hash(self):
        self.file("input", b"abcd\x00\xffefgh")
        args = ["--offset", "2", "--limit", "4", "input"]
        native = self.native(["tread", "--data", *args])
        self.assertEqual((native.state, native.reason), (3, "page_limit"))
        result = self.run_command(["tread", "--json", *args])
        self.assertEqual(result.returncode, 0, result.stderr)
        row = json.loads(result.stdout)
        self.assertEqual(row["schema"], "jash.tread.v1")
        self.assertEqual(base64.b64decode(row["data"]["data"]), native.items[0].value["data"].value)
        self.assertEqual(base64.b64decode(row["page_blake3"]["data"]), native.items[0].value["page_blake3"].value)
        self.assertFalse(row["eof"])

    def history(self, command, expected):
        self.file("old", b"old\nlast\n")
        self.file("new", b"new\nlast")
        env = dict(self.env, JASH_TIMELINE="1")
        script = "cp old current >/dev/null; checkpoint base >/dev/null; cp new current >/dev/null; " + command
        result = subprocess.run([str(fixture.JASH), "-c", script], cwd=self.work, env=env,
                                capture_output=True, timeout=20)
        self.assertEqual(result.returncode, expected, result.stderr)
        self.assertEqual((self.work / "current").read_bytes(), b"new\nlast")
        return result.stdout

    def test_checkpoint_page_returns_historical_bytes_without_rewind(self):
        result = fixture.stream(self.history("tread --data-v2 current @base", 0))
        self.assertEqual((result.count, result.state), (1, 0))
        self.scalar(result.items[0], "checkpoint", "bytes", b"base")
        self.scalar(result.items[0], "path", "path", b"current")
        self.scalar(result.items[0], "data", "bytes", b"old\nlast\n")
        self.scalar(result.items[0], "snapshot", "bool", True)

    def test_tdiff_uses_native_diff_provider_and_logical_snapshot_path(self):
        result = fixture.stream(self.history("tdiff --data-v2 current @base", 1))
        self.assertEqual((result.state, result.errors, result.producer), (0, 0, 1))
        self.assertGreaterEqual(result.count, 1)
        for row in result.items:
            self.scalar(row, "checkpoint", "bytes", b"base")
            self.scalar(row.value["a"], "path", "path", b"current")
            self.scalar(row.value["b"], "path", "path", b"current")
        old = b"\n".join(line.value for row in result.items for line in row.value["a_lines"].value)
        self.assertIn(b"old", old)
        self.assertFalse(result.items[-1].value["b"].value["final_newline"].value)

    def test_tdiff_json_hunks_include_checkpoint_and_real_diff_status(self):
        rows = json.loads(self.history("tdiff --json current @base", 1))
        self.assertEqual(rows[0]["checkpoint"], "base")
        self.assertEqual(rows[0]["path"], "current")
        self.assertIn("old", rows[0]["a_lines"])
        self.assertIn("new", rows[0]["b_lines"])

    def test_checkpoint_read_and_parse_errors_are_explicit(self):
        self.file("input", b"data")
        for command, args, expected in (("tread", ["missing"], 1),
                                        ("tread", ["input", "@absent"], 1),
                                        ("tdiff", ["input"], 2),
                                        ("tread", ["--limit", "16385", "input"], 2),
                                        ("tread", ["--json", "input"], 2),
                                        ("tdiff", ["--unknown", "input", "@base"], 2)):
            with self.subTest(command=command, args=args):
                result = self.native([command, "--data", *args], rc=expected)
                self.assertEqual((result.count, result.state, result.errors), (0, 2, 1))
        store = self.root / "home/.jash"
        store.mkdir(exist_ok=True)
        checkpoint = store / "checkpoints.tsv"
        checkpoint.write_text("broken\tnot-a-depth\t0\t/work\n")
        checkpoint.chmod(0o600)
        result = self.native(["tread", "--data", "input", "@broken"], rc=1)
        self.assertEqual(result.reason, "checkpoint_read_failed")

    def test_literal_mode_path_budget_and_sink(self):
        self.file("--json", b"bytes")
        self.env["JASH_LLM_BUDGET"] = "1"
        result = self.native(["tread", "--data", "--", "--json"])
        self.scalar(result.items[0], "path", "path", b"--json")
        for mode in ("--data-v2", "--json"):
            result = subprocess.run([str(fixture.JASH), "-c", f"tread {mode} -- --json > /dev/full"],
                                    cwd=self.work, env=self.env, capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 125, result.stderr)

    def test_process_selector_validates_pid_and_refuses_unimplemented_variants(self):
        for selector in ("proc:", "proc:0", "proc:-1", "proc:2x", "proc:2147483648"):
            with self.subTest(selector=selector):
                result = self.native(["tread", "--data-v2", "@" + selector], rc=2)
                self.assertEqual(result.reason, "invalid_process_id")
        difference = self.native(["tdiff", "--data", "@proc:1"], rc=2)
        self.assertEqual(difference.reason, "process_diff_unavailable")
        page = self.native(["tread", "--data", "--limit", "1", "@proc:1"], rc=2)
        self.assertEqual(page.reason, "process_page_unavailable")


if __name__ == "__main__":
    unittest.main()
