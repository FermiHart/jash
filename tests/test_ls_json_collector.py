"""Regression gates for the shared, byte-preserving ls JSON collector."""

import base64
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


JASH = Path(os.environ.get("JASH", str(Path(__file__).resolve().parents[1] /
                                       "build" / "jash")))


class LsJsonCollectorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def run_ls(self, *args):
        environment = dict(os.environ, JASH_SIGIL="0", JASH_NUDGE="0")
        return subprocess.run(
            [str(JASH), "-c", "ls --json " + " ".join(map(str, args))],
            cwd=self.root,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

    def test_order_hidden_and_no_synthetic_dot_entries(self):
        for name in ("z", "a", ".hidden", "b"):
            (self.root / name).touch()
        result = self.run_ls("-a", self.root)
        self.assertEqual(result.returncode, 0, result.stderr)
        names = [item["name"] for item in map(json.loads, result.stdout.splitlines())]
        self.assertEqual(names, [".hidden", "a", "b", "z"])

    def test_non_utf8_filename_is_reversible(self):
        name = b"raw-\xff"
        fd = os.open(os.fsencode(self.root) + b"/" + name,
                     os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        os.close(fd)
        result = self.run_ls(self.root)
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(len(rows), 1)
        encoded_name = rows[0]["name"]
        self.assertEqual(encoded_name["encoding"], "base64")
        self.assertEqual(base64.b64decode(encoded_name["data"]), name)
        encoded_path = rows[0]["path"]
        self.assertEqual(base64.b64decode(encoded_path["data"]),
                         os.fsencode(self.root) + b"/" + name)

    def test_more_than_512_entries_have_no_silent_cutoff(self):
        for index in range(520):
            (self.root / f"item-{index:04d}").touch()
        result = self.run_ls(self.root)
        self.assertEqual(result.returncode, 0, result.stderr)
        names = [item["name"] for item in map(json.loads, result.stdout.splitlines())]
        self.assertEqual(names, sorted(names))
        self.assertEqual(len(names), 520)

    def test_human_listing_reports_its_legacy_capacity(self):
        for index in range(520):
            (self.root / f"item-{index:04d}").touch()
        environment = dict(os.environ, JASH_SIGIL="0", JASH_NUDGE="0")
        result = subprocess.run(
            [str(JASH), "-c", "ls"], cwd=self.root, env=environment,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"incomplete", result.stderr)

    def test_human_symlink_directory_operand_keeps_existing_behavior(self):
        directory = self.root / "directory"
        directory.mkdir()
        (directory / "member").touch()
        link = self.root / "link"
        link.symlink_to(directory, target_is_directory=True)
        environment = dict(os.environ, JASH_SIGIL="0", JASH_NUDGE="0")
        result = subprocess.run(
            [str(JASH), "-c", "ls " + str(link)], cwd=self.root,
            env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b"member\n")

    def test_human_rejects_unrepresentable_path_instead_of_truncating(self):
        directory = self.root / ("d" * 240) / ("p" * 40)
        directory.mkdir(parents=True)
        (directory / ("f" * 240)).touch()
        environment = dict(os.environ, JASH_SIGIL="0", JASH_NUDGE="0")
        result = subprocess.run(
            [str(JASH), "-c", "ls " + str(directory)], cwd=self.root,
            env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"incomplete", result.stderr)

    def test_argument_longer_than_target_slab_is_rejected_before_lookup(self):
        result = self.run_ls("x" * 1100)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, b"")
        self.assertIn(b"path argument exceeds 1023 bytes", result.stderr)


if __name__ == "__main__":
    unittest.main()
