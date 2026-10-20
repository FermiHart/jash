#!/usr/bin/env python3
"""Provider-reported usage and latency are surfaced as typed AI evidence."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
JASH = ROOT / "build/jash"


class ProviderUsage(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build_tmp = tempfile.TemporaryDirectory(prefix="jash-ai-usage-build-")
        base = Path(cls.build_tmp.name)
        cls.home = base / "home"
        cls.home.mkdir(mode=0o700)
        (cls.home / ".jash").mkdir(mode=0o700)
        source = base / "module-source" / "ai"
        source.mkdir(parents=True)
        for name in ("ai_module.c", "manifest.toml"):
            shutil.copy2(ROOT / "modules/ai" / name, source / name)
        env = {k: v for k, v in os.environ.items()
               if not k.startswith("JASH_") and k != "POOLSIDE_API_KEY"}
        env.update(HOME=str(cls.home), PATH="/usr/bin:/bin")
        built = subprocess.run(
            ["/usr/bin/bash", str(ROOT / "modules/build_module.sh"), str(source)],
            cwd=ROOT, env=env, capture_output=True, timeout=120)
        if built.returncode:
            raise AssertionError(built.stderr.decode(errors="replace"))
        cls.artifact = cls.home / ".jash/modules/ai.modjash"
        if not cls.artifact.is_file():
            raise AssertionError("AI module build did not publish its temporary artifact")

    @classmethod
    def tearDownClass(cls):
        cls.build_tmp.cleanup()

    def run_query(self, response):
        with tempfile.TemporaryDirectory(prefix="jash-ai-usage-run-") as directory:
            base = Path(directory)
            home = base / "home"
            (home / ".jash/modules").mkdir(parents=True, mode=0o700)
            shutil.copy2(self.artifact, home / ".jash/modules/ai.modjash")
            bindir = base / "bin"
            bindir.mkdir(mode=0o700)
            response_file = base / "provider-response.json"
            response_file.write_text(json.dumps(response), encoding="utf-8")
            calls = base / "provider-calls.log"
            curl = bindir / "curl"
            curl.write_text(
                "#!/usr/bin/python3\n"
                "import os, pathlib\n"
                "with open(os.environ['AI_USAGE_CALLS'], 'a', encoding='ascii') as f: f.write('call\\n')\n"
                "print(pathlib.Path(os.environ['AI_USAGE_RESPONSE']).read_text(encoding='utf-8'))\n",
                encoding="utf-8")
            curl.chmod(0o700)
            script = base / "query.jsh"
            script.write_text("? on\n? hello provider usage\n", encoding="utf-8")
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith("JASH_") and k != "POOLSIDE_API_KEY"}
            env.update(HOME=str(home), PATH=f"{bindir}:/usr/bin:/bin",
                       POOLSIDE_API_KEY="test", JASH_SIGIL="0",
                       JASH_AI_CELLS="1", JASH_NUDGE="0", JASH_BRIEF="0",
                       JASH_ADOPT="0", AI_USAGE_RESPONSE=str(response_file),
                       AI_USAGE_CALLS=str(calls))
            result = subprocess.run(
                [str(JASH), str(script)], cwd=base, env=env,
                stdin=subprocess.DEVNULL, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0,
                             result.stdout.decode(errors="replace") +
                             result.stderr.decode(errors="replace"))
            self.assertEqual(calls.read_text(encoding="ascii").splitlines(), ["call"])
            rows = [json.loads(line) for line in result.stdout.splitlines()
                    if line.startswith(b'{"schema":"jash.ai.cell.v1"')]
            usage = [row for row in rows if row.get("kind") == "usage"]
            self.assertEqual(len(usage), 1, rows)
            return usage[0]

    def test_provider_usage_counts_and_latency_are_typed(self):
        usage = self.run_query({
            "choices": [{"message": {"content": "fixture answer"}}],
            "usage": {"prompt_tokens": 11, "completion_tokens": 7, "total_tokens": 18},
        })
        self.assertEqual(usage["provider"], "poolside")
        self.assertEqual(usage["model"], "poolside/laguna-xs-2.1")
        self.assertEqual(usage["source"], "provider_response")
        self.assertTrue(usage["available"], usage)
        self.assertEqual((usage["prompt_tokens"], usage["completion_tokens"],
                          usage["total_tokens"]), (11, 7, 18))
        self.assertGreaterEqual(usage["latency_ms"], 0)

    def test_missing_or_inconsistent_usage_is_not_estimated(self):
        missing = self.run_query({
            "choices": [{"message": {"content": "no counters"}}]})
        self.assertFalse(missing["available"], missing)
        self.assertEqual(missing["reason"], "provider_usage_missing")

        inconsistent = self.run_query({
            "choices": [{"message": {"content": "bad counters"}}],
            "usage": {"prompt_tokens": 11, "completion_tokens": 7,
                      "total_tokens": 99},
        })
        self.assertFalse(inconsistent["available"], inconsistent)
        self.assertEqual(inconsistent["reason"], "provider_usage_invalid")


if __name__ == "__main__":
    unittest.main()
