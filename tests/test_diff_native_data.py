#!/usr/bin/env python3
"""Hunk values reconstruct file bytes, including non-UTF8 and final newlines."""

import base64
import json
import os
import random
import subprocess
import unittest

import native_data_fixture as fixture


class DiffNative(fixture.NativeQueryCase):
    def file(self, name, contents):
        path = os.fsencode(self.work) + b"/" + os.fsencode(name)
        with open(path, "wb") as output:
            output.write(contents)

    def reconstruct(self, source, stream):
        original = source.split(b"\n")
        if not source: original = []
        elif source.endswith(b"\n"): original.pop()
        result, position = [], 0
        final_newline = source.endswith(b"\n") or not source
        for item in stream.items:
            row = item.value
            before, after = row["a"].value, row["b"].value
            start, count = before["start"].value, before["count"].value
            old = [line.value for line in row["a_lines"].value]
            new = [line.value for line in row["b_lines"].value]
            self.assertEqual(original[start:start + count], old)
            self.assertEqual(len(new), after["count"].value)
            result.extend(original[position:start])
            self.assertEqual(after["start"].value, len(result))
            result.extend(new)
            position = start + count
            final_newline = after["final_newline"].value
        result.extend(original[position:])
        return b"\n".join(result) + (b"\n" if result and final_newline else b"")

    def test_hunks_reconstruct_curated_and_generated_files(self):
        pairs = [(b"", b""), (b"", b"new\n"), (b"old", b""),
                 (b"old\ntail\n", b"new\ntail"), (b"same", b"same\n"),
                 (b"a\n\xff\nb\n", b"a\n\xfe\nb\n")]
        randomizer = random.Random(73623)
        for _ in range(20):
            before = [randomizer.choice([b"a", b"b", b"c", b"d", b"\xff"]) for _ in range(randomizer.randrange(16))]
            after = [randomizer.choice([b"a", b"b", b"c", b"d", b"\xfe"]) for _ in range(randomizer.randrange(16))]
            a, b = b"\n".join(before), b"\n".join(after)
            if a and randomizer.randrange(2): a += b"\n"
            if b and randomizer.randrange(2): b += b"\n"
            pairs.append((a, b))
        for index, (a, b) in enumerate(pairs):
            with self.subTest(pair=index):
                self.file("before", a); self.file("after", b)
                stream = self.native(["diff", "--data", "before", "after"], rc=int(a != b))
                self.assertEqual((stream.state, stream.errors), (0, 0))
                self.assertEqual(self.reconstruct(a, stream), b)

    def test_native_modes_raw_path_and_strict_read_authority(self):
        self.file(b"before \xff", b"old\n")
        self.file(b"after \xfe", b"new\n")
        for strict in (False, True):
            for mode in ("--data", "--data-v2"):
                stream = self.native(["diff", mode, b"before \xff", b"after \xfe"], rc=1, strict=strict)
                self.scalar(stream.items[0].value["a"], "path", "path", b"before \xff")
                self.scalar(stream.items[0].value["b"], "path", "path", b"after \xfe")
                self.scalar(stream.items[0], "checkpoint", "nothing")

    def test_legacy_json_bytes_and_newline_metadata_are_reversible(self):
        self.file("before", b"old-\xff\nend\n")
        self.file("after", b"new-\xfe\nend")
        result = self.run_command(["diff", "--json", "before", "after"])
        self.assertEqual(result.returncode, 1, result.stderr)
        rows = json.loads(result.stdout)
        self.assertEqual(base64.b64decode(rows[0]["a_lines"][0]["data"]), b"old-\xff")
        self.assertEqual(base64.b64decode(rows[0]["b_lines"][0]["data"]), b"new-\xfe")
        self.assertTrue(rows[0]["a_final_newline"])
        self.assertFalse(rows[0]["b_final_newline"])

    def test_large_hunk_uses_bounded_measured_workspace(self):
        before, after = b"a" * 200000, b"b" * 200000
        self.file("before", before); self.file("after", after)
        result = self.native(["diff", "--data-v2", "before", "after"], rc=1)
        self.assertEqual(result.count, 1)
        self.assertEqual(self.reconstruct(before, result), after)

    def test_hunk_page_retains_global_positions_and_partial_terminal(self):
        self.file("before", b"a\nkeep\nb\nkeep\nc\n")
        self.file("after", b"A\nkeep\nB\nkeep\nC\n")
        result = self.native(["diff", "--data", "--offset", "1", "--limit", "1", "before", "after"], rc=1)
        self.assertEqual((result.count, result.state, result.reason), (1, 3, "page_limit"))
        self.scalar(result.items[0], "hunk_index", "u64", 1)
        self.scalar(result.items[0].value["a"], "start", "u64", 2)

    def test_errors_and_complexity_have_typed_terminal_and_no_fallback(self):
        self.file("regular", b"normal\n")
        self.file("late-binary", b"a" * 10000 + b"\x00")
        for args in (["missing", "regular"], ["late-binary", "regular"], [".", "regular"],
                     ["--unknown", "regular", "regular"], ["-U", "bad", "regular", "regular"],
                     ["--json", "regular", "regular"], ["--limit", "0", "regular", "regular"]):
            with self.subTest(args=args):
                result = self.native(["diff", "--data", *args], rc=2)
                self.assertEqual((result.count, result.state, result.errors), (0, 2, 1))
        self.file("complex-a", b"".join(f"a-{i}\n".encode() for i in range(3000)))
        self.file("complex-b", b"".join(f"b-{i}\n".encode() for i in range(3000)))
        limited = self.native(["diff", "--data", "complex-a", "complex-b"], rc=2)
        self.assertEqual((limited.state, limited.reason), (2, "complexity_limit"))

    def test_literal_modes_budget_and_checked_sink(self):
        self.file("--json", b"old\n"); self.file("after", b"new\n")
        self.env["JASH_LLM_BUDGET"] = "1"
        self.native(["diff", "--data", "--", "--json", "after"], rc=1)
        for mode in ("--data-v2", "--json"):
            result = subprocess.run([str(fixture.JASH), "-c", f"diff {mode} -- --json after > /dev/full"],
                                    cwd=self.work, env=self.env, capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 125, result.stderr)


if __name__ == "__main__":
    unittest.main()
