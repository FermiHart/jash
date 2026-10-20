#!/usr/bin/env python3
"""Literal ls operands remain intact through shell, exec-json and agentd."""

import base64
import json
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile
from types import SimpleNamespace
import unittest

from test_ls_data_wire import frames


ROOT = Path(__file__).resolve().parents[1]
JASH = Path(os.environ.get("JASH", ROOT / "build/jash")).resolve()
AGENT = Path(os.environ.get("JASH_AGENTD_BIN", ROOT / "build/jash-agentd")).resolve()
TRANSPORTS = ("shell", "exec-json", "agentd")


def path_bytes(value):
    if isinstance(value, str):
        return value.encode("utf-8")
    return base64.b64decode(value["data"], validate=True)


class LsArgvBoundaries(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="jash-ls-argv-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.work = self.root / "work"
        self.work.mkdir()
        home = self.root / "home"
        home.mkdir()
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("JASH_")}
        self.env.update(HOME=str(home), JASH_SIGIL="0", JASH_NUDGE="0",
                        JASH_AGENT="1", JASH_TIMELINE="0", JASH_MEMO="0",
                        JASH_BRIEF="0", JASH_ADOPT="0", JASH_LOOPGUARD="off",
                        JASH_WORLD="0", JASH_PREFETCH="0")
        self.serial = 0

    def run_ls(self, transport, *args):
        argv = ["ls", *args]
        if transport == "shell":
            return subprocess.run([str(JASH), "-c", shlex.join(argv)],
                                  cwd=self.work, env=self.env, capture_output=True,
                                  timeout=20)
        request = {"request_version": 2, "argv": argv, "cwd": str(self.work)}
        if transport == "exec-json":
            return subprocess.run([str(JASH), "--exec-json", json.dumps(request)],
                                  cwd=self.work, env=self.env, capture_output=True,
                                  timeout=20)
        self.serial += 1
        identifier = f"ls-argv-{self.serial}"
        del request["request_version"]
        request.update(id=identifier, protocol_version=2,
                       caps=["effect:read", "effect:mutate"])
        result = subprocess.run([str(AGENT)],
                                input=(json.dumps(request) + "\n").encode(),
                                cwd=self.work, env=self.env, capture_output=True,
                                timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        replies = [row for row in rows if row.get("id") == identifier]
        terminal = [row for row in replies if "rc" in row]
        self.assertEqual(len(terminal), 1, replies)
        output = {}
        for stream in ("stdout", "stderr"):
            chunks = [row for row in replies
                      if row.get("event") == "chunk" and row["stream"] == stream]
            output[stream] = b"".join(base64.b64decode(row["data_b64"], validate=True)
                                      for row in chunks)
            if stream + "_bytes" in terminal[0]:
                self.assertEqual(len(output[stream]), terminal[0][stream + "_bytes"])
        if "error" in terminal[0]:
            output["stderr"] += json.dumps(terminal[0]).encode()
        return SimpleNamespace(returncode=terminal[0]["rc"], **output)

    def test_json_preserves_whitespace_quotes_and_metacharacters(self):
        names = ["with space", "with\ttab", "with\nnewline", "with'quote",
                 'with"quote', "with\\slash", "$literal", "semi; touch canary"]
        for name in names:
            (self.work / name).touch()
        for transport in TRANSPORTS:
            with self.subTest(transport=transport):
                result = self.run_ls(transport, "--json", *names)
                self.assertEqual(result.returncode, 0, result.stderr)
                rows = [json.loads(line) for line in result.stdout.splitlines()]
                self.assertEqual([row["path"] for row in rows], names)
                self.assertFalse((self.work / "canary").exists())

    def test_json_multiple_directory_operands_keep_their_boundaries(self):
        for directory, member in (("first root", "alpha"), ("second\troot", "beta")):
            (self.work / directory).mkdir()
            (self.work / directory / member).touch()
        for transport in TRANSPORTS:
            with self.subTest(transport=transport):
                result = self.run_ls(transport, "--json", "first root", "second\troot")
                self.assertEqual(result.returncode, 0, result.stderr)
                rows = [json.loads(line) for line in result.stdout.splitlines()]
                self.assertEqual([row["path"] for row in rows],
                                 ["first root/alpha", "second\troot/beta"])

    def test_wire_modes_keep_roots_raw_bytes_and_one_terminal(self):
        directory = self.work / "with space"
        directory.mkdir()
        (directory / "alpha").touch()
        raw = os.fsencode(directory) + b"/raw-\xff"
        descriptor = os.open(raw, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        os.close(descriptor)
        for transport in TRANSPORTS:
            for flag, magic in (("--data", b"JTV1"), ("--data-v2", b"JTV2")):
                with self.subTest(transport=transport, flag=flag):
                    result = self.run_ls(transport, flag, "with space")
                    self.assertEqual(result.returncode, 0, result.stderr)
                    records = list(frames(result.stdout, magic))
                    self.assertEqual([tag for tag, _ in records],
                                     [b"D", b"G", b"I", b"I", b"F", b"T"])
                    items = [value for tag, value in records if tag == b"I"]
                    self.assertIn(b"with space/alpha", items[0])
                    self.assertIn(b"with space/raw-\xff", items[1])
                    self.assertEqual(records[-1][1][0], 0)
                    self.assertEqual(struct.unpack_from(">Q", records[-1][1], 8)[0], 2)
                    self.assertEqual(struct.unpack_from(">Q", records[-1][1], 24)[0], 0)

    def test_double_dash_keeps_option_shaped_operand_literal_in_every_mode(self):
        (self.work / "--json").mkdir()
        (self.work / "--json" / "member").touch()
        for transport in TRANSPORTS:
            for mode in ([], ["--json"], ["--data"], ["--data-v2"]):
                with self.subTest(transport=transport, mode=mode):
                    result = self.run_ls(transport, *mode, "--", "--json")
                    self.assertEqual(result.returncode, 0, result.stderr)
                    if not mode:
                        self.assertEqual(result.stdout, b"member\n")
                    elif mode == ["--json"]:
                        self.assertEqual(json.loads(result.stdout)["path"], "--json/member")
                    else:
                        magic = b"JTV2" if mode == ["--data-v2"] else b"JTV1"
                        items = [value for tag, value in frames(result.stdout, magic)
                                 if tag == b"I"]
                        self.assertEqual(len(items), 1)
                        self.assertIn(b"--json/member", items[0])

    def test_human_directory_and_file_operands_keep_spaces(self):
        (self.work / "directory name").mkdir()
        (self.work / "directory name" / "member name").touch()
        (self.work / "file name").touch()
        for transport in TRANSPORTS:
            with self.subTest(transport=transport):
                directory = self.run_ls(transport, "directory name")
                self.assertEqual(directory.returncode, 0, directory.stderr)
                self.assertEqual(directory.stdout, b"member name\n")
                file_result = self.run_ls(transport, "file name")
                self.assertEqual(file_result.returncode, 0, file_result.stderr)
                self.assertEqual(file_result.stdout, b"file name\n")

    def test_empty_operand_is_not_replaced_with_current_directory(self):
        (self.work / "unexpected").touch()
        for transport in TRANSPORTS:
            with self.subTest(transport=transport):
                result = self.run_ls(transport, "--json", "")
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, b"")
                self.assertNotIn(b"unexpected", result.stderr)

    def test_unknown_options_fail_before_any_output_header(self):
        for transport in TRANSPORTS:
            for flag in ("--json", "--data", "--data-v2"):
                with self.subTest(transport=transport, flag=flag):
                    result = self.run_ls(transport, flag, "--no-such-option")
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(result.stdout, b"")

    def test_exec_argv_preserves_non_utf8_operand_with_whitespace(self):
        name = b"raw space-\xff"
        descriptor = os.open(os.fsencode(self.work) + b"/" + name,
                             os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        os.close(descriptor)
        tagged = {"encoding": "base64", "data": base64.b64encode(name).decode("ascii")}
        for transport in ("exec-json", "agentd"):
            with self.subTest(transport=transport):
                result = self.run_ls(transport, "--json", tagged)
                self.assertEqual(result.returncode, 0, result.stderr)
                row = json.loads(result.stdout)
                self.assertEqual(path_bytes(row["path"]), name)
                self.assertEqual(path_bytes(row["name"]), name)


if __name__ == "__main__":
    unittest.main()
