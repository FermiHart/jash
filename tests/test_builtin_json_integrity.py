#!/usr/bin/env python3
"""Semantic regressions from the 2026-10 builtin JSON/data audit."""
import base64
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BIN = Path(os.environ.get("JASH", ROOT / "build/jash")).resolve()


class Integrity(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="jash-builtin-json-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.home = self.root / "home"
        self.home.mkdir()
        self.env = dict(PATH="/usr/bin:/bin", HOME=str(self.home), LC_ALL="C.UTF-8",
                        JASH_QUIET="1", JASH_SIGIL="0", JASH_TIMELINE="0",
                        JASH_WORLD="0", JASH_MEMO="0", JASH_PREFETCH="0",
                        JASH_NUDGE="0", JASH_ADOPT="0", JASH_LOOPGUARD="off")

    def runj(self, command, **kw):
        return subprocess.run([str(BIN), "-c", command], cwd=self.root,
                              env=self.env, capture_output=True, timeout=20, **kw)

    def build_graph(self):
        (self.root / "fixture.c").write_text("int useful(void) { return 3; }\n")
        result = self.runj("graph build .")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_environment_escaped_value_is_not_truncated(self):
        self.env["AUDIT_VALUE"] = '"' * 3000
        result = self.runj("env --json")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["AUDIT_VALUE"], self.env["AUDIT_VALUE"])

    def test_environment_binary_value_is_reversible(self):
        self.env["AUDIT_VALUE"] = os.fsdecode(b"A\xffB")
        result = self.runj("env --json")
        self.assertEqual(result.returncode, 0, result.stderr)
        value = json.loads(result.stdout.decode("utf-8"))["AUDIT_VALUE"]
        self.assertEqual(value, {"encoding": "base64", "data": "Qf9C"})

    def test_pwd_binary_path_is_reversible(self):
        path = os.fsencode(self.root) + b"/path-\xff"
        os.mkdir(path)
        result = subprocess.run([str(BIN), "-c", "pwd --json"], cwd=path,
                                env=self.env, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        value = json.loads(result.stdout.decode("utf-8"))["cwd"]
        self.assertEqual(base64.b64decode(value["data"], validate=True), path)

    def test_structured_delivery_failures_change_exit_status(self):
        if not Path("/dev/full").exists():
            self.skipTest("requires Linux /dev/full")
        self.build_graph()
        for command in ("env --json", "pwd --json", "state --json",
                        "graph stats --json", "mem brief --json"):
            with self.subTest(command=command), open("/dev/full", "wb", buffering=0) as full:
                result = subprocess.run([str(BIN), "-c", command], cwd=self.root,
                                        env=self.env, stdout=full, stderr=subprocess.PIPE,
                                        timeout=20)
                self.assertEqual(result.returncode, 125, result.stderr)

    def test_budget_returns_complete_error_document(self):
        self.build_graph()
        self.env["JASH_LLM_BUDGET"] = "64"
        for command in ("state --json", "graph stats --json --budget 64",
                        "mem brief --json --budget 8"):
            with self.subTest(command=command):
                result = self.runj(command)
                value = json.loads(result.stdout)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(value["complete"])
                self.assertEqual(value["error"]["code"], "output_budget_exceeded")
                self.assertNotIn(b"context-governor", result.stdout)

    def test_graph_missing_query_is_escaped(self):
        self.build_graph()
        query = 'xx"yy\\zz'
        result = self.runj(shlex.join(["graph", "explain", query, "--json"]))
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(json.loads(result.stdout)["query"], query)

    def test_env_json_is_an_exact_selector(self):
        result = self.runj("env AUDIT_VALUE=--json /usr/bin/printenv AUDIT_VALUE")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b"--json\n")


if __name__ == "__main__":
    unittest.main()
