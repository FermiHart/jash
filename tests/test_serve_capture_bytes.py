#!/usr/bin/env python3
"""Legacy service capture must preserve NUL, UTF-8 and arbitrary binary bytes."""
import base64
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BIN = Path(os.environ.get("JASH", ROOT / "build/jash")).resolve()


class Capture(unittest.TestCase):
    def test_inline_payloads_are_exact(self):
        with tempfile.TemporaryDirectory(prefix="jash-capture-") as tmp:
            root = Path(tmp)
            home = root / "home"
            home.mkdir()
            env = dict(PATH="/usr/bin:/bin", HOME=str(home), JASH_SIGIL="0",
                       JASH_QUIET="1", JASH_TIMELINE="0", JASH_WORLD="0",
                       JASH_MEMO="0", JASH_NUDGE="0", JASH_ADOPT="0")
            payloads = [b"", b"plain\n", "Olá ∞ 😀".encode(), b"A\0B\n",
                        b"A\0B\xffC\n", bytes(range(256)), b"x" * 32768]
            for i, data in enumerate(payloads):
                with self.subTest(payload=i):
                    (root / "payload").write_bytes(data)
                    req = {"id": str(i), "argv": ["cat", "payload"]}
                    result = subprocess.run([str(BIN), "--serve"], env=env, cwd=root,
                                            input=(json.dumps(req)+"\n").encode(),
                                            capture_output=True, timeout=20)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    replies = [json.loads(line) for line in result.stdout.splitlines()]
                    reply = next(row for row in replies if row.get("id") == str(i))
                    self.assertEqual(reply["rc"], 0, reply)
                    value = reply["stdout"]
                    decoded = value.encode("utf-8") if isinstance(value, str) else base64.b64decode(value["data"], validate=True)
                    self.assertEqual(decoded, data)
                    self.assertEqual(reply["stdout_bytes"], len(data))
                    self.assertEqual(reply["stderr"], "")
                    self.assertEqual(reply["stderr_bytes"], 0)


if __name__ == "__main__":
    unittest.main()
