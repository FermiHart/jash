"""? on admitted filesystem question must remain typed and read-only."""

import base64
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
JASH = Path(os.environ.get("JASH", str(ROOT / "build" / "jash")))


class AiTypedObservationTests(unittest.TestCase):
    def test_source_page_tool_is_text_friendly_and_rejects_escape(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-source-page-") as temporary:
            workspace = Path(temporary)
            project = workspace / "project"
            project.mkdir()
            (project / "fix.h").write_text(
                "static int answer(void) { return 42; }\n", encoding="utf-8")
            outside = workspace / "outside.h"
            outside.write_text("PRIVATE_BYTES", encoding="utf-8")
            (project / "link.h").symlink_to(outside)
            calls = [
                {"version": 1, "id": "read-1", "op": "fs.source.read",
                 "path": "fix.h", "offset": 0, "limit": 256},
                {"version": 1, "id": "deny-parent", "op": "fs.source.read",
                 "path": "../outside.h", "offset": 0, "limit": 256},
                {"version": 1, "id": "deny-link", "op": "fs.source.read",
                 "path": "link.h", "offset": 0, "limit": 256},
            ]
            script = workspace / "read.jsh"
            script.write_text("? on\n" + "".join(
                "? tool " + json.dumps(call, separators=(",", ":")) + "\n"
                for call in calls), encoding="utf-8")
            home = workspace / "home"
            home.mkdir()
            (home / ".jash").mkdir(mode=0o700)
            source = workspace / "module-sources" / "ai"
            source.mkdir(parents=True)
            for filename in ("ai_module.c", "manifest.toml"):
                shutil.copy2(ROOT / "modules/ai" / filename, source / filename)
            env = dict(os.environ, HOME=str(home),
                       JASH_SIGIL="0", JASH_NUDGE="0", JASH_BRIEF="0",
                       PATH="/usr/bin:/bin")
            env.pop("POOLSIDE_API_KEY", None)
            built = subprocess.run(
                ["bash", str(ROOT / "modules/build_module.sh"), str(source)],
                cwd=ROOT, env=env, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=120)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([str(JASH), str(script)], cwd=project,
                                    env=env, stdin=subprocess.DEVNULL,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    timeout=15)
            pages = [json.loads(line) for line in result.stdout.splitlines()
                     if line.startswith(b'{"schema":"jash.ai-tool-result.v1"')]
            self.assertEqual(len(pages), 1, result.stdout + result.stderr)
            page = pages[0]["result"]
            self.assertEqual(page["schema"], "jash.source-page.v1")
            self.assertIn("return 42", page["text"])
            self.assertTrue(page["eof"])
            self.assertEqual(base64.b64decode(page["data"]["data"]),
                             (project / "fix.h").read_bytes())
            self.assertEqual(result.stdout.count(b"tool observation unavailable"), 2)
            self.assertNotIn(b"PRIVATE_BYTES", result.stdout)

    def test_core_ai_controls_remain_local_after_module_load(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-controls-") as temporary:
            home = Path(temporary) / "home"
            script = Path(temporary) / "controls.jsh"
            script.write_text(
                "? on\n? status\n? auto\n? status\n"
                "? auto\n? status\n? off\n? status\n",
                encoding="utf-8")
            env = dict(os.environ, HOME=str(home), JASH_SIGIL="0",
                       JASH_NUDGE="0", JASH_BRIEF="0", PATH="/usr/bin:/bin")
            env.pop("POOLSIDE_API_KEY", None)
            result = subprocess.run([str(JASH), str(script)], cwd=ROOT,
                                    env=env, stdin=subprocess.DEVNULL,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            status = [line for line in result.stdout.splitlines()
                      if line.startswith(b"[ai] active=")]
            self.assertEqual(len(status), 4, result.stdout)
            self.assertIn(b"auto=0", status[0])
            self.assertIn(b"auto=1", status[1])
            self.assertIn(b"auto=0", status[2])
            self.assertIn(b"active=0", status[3])
            self.assertNotIn(b"Pensando", result.stdout)

    def test_tool_json_unicode_escapes_preserve_utf8_path(self):
        with tempfile.TemporaryDirectory(prefix="jash-tool-unicode-") as temporary:
            workspace = Path(temporary)
            observed = workspace / "pátio-🐻"
            observed.mkdir()
            (observed / "entrada").touch()
            valid = json.dumps({"version": 1, "id": "utf8", "op": "fs.count",
                                "root": str(observed)}, separators=(",", ":"))
            invalid = ('{"version":1,"id":"bad","op":"fs.count",'
                       '"root":"\\ud83d"}')
            script = workspace / "query.jsh"
            script.write_text(f"? on\n? tool {valid}\n? tool {invalid}\n",
                              encoding="utf-8")
            env = dict(os.environ, HOME=str(workspace / "home"),
                       JASH_SIGIL="0", JASH_NUDGE="0", JASH_BRIEF="0",
                       PATH="/usr/bin:/bin")
            env.pop("POOLSIDE_API_KEY", None)
            result = subprocess.run([str(JASH), str(script)], cwd=ROOT,
                                    env=env, stdin=subprocess.DEVNULL,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    timeout=15)
            answers = [json.loads(line) for line in result.stdout.splitlines()
                       if line.startswith(b'{"schema":"jash.ai-tool-result.v1"')]
            self.assertEqual(len(answers), 1, result.stdout + result.stderr)
            self.assertEqual(answers[0]["result"]["root"], str(observed))
            self.assertEqual(answers[0]["result"]["items_seen"], 1)
            self.assertIn(b"[ai] tool request denied", result.stdout)

    def test_versioned_tool_request_is_local_and_unknown_operation_is_denied(self):
        with tempfile.TemporaryDirectory() as temporary:
            workspace = Path(temporary)
            observed = workspace / "observed"
            observed.mkdir()
            (observed / "one").touch()
            (observed / "two").touch()
            target = workspace / "should-not-exist"
            script = workspace / "tool.jsh"
            calls = [
                {"version": 1, "id": "count-1", "op": "fs.count",
                 "root": str(observed), "type": "file"},
                {"version": 1, "id": "list-1", "op": "fs.list",
                 "root": str(observed)},
                {"version": 1, "id": "read-1", "op": "fs.observation.read",
                 "ref": "latest", "offset": 0, "limit": 16},
                {"version": 1, "id": "invalid-1", "op": "exec",
                 "root": f"$(touch {target})"},
            ]
            script.write_text("? on\n" + "".join(
                "? tool " + json.dumps(call, separators=(",", ":")) + "\n"
                for call in calls) +
                '? tool {"version":1,"version":1,"id":"bad","op":"fs.count",'
                f'"root":"{observed}"}}\n' +
                '? tool {"version":01,"id":"bad2","op":"fs.count",'
                f'"root":"{observed}"}}\n' + '? tool\n', encoding="utf-8")
            environment = dict(os.environ, HOME=str(workspace / "home"),
                               JASH_SIGIL="0", JASH_NUDGE="0", JASH_BRIEF="0",
                               PATH="/usr/bin:/bin")
            environment.pop("POOLSIDE_API_KEY", None)
            result = subprocess.run([str(JASH), str(script)], cwd=ROOT,
                                    env=environment, stdin=subprocess.DEVNULL,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    timeout=15)
            outputs = [json.loads(line) for line in result.stdout.splitlines()
                       if line.startswith(b'{"schema":"jash.ai-tool-result.v1"')]
            self.assertEqual(len(outputs), 3, result.stdout + result.stderr)
            self.assertEqual([output["id"] for output in outputs],
                             ["count-1", "list-1", "read-1"])
            self.assertEqual(outputs[0]["result"]["items_seen"], 2)
            self.assertEqual(outputs[1]["result"]["operation"], "fs.list")
            self.assertEqual(outputs[2]["result"]["bytes_returned"], 16)
            self.assertIn(b"[ai] tool request denied", result.stdout)
            self.assertEqual(result.stdout.count(b"[ai] tool request denied"), 4)
            self.assertNotIn(b"Pensando", result.stdout)
            self.assertFalse(target.exists())

    def test_count_files_reduces_native_stream_without_publishing_ref(self):
        with tempfile.TemporaryDirectory() as temporary:
            workspace = Path(temporary)
            observed = workspace / "observed"
            observed.mkdir()
            (observed / "first").touch()
            (observed / "second").touch()
            (observed / "folder").mkdir()
            script = workspace / "query.jsh"
            script.write_text(
                f"? on\n? count files --type=file --uid={os.getuid()} {observed}\n"
                "? observation read latest 0 64\n", encoding="utf-8")
            environment = dict(os.environ, HOME=str(workspace / "home"),
                               JASH_SIGIL="0", JASH_NUDGE="0", JASH_BRIEF="0",
                               PATH="/usr/bin:/bin")
            environment.pop("POOLSIDE_API_KEY", None)
            result = subprocess.run([str(JASH), str(script)], cwd=ROOT,
                                    env=environment, stdin=subprocess.DEVNULL,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    timeout=15)
            counts = [json.loads(line) for line in result.stdout.splitlines()
                      if line.startswith(b'{"operation":"fs.count"')]
            self.assertEqual(len(counts), 1, result.stdout + result.stderr)
            self.assertEqual(counts[0]["items_seen"], 2)
            self.assertTrue(counts[0]["complete"])
            self.assertEqual(counts[0]["filter"],
                             {"type": "file", "uid": os.getuid()})
            self.assertNotIn("observation_ref", counts[0])
            self.assertIn(b"observation ref unavailable", result.stdout)
            self.assertNotIn(b"Pensando", result.stdout)

    def test_filtered_file_observation_is_local_and_ref_matches_projection(self):
        with tempfile.TemporaryDirectory() as temporary:
            workspace = Path(temporary)
            observed = workspace / "observed"
            observed.mkdir()
            (observed / "file.txt").write_bytes(b"untouched")
            (observed / "directory").mkdir()
            (observed / "link").symlink_to("file.txt")
            script = workspace / "query.jsh"
            script.write_text(
                f"? on\n? tools\n? list files --type=file {observed}\n"
                "? observation read latest 0 64\n"
                f"? list files --type=file --uid={os.getuid()} {observed}\n"
                f"? list files --uid={os.getuid()} {observed}\n"
                f"? list files --uid=4294967296 {observed}\n"
                f"? list files --type=invalid {observed}\n",
                encoding="utf-8")
            environment = dict(os.environ, HOME=str(workspace / "home"),
                               JASH_SIGIL="0", JASH_NUDGE="0", JASH_BRIEF="0",
                               PATH="/usr/bin:/bin")
            environment.pop("POOLSIDE_API_KEY", None)
            result = subprocess.run([str(JASH), str(script)], cwd=ROOT,
                                    env=environment, stdin=subprocess.DEVNULL,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    timeout=15)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            lines = result.stdout.splitlines()
            observations = [json.loads(line) for line in lines
                            if line.startswith(b'{"operation":"fs.list"')]
            self.assertEqual(len(observations), 3, result.stdout)
            observation = observations[0]
            self.assertEqual(observation["items_seen"], 1)
            self.assertEqual(observation["rows_preview"], [{"name": "file.txt"}])
            self.assertTrue(observation["complete"])
            self.assertEqual(observation["filter"], {"type": "file"})
            self.assertEqual(observations[1]["filter"],
                             {"type": "file", "uid": os.getuid()})
            self.assertEqual(observations[1]["items_seen"], 1)
            self.assertEqual(observations[1]["rows_preview"],
                             [{"name": "file.txt"}])
            self.assertEqual(observations[2]["filter"], {"uid": os.getuid()})
            self.assertEqual(observations[2]["items_seen"], 3)
            pages = [json.loads(line) for line in lines
                     if line.startswith(b'{"schema":"jash.observation-page.v1"')]
            self.assertEqual(len(pages), 1, result.stdout)
            self.assertEqual(pages[0]["observation_ref"], observation["observation_ref"])
            self.assertEqual(base64.b64decode(pages[0]["data"]["data"])[:4], b"JTV1")
            self.assertIn(b"unsupported type filter", result.stdout)
            self.assertNotIn(b"Pensando", result.stdout)
            self.assertEqual((observed / "file.txt").read_bytes(), b"untouched")

    def test_list_files_has_bounded_evidence_and_no_execution(self):
        with tempfile.TemporaryDirectory() as temporary:
            workspace = Path(temporary)
            observed = workspace / "observed"
            observed.mkdir()
            (observed / "ordinary").touch()
            (observed / "$(touch UNEXPECTED_EXECUTION)").touch()
            before = set(os.listdir(observed))
            script = workspace / "query.jsh"
            script.write_text(f"? on\n? tools\n? list files {observed}\n"
                              "? observation read latest 0 64\n",
                              encoding="utf-8")
            environment = dict(os.environ, HOME=str(workspace / "home"),
                               JASH_SIGIL="0", JASH_NUDGE="0",
                               JASH_BRIEF="0", PATH="/usr/bin:/bin")
            environment.pop("POOLSIDE_API_KEY", None)
            result = subprocess.run([str(JASH), str(script)], cwd=ROOT,
                                    env=environment, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
            catalogs = [json.loads(line) for line in result.stdout.splitlines()
                        if line.startswith(b'{"version":1,"operations":')]
            self.assertEqual(len(catalogs), 1, result.stdout)
            self.assertEqual(catalogs[0]["operations"][0]["name"], "fs.list")
            self.assertEqual(catalogs[0]["operations"][0]["effect"], "read")
            self.assertEqual(catalogs[0]["repair_cell_schema"],
                             "jash.ai.cell.v1")
            observations = [json.loads(line) for line in result.stdout.splitlines()
                            if line.startswith(b'{"operation":"fs.list"')]
            self.assertEqual(len(observations), 1, result.stdout)
            observation = observations[0]
            self.assertTrue(observation["complete"])
            self.assertEqual(observation["items_seen"], 2)
            self.assertEqual(len(observation["content_hash"]), 64)
            self.assertTrue(observation["observation_ref"].startswith("o:"))
            self.assertGreater(observation["stored_bytes"], 0)
            pages = [json.loads(line) for line in result.stdout.splitlines()
                     if line.startswith(b'{"schema":"jash.observation-page.v1"')]
            self.assertEqual(len(pages), 1, result.stdout)
            self.assertEqual(pages[0]["observation_ref"],
                             observation["observation_ref"])
            self.assertEqual(base64.b64decode(pages[0]["data"]["data"])[:4],
                             b"JTV1")
            self.assertEqual(before, set(os.listdir(observed)))
            self.assertFalse((workspace / "UNEXPECTED_EXECUTION").exists())

            replay = workspace / "replay.jsh"
            replay.write_text(
                f"? on\n? observation read {observation['observation_ref']} 0 64\n",
                encoding="utf-8")
            separate = subprocess.run([str(JASH), str(replay)], cwd=ROOT,
                                      env=environment, stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE, timeout=15)
            self.assertNotIn(b'"schema":"jash.observation-page.v1"',
                             separate.stdout)
            self.assertIn(b"observation ref unavailable", separate.stdout)


if __name__ == "__main__":
    unittest.main()
