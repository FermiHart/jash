#!/usr/bin/env python3
"""REPL prediction lifecycle and non-interactive syscall scaling (no timing gate)."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
JASH = Path(os.environ.get("JASH", ROOT / "build/jash")).resolve()
STRACE = shutil.which("strace")


class PredictionScope(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="jash-prediction-scope-")
        self.addCleanup(tmp.cleanup)
        self.work = Path(tmp.name)
        self.home = self.work / "home"
        self.home.mkdir()
        (self.home / ".jashrc").touch()
        self.predictions = self.home / ".jash_predictions"
        self.env = {
            "HOME": str(self.home), "TMPDIR": str(self.work),
            "PATH": "/usr/bin:/bin", "LC_ALL": "C", "TERM": "dumb",
            "JASH_SIGIL": "0", "JASH_TIMELINE": "0", "JASH_CHANGES": "0",
            "JASH_MEMO": "0", "JASH_BRIEF": "0", "JASH_NUDGE": "0",
            "JASH_WORLD": "0", "JASH_LOOPGUARD": "off", "JASH_DRIFT": "0",
            "JASH_GUARD": "0", "JASH_GUARD_BYPASS": "0",
            "JASH_GUARD_BYPASS_STRICT": "0", "JASH_RECALL_GUARD": "0",
            "JASH_RISK_CHECKPOINT": "0", "JASH_SKIP_SELFCHECK": "1",
            "JASH_LLM_BUDGET": "0", "JASH_NO_SPINNER": "1",
        }

    def run_shell(self, args=(), stdin=b"", trace=False):
        command = [str(JASH), *args]
        if trace:
            command = [STRACE, "-f", "-e", "trace=%file", *command]
        return subprocess.run(command, input=stdin, cwd=self.work, env=self.env,
                              capture_output=True, timeout=30)

    def records(self):
        records = {}
        for line in self.predictions.read_text().splitlines():
            key, count, stamp = line.rsplit(" ", 2)
            command, context = key.split(":", 1)
            self.assertIn(context, [f"{self.work.name}:{period}"
                                    for period in ("am", "pm", "eve", "night")])
            self.assertGreater(int(stamp), 0)
            records[command] = records.get(command, 0) + int(count)
        return records

    def test_interactive_builtin_and_external_learning_survives_reload(self):
        # -i exercises the REPL consumer even with deterministic pipe input;
        # EOF exits without the interactive `exit` confirmation dialogue.
        proc = self.run_shell(["-i"], b"true; true; /usr/bin/true; false\n")
        self.assertEqual(proc.returncode, 1, proc.stderr)
        self.assertEqual(self.records(), {"true": 2, "/usr/bin/true": 1})
        proc = self.run_shell(["-i"], b"true\n")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(self.records(), {"true": 3, "/usr/bin/true": 1})

    def test_noninteractive_modes_do_not_train_repl_predictions(self):
        seed = f"true:{self.work.name}:am 7 1700000000\n".encode()
        script = self.work / "commands.sh"
        code = b"true; /usr/bin/true; printf complete\n"
        script.write_bytes(code)
        for args, stdin in ((["-c", code.decode()], b""),
                            ([str(script)], b""), ([], code)):
            with self.subTest(args=args):
                self.predictions.write_bytes(seed)
                proc = self.run_shell(args, stdin)
                self.assertEqual((proc.returncode, proc.stdout), (0, b"complete"),
                                 proc.stderr)
                self.assertEqual(self.predictions.read_bytes(), seed)

    @unittest.skipUnless(STRACE, "strace required for non-timing syscall gate")
    def test_command_lists_do_not_probe_timezone_per_command(self):
        for command, lengths in ((":", (8, 96)), ("/usr/bin/true", (4, 16))):
            with self.subTest(command=command):
                counts = []
                for length in lengths:
                    code = (command + ";") * length + "echo complete"
                    proc = self.run_shell(["-c", code], trace=True)
                    self.assertEqual((proc.returncode, proc.stdout), (0, b"complete\n"),
                                     proc.stderr)
                    counts.append(proc.stderr.count(b'"/etc/localtime"'))
                # Allow constant startup/platform overhead, never per-command IO.
                self.assertLessEqual(counts[1], counts[0] + 2, counts)


if __name__ == "__main__":
    unittest.main()
