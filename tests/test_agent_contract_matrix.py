#!/usr/bin/env python3
"""W06 matrix is evidence, not a percentage inferred from registry presence."""
import json
import os
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Matrix(unittest.TestCase):
    def test_profiles_have_explicit_denominators_and_real_effects(self):
        for binary in (os.environ.get("JASH", str(ROOT / "build/jash")),
                       os.environ.get("JASH_AGENTD_BIN", str(ROOT / "build/jash-agentd"))):
            with self.subTest(binary=binary):
                p = subprocess.run(["python3", str(ROOT / "tools/agent_contract_matrix.py"), "--binary", binary],
                                   capture_output=True, timeout=120)
                d = json.loads(p.stdout)
                self.assertEqual(p.returncode, 0, (p.stderr, [r for r in d["operations"] if r["qualification"] == "failed"]))
                self.assertEqual(d["denominator"], len(d["operations"]))
                self.assertEqual(d["exercised"], 16)
                self.assertFalse(d["complete"])
                if d["profile"] == "agent-strict":
                    self.assertTrue(d["lifecycle_observations"]["exact_replay"])
                    self.assertTrue(d["lifecycle_observations"]["effect_once"])
                    self.assertTrue(d["lifecycle_observations"]["conflict_denied"])
                self.assertTrue(any(r["qualification"] == "unqualified" for r in d["operations"]))
                for row in d["operations"]:
                    if row["observations"]: self.assertTrue(all(row["observations"].values()))


if __name__ == "__main__": unittest.main()
