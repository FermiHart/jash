"""JTV1 is an explicit byte adapter for the native ls data stream."""

import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


JASH = Path(os.environ.get("JASH", str(Path(__file__).resolve().parents[1] /
                                       "build" / "jash")))


def frames(payload, magic=b"JTV1"):
    assert payload[:4] == magic, payload[:32]
    offset = 4
    while offset < len(payload):
        assert offset + 9 <= len(payload)
        tag = payload[offset:offset + 1]
        length = struct.unpack_from(">Q", payload, offset + 1)[0]
        offset += 9
        assert offset + length <= len(payload)
        yield tag, payload[offset:offset + length]
        offset += length


class LsDataWireTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def run_ls(self, flags=""):
        return subprocess.run(
            [str(JASH), "-c", f"ls --data {flags} {self.root}"],
            env=dict(os.environ, JASH_SIGIL="0", JASH_NUDGE="0"),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def test_agent_job_stream_magic_is_not_typed_value_magic(self):
        # W15 reserves JDS1 for another packet layout. Never guess by magic.
        with self.assertRaises(AssertionError):
            list(frames(b"JDS1" + b"\0" * 32))

    def test_explicit_v2_export_preserves_item_logical_credits(self):
        (self.root / "a").touch()
        raw = b"non-utf8-\xff"
        fd = os.open(os.fsencode(self.root) + b"/" + raw,
                     os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        os.close(fd)
        result = subprocess.run(
            [str(JASH), "-c", f"ls --data-v2 {self.root}"],
            env=dict(os.environ, JASH_SIGIL="0", JASH_NUDGE="0"),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(result.returncode, 0, result.stderr)
        parsed = list(frames(result.stdout, b"JTV2"))
        self.assertEqual([tag for tag, _ in parsed],
                         [b"D", b"G", b"I", b"I", b"F", b"T"])
        items = [payload for tag, payload in parsed if tag == b"I"]
        for payload in items:
            self.assertEqual(payload[8:12], b"JDV1")
        self.assertIn(raw, items[1][8:])
        credits = sum(struct.unpack_from(">Q", payload)[0] for payload in items)
        self.assertEqual(credits, struct.unpack_from(">Q", parsed[-1][1], 16)[0])

    def test_v2_native_filter_and_exclusive_wire_versions(self):
        (self.root / "one").touch()
        (self.root / "directory").mkdir()
        environment = dict(os.environ, JASH_SIGIL="0", JASH_NUDGE="0")
        filtered = subprocess.run(
            [str(JASH), "-c", f"ls --data-v2 --where-type=file {self.root}"],
            env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(filtered.returncode, 0, filtered.stderr)
        parsed = list(frames(filtered.stdout, b"JTV2"))
        self.assertEqual([tag for tag, _ in parsed].count(b"I"), 1)
        self.assertIn(b"one", next(data for tag, data in parsed if tag == b"I"))
        conflict = subprocess.run(
            [str(JASH), "-c", f"ls --data --data-v2 {self.root}"],
            env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertNotEqual(conflict.returncode, 0)
        self.assertEqual(conflict.stdout, b"")

    def test_schema_items_terminal_and_raw_name(self):
        (self.root / "a").touch()
        name = b"raw-\xff"
        fd = os.open(os.fsencode(self.root) + b"/" + name,
                     os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        os.close(fd)
        result = self.run_ls()
        self.assertEqual(result.returncode, 0, result.stderr)
        parsed = list(frames(result.stdout))
        self.assertEqual([tag for tag, _ in parsed],
                         [b"D", b"G", b"I", b"I", b"F", b"T"])
        for tag, data in parsed[:-1]:
            self.assertTrue(data.startswith(b"JDV1" if tag == b"I" else b"JSC1"))
        self.assertIn(name, parsed[3][1])
        self.assertEqual(parsed[-1][1][0], 0)  # COMPLETE

    def test_empty_directory_terminal_without_items(self):
        parsed = list(frames(self.run_ls().stdout))
        self.assertEqual([tag for tag, _ in parsed], [b"D", b"G", b"F", b"T"])
        self.assertEqual(parsed[-1][1][0], 0)

    def test_missing_operand_is_an_item_error_and_nonzero_exit(self):
        missing = self.root / "missing"
        result = subprocess.run(
            [str(JASH), "-c", f"ls --data {missing}"],
            env=dict(os.environ, JASH_SIGIL="0", JASH_NUDGE="0"),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertNotEqual(result.returncode, 0)
        parsed = list(frames(result.stdout))
        self.assertEqual([tag for tag, _ in parsed],
                         [b"D", b"G", b"I", b"F", b"T"])
        self.assertIn(b"metadata", parsed[2][1])
        self.assertEqual(parsed[-1][1][0], 0)  # enumeration itself complete
        self.assertEqual(struct.unpack_from(">Q", parsed[-1][1], 24)[0], 1)

    def test_incompatible_flags_do_not_emit_a_partial_header(self):
        result = self.run_ls("--json")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, b"")

    def test_more_than_human_limit_still_emits_all_items(self):
        for index in range(520):
            (self.root / f"item-{index:04d}").touch()
        result = self.run_ls()
        self.assertEqual(result.returncode, 0, result.stderr)
        parsed = list(frames(result.stdout))
        self.assertEqual([tag for tag, _ in parsed].count(b"I"), 520)
        self.assertEqual(parsed[-1][0], b"T")
        self.assertEqual(struct.unpack_from(">Q", parsed[-1][1], 8)[0], 520)

    def test_native_type_filter_is_composed_before_wire_adapter(self):
        (self.root / "file.txt").touch()
        (self.root / "directory").mkdir()
        result = self.run_ls("--where-type=file")
        self.assertEqual(result.returncode, 0, result.stderr)
        parsed = list(frames(result.stdout))
        self.assertEqual([tag for tag, _ in parsed].count(b"I"), 1)
        self.assertIn(b"file.txt", next(data for tag, data in parsed if tag == b"I"))
        self.assertEqual(struct.unpack_from(">Q", parsed[-1][1], 8)[0], 1)

    def test_two_directory_operands_share_one_terminal(self):
        first, second = self.root / "first", self.root / "second"
        first.mkdir()
        second.mkdir()
        (first / "alpha").touch()
        (second / "beta").touch()
        result = subprocess.run(
            [str(JASH), "-c", f"ls --data {first} {second}"],
            env=dict(os.environ, JASH_SIGIL="0", JASH_NUDGE="0"),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(result.returncode, 0, result.stderr)
        parsed = list(frames(result.stdout))
        self.assertEqual([tag for tag, _ in parsed],
                         [b"D", b"G", b"I", b"I", b"F", b"T"])
        self.assertIn(b"alpha", parsed[2][1])
        self.assertIn(b"beta", parsed[3][1])
        self.assertEqual(struct.unpack_from(">Q", parsed[-1][1], 8)[0], 2)

    def test_multi_operand_filter_keeps_only_matching_rows(self):
        first, second = self.root / "first", self.root / "second"
        first.mkdir()
        second.mkdir()
        (first / "subdirectory").mkdir()
        (first / "a").touch()
        (second / "b").touch()
        result = subprocess.run(
            [str(JASH), "-c", f"ls --data --where-type=file {first} {second}"],
            env=dict(os.environ, JASH_SIGIL="0", JASH_NUDGE="0"),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(result.returncode, 0, result.stderr)
        parsed = list(frames(result.stdout))
        items = [data for tag, data in parsed if tag == b"I"]
        self.assertEqual(len(items), 2)
        self.assertIn(b"a", items[0])
        self.assertIn(b"b", items[1])
        self.assertEqual(struct.unpack_from(">Q", parsed[-1][1], 8)[0], 2)

    def test_missing_second_operand_is_one_item_error_not_lost_terminal(self):
        first = self.root / "first"
        first.mkdir()
        (first / "a").touch()
        result = subprocess.run(
            [str(JASH), "-c", f"ls --data {first} {self.root / 'missing'}"],
            env=dict(os.environ, JASH_SIGIL="0", JASH_NUDGE="0"),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertNotEqual(result.returncode, 0)
        parsed = list(frames(result.stdout))
        self.assertEqual([tag for tag, _ in parsed],
                         [b"D", b"G", b"I", b"I", b"F", b"T"])
        terminal = parsed[-1][1]
        self.assertEqual(terminal[0], 0)
        self.assertEqual(struct.unpack_from(">Q", terminal, 8)[0], 2)
        self.assertEqual(struct.unpack_from(">Q", terminal, 24)[0], 1)

    def test_file_operands_preserve_requested_order(self):
        first, second = self.root / "a", self.root / "z"
        first.touch()
        second.touch()
        result = subprocess.run(
            [str(JASH), "-c", f"ls --data {second} {first}"],
            env=dict(os.environ, JASH_SIGIL="0", JASH_NUDGE="0"),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(result.returncode, 0, result.stderr)
        items = [data for tag, data in frames(result.stdout) if tag == b"I"]
        self.assertEqual(len(items), 2)
        self.assertIn(os.fsencode(second), items[0])
        self.assertIn(os.fsencode(first), items[1])


if __name__ == "__main__":
    unittest.main()
