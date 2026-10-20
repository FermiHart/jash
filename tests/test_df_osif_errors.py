#!/usr/bin/env python3
"""df never replaces a requested JSON document with an OSIF error sentence."""

import base64
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
JASH = Path(os.environ.get("JASH", ROOT / "build/jash")).resolve()
AGENT = Path(os.environ.get("JASH_AGENTD_BIN", ROOT / "build/jash-agentd")).resolve()


class DfOsifErrors(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temporary = tempfile.TemporaryDirectory(prefix="jash-df-fault-hook-")
        cls.addClassCleanup(temporary.cleanup)
        cls.hook = Path(temporary.name) / "mounts.so"
        compiler = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run([*compiler, "-std=gnu11", "-shared", "-fPIC", "-Wall",
                        "-Wextra", "-Wpedantic", "-Werror",
                        str(ROOT / "tests/mount_table_faults.c"), "-ldl",
                        "-o", str(cls.hook)], check=True, capture_output=True)

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="jash-df-osif-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("JASH_")}
        self.env.update(HOME=str(self.root), JASH_AGENT="1", JASH_SIGIL="0",
                        JASH_NUDGE="0", JASH_TIMELINE="0", JASH_MEMO="0",
                        JASH_BRIEF="0", JASH_ADOPT="0", JASH_LOOPGUARD="off")
        self.env["LD_PRELOAD"] = str(self.hook)

    def assert_error(self, payload):
        document = json.loads(payload)
        self.assertEqual(document["schema"], "jash.builtin.error.v1")
        self.assertEqual(document["command"], "df")
        self.assertFalse(document["complete"])
        self.assertEqual(document["error"]["code"], "mount_table_read_failed")

    def test_json_modes_report_structured_provider_errors(self):
        for flag in ("--json", "--json-report"):
            with self.subTest(flag=flag):
                result = subprocess.run([str(JASH), "-c", "df " + flag],
                                        cwd=self.root, env=self.env,
                                        capture_output=True, timeout=20)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assert_error(result.stdout)

    def test_exec_json_and_strict_runtime_keep_the_error_document(self):
        for index, flag in enumerate(("--json", "--json-report")):
            with self.subTest(flag=flag):
                request = {"argv": ["df", flag], "cwd": str(self.root)}
                general = subprocess.run([str(JASH), "--exec-json", json.dumps(request)],
                                         cwd=self.root, env=self.env,
                                         capture_output=True, timeout=20)
                self.assertEqual(general.returncode, 1, general.stderr)
                self.assert_error(general.stdout)
                request.update(id=f"df-osif-{index}", protocol_version=2,
                               caps=["effect:read", "effect:mutate"])
                strict = subprocess.run([str(AGENT)],
                                        input=(json.dumps(request) + "\n").encode(),
                                        cwd=self.root, env=self.env,
                                        capture_output=True, timeout=20)
                self.assertEqual(strict.returncode, 0, strict.stderr)
                rows = [json.loads(line) for line in strict.stdout.splitlines()]
                replies = [row for row in rows if row.get("id") == request["id"]]
                terminal = [row for row in replies if "rc" in row]
                self.assertEqual(len(terminal), 1, replies)
                self.assertEqual(terminal[0]["rc"], 1, terminal)
                output = b"".join(base64.b64decode(row["data_b64"], validate=True)
                                  for row in replies if row.get("event") == "chunk"
                                  and row["stream"] == "stdout")
                self.assert_error(output)

    def test_error_document_survives_prose_budget_and_checks_failed_sink(self):
        self.env["JASH_LLM_BUDGET"] = "1"
        result = subprocess.run([str(JASH), "-c", "df --json-report"],
                                cwd=self.root, env=self.env,
                                capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assert_error(result.stdout)
        failed = subprocess.run([str(JASH), "-c", "df --json-report > /dev/full"],
                                cwd=self.root, env=self.env,
                                capture_output=True, timeout=20)
        self.assertEqual(failed.returncode, 125, failed.stderr)

    def test_text_error_retains_nonzero_status(self):
        result = subprocess.run([str(JASH), "-c", "df"], cwd=self.root,
                                env=self.env, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, b"df: cannot read mount table\n")


if __name__ == "__main__":
    unittest.main()
