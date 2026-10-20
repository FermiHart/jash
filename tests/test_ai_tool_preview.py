#!/usr/bin/env python3
"""AUTO tool display follows the agent-CLI rule: one summary line plus a
bounded head preview per action; the full body spills with a recall handle.
No timing gates: byte counts and file identity only."""

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
JASH = Path(os.environ.get("JASH", ROOT / "build/jash")).resolve()


class ToolPreviewTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="jash-ai-tool-preview-")
        self.addCleanup(tmp.cleanup)
        base = Path(tmp.name)
        self.home = base / "home"
        self.home.mkdir()
        (self.home / ".jashrc").touch()
        (self.home / ".jash").mkdir(mode=0o700)
        self.project = base / "project"
        self.project.mkdir()
        self.big = self.project / "BIG.txt"
        self.big.write_bytes(b"".join(
            b"line-%04d padding-xxxxxxxxxxxx\n" % i for i in range(3000)))
        (self.project / "SMALL.txt").write_text("hello\n", encoding="utf-8")
        bindir = base / "bin"
        bindir.mkdir()
        curl = bindir / "curl"
        curl.write_text(
            "#!/usr/bin/python3\nimport json\n"
            "print(json.dumps({'choices':[{'message':{'content':"
            "'```jash\\ncat BIG.txt\\n```\\n```jash\\ncat SMALL.txt\\n```'}}]}))\n",
            encoding="utf-8")
        curl.chmod(0o700)
        script = base / "query.jsh"
        script.write_text("? on\n? on auto\n? mostra arquivos\n",
                          encoding="utf-8")
        self.env = dict(
            os.environ, HOME=str(self.home), JASH_SIGIL="0", JASH_NUDGE="0",
            JASH_BRIEF="0", JASH_ADOPT="0", JASH_TIMELINE="0",
            JASH_LOOPGUARD="off", JASH_SKIP_SELFCHECK="1",
            POOLSIDE_API_KEY="test", TERM="dumb", LC_ALL="C",
            PATH=f"{bindir}:/usr/bin:/bin")
        self.install_module()
        proc = subprocess.run(
            [str(JASH), str(script)], cwd=self.project, env=self.env,
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, timeout=60)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.out = proc.stdout

    @classmethod
    def setUpClass(cls):
        cache = tempfile.TemporaryDirectory(prefix="jash-ai-preview-modules-")
        cls.addClassCleanup(cache.cleanup)
        base = Path(cache.name)
        home = base / "home"
        home.mkdir()
        (home / ".jash").mkdir(mode=0o700)
        cls.artifacts = {}
        environment = dict(os.environ, HOME=str(home))
        source = base / "module-sources" / "ai"
        source.mkdir(parents=True)
        for filename in ("ai_module.c", "manifest.toml"):
            shutil.copy2(ROOT / "modules" / "ai" / filename,
                         source / filename)
        built = subprocess.run(
            ["bash", str(ROOT / "modules/build_module.sh"), str(source)],
            cwd=ROOT, env=environment, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, timeout=120)
        if built.returncode:
            raise AssertionError(built.stderr.decode(errors="replace"))
        cls.artifacts["ai"] = home / ".jash/modules" / "ai.modjash"

    def install_module(self):
        modules = self.home / ".jash/modules"
        modules.mkdir(mode=0o700, exist_ok=True)
        installed = modules / "ai.modjash"
        shutil.copy2(self.artifacts["ai"], installed)
        installed.chmod(0o600)

    def test_big_action_is_previewed_with_recall_handle(self):
        starts = re.findall(rb"\[ai:auto\] \$ (.*)\r", self.out)
        self.assertEqual(starts, [b"cat BIG.txt", b"cat SMALL.txt"])
        end = re.search(
            rb"\[ai:auto\] \xe2\x86\x92 rc=0 \xc2\xb7 (\d+) bytes "
            rb"\xc2\xb7 (\d+) linhas \xc2\xb7 \+(\d+) omitidos "
            rb"\xe2\x86\x92 recall (\S+)\r", self.out)
        self.assertIsNotNone(end, self.out[-500:])
        total, omitted, handle = int(end.group(1)), int(end.group(3)), end.group(4)
        self.assertGreater(total, 2048)
        self.assertGreater(omitted, 0)
        # Head preview only: first data line shown, late lines never reach
        # the terminal.
        self.assertIn(b"line-0000", self.out)
        self.assertNotIn(b"line-0020", self.out)
        self.assertNotIn(b"line-2999", self.out)
        # The handle is real: the spill holds every byte the action emitted.
        spill = self.home / ".jash/overflow" / handle.decode()
        self.assertTrue(spill.is_file())
        self.assertEqual(spill.stat().st_size, total)
        recalled = subprocess.run(
            [str(JASH), "-c", f"recall {handle.decode()} | wc -c"],
            cwd=self.project, env=self.env, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        self.assertEqual(recalled.returncode, 0, recalled.stderr)
        self.assertEqual(int(recalled.stdout.strip()), total)

    def test_small_action_shows_fully_without_spill(self):
        end = re.search(
            rb"\[ai:auto\] \xe2\x86\x92 rc=0 \xc2\xb7 17 bytes "
            rb"\xc2\xb7 2 linhas\r", self.out)
        self.assertIsNotNone(end, self.out[-500:])
        self.assertIn(b"hello\n", self.out)
        self.assertNotIn(b"omitidos \xe2\x86\x92 recall", self.out[end.start():end.end()])
        spills = list((self.home / ".jash/overflow").glob("*.txt"))
        self.assertEqual(len(spills), 1, [p.name for p in spills])

    def test_terminal_stays_bounded(self):
        # Two actions, one 93 KB and one tiny: the terminal sees previews
        # and framing only, far below the old 16 KiB-per-round dumps.
        self.assertLess(len(self.out), 8192)


if __name__ == "__main__":
    unittest.main()
