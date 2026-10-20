#!/usr/bin/env python3
"""Recent AI turns persist privately and are supplied to the next session."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
JASH = ROOT / "build/jash"


class ConversationMemory(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build_tmp = tempfile.TemporaryDirectory(prefix="jash-ai-memory-build-")
        base = Path(cls.build_tmp.name)
        home = base / "home"
        home.mkdir(mode=0o700)
        (home / ".jash").mkdir(mode=0o700)
        source = base / "module-source" / "ai"
        source.mkdir(parents=True)
        for name in ("ai_module.c", "manifest.toml"):
            shutil.copy2(ROOT / "modules/ai" / name, source / name)
        environment = {k: v for k, v in os.environ.items()
                       if not k.startswith("JASH_") and k != "POOLSIDE_API_KEY"}
        environment.update(HOME=str(home), PATH="/usr/bin:/bin")
        built = subprocess.run(
            ["/usr/bin/bash", str(ROOT / "modules/build_module.sh"), str(source)],
            cwd=ROOT, env=environment, capture_output=True, timeout=120)
        if built.returncode:
            raise AssertionError(built.stderr.decode(errors="replace"))
        cls.module = home / ".jash/modules/ai.modjash"

    @classmethod
    def tearDownClass(cls):
        cls.build_tmp.cleanup()

    def make_home(self, base):
        home = base / "home"
        home.mkdir(mode=0o700)
        (home / ".jash").mkdir(mode=0o700)
        module_dir = home / ".jash/modules"
        module_dir.mkdir(mode=0o700)
        shutil.copy2(self.module, module_dir / "ai.modjash")
        (module_dir / "ai.modjash").chmod(0o600)
        return home

    def run_turn(self, base, home, capture, query, answer, memory_setting=None,
                 auto=False, after="", output_budget=None):
        bindir = base / "bin"
        bindir.mkdir(exist_ok=True)
        response_file = base / f"response-{len(capture.read_text().splitlines()) if capture.exists() else 0}.json"
        answers = answer if isinstance(answer, list) else [answer]
        response_lines = []
        for response in answers:
            provider_response = response if isinstance(response, dict) else {
                "choices": [{"message": {"content": response}}],
            }
            response_lines.append(json.dumps(provider_response))
        response_file.write_text("\n".join(response_lines) + "\n", encoding="utf-8")
        curl = bindir / "curl"
        curl.write_text(
            "#!/usr/bin/python3\n"
            "import json, os, sys\n"
            "arg = sys.argv[sys.argv.index('-d') + 1]\n"
            "request = json.load(open(arg[1:], encoding='utf-8'))\n"
            "capture = os.environ['AI_MEMORY_CAPTURE']\n"
            "try:\n"
            "    index = len(open(capture, encoding='utf-8').read().splitlines())\n"
            "except FileNotFoundError:\n"
            "    index = 0\n"
            "with open(capture, 'a', encoding='utf-8') as f:\n"
            "    f.write(json.dumps(request, ensure_ascii=False) + '\\n')\n"
            "responses = open(os.environ['AI_MEMORY_RESPONSE'], encoding='utf-8').read().splitlines()\n"
            "print(responses[min(index, len(responses) - 1)])\n",
            encoding="utf-8")
        curl.chmod(0o700)
        script = base / "query.jsh"
        script.write_text(f"? {'on auto' if auto else 'on'}\n? {query}\n{after}",
                          encoding="utf-8")
        environment = {k: v for k, v in os.environ.items()
                       if not k.startswith("JASH_") and k != "POOLSIDE_API_KEY"}
        environment.update(
            HOME=str(home), PATH=f"{bindir}:/usr/bin:/bin",
            JASH_SIGIL="0", JASH_NUDGE="0", JASH_BRIEF="0", JASH_ADOPT="0",
            POOLSIDE_API_KEY="test", AI_MEMORY_CAPTURE=str(capture),
            AI_MEMORY_RESPONSE=str(response_file))
        if memory_setting is not None:
            environment["JASH_AI_MEMORY"] = memory_setting
        if output_budget is not None:
            environment["JASH_AI_OUTPUT_BUDGET"] = str(output_budget)
        return subprocess.run([str(JASH), str(script)], cwd=base, env=environment,
                              stdin=subprocess.DEVNULL, capture_output=True,
                              timeout=30)

    def test_prior_turns_are_recalled_by_a_new_shell_process(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-memory-run-") as directory:
            base = Path(directory)
            home = self.make_home(base)
            capture = base / "provider-requests.jsonl"
            first_user = "Estou criando um simulador orbital e o resultado esperado é 42."
            first_ai = "Vou lembrar: simulador orbital, resultado esperado 42."

            first = self.run_turn(base, home, capture, first_user, first_ai)
            self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
            history = home / ".jash/ai_memory.log"
            self.assertTrue(history.is_file(), first.stdout + first.stderr)
            self.assertEqual(history.stat().st_mode & 0o777, 0o600)

            second = self.run_turn(base, home, capture,
                                   "Qual projeto e valor eu mencionei?",
                                   "Você mencionou o simulador orbital e 42.")
            self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
            requests = [json.loads(line) for line in capture.read_text(
                encoding="utf-8").splitlines()]
            system = requests[1]["messages"][0]["content"]
            self.assertIn("Recent conversation memory", system)
            self.assertIn(first_user, system)
            self.assertIn(first_ai, system)

    def test_empty_memory_context_does_not_claim_prior_recall(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-memory-empty-") as directory:
            base = Path(directory)
            home = self.make_home(base)
            capture = base / "provider-requests.jsonl"
            result = self.run_turn(base, home, capture, "Você lembra de mim?",
                                   "Ainda não há histórico local.")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            request = json.loads(capture.read_text(encoding="utf-8").splitlines()[0])
            system = request["messages"][0]["content"]
            self.assertIn("No prior conversation is saved", system)
            self.assertIn("Do not claim to remember previous sessions", system)

    def test_memory_can_be_disabled_without_writing(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-memory-disabled-") as directory:
            base = Path(directory)
            home = self.make_home(base)
            capture = base / "provider-requests.jsonl"
            result = self.run_turn(base, home, capture, "hello", "Hello.",
                                  memory_setting="0")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            request = json.loads(capture.read_text(encoding="utf-8").splitlines()[0])
            self.assertIn("Local conversation memory is disabled",
                          request["messages"][0]["content"])
            self.assertFalse((home / ".jash/ai_memory.log").exists())

    def test_imported_memory_notes_are_retrievable(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-memory-import-") as directory:
            base = Path(directory)
            home = self.make_home(base)
            memory = home / ".jash/ai_memory.log"
            memory.write_text("M\tImported note: previous project target was 42.\n",
                              encoding="utf-8")
            memory.chmod(0o600)
            capture = base / "provider-requests.jsonl"
            result = self.run_turn(base, home, capture, "What target did I mention?",
                                   "The target was 42.")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            request = json.loads(capture.read_text(encoding="utf-8").splitlines()[0])
            system = request["messages"][0]["content"]
            self.assertIn("Imported note: previous project target was 42.", system)

    def test_c_code_fence_uses_indentation_line_numbers_and_syntax_colors(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-code-render-") as directory:
            base = Path(directory)
            home = self.make_home(base)
            capture = base / "provider-requests.jsonl"
            answer = ("```c\n#include <stdio.h>\nint main(void) {\n"
                      "    printf(\"hello\");\n    return 0; // done\n}\n```")
            result = self.run_turn(base, home, capture, "show C code", answer)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn(b"001", result.stdout)
            self.assertIn(b"002", result.stdout)
            self.assertIn(b"\x1b[38;5;81m", result.stdout)
            self.assertIn(b"\x1b[38;5;114m", result.stdout)

    def test_python_code_fence_highlights_keywords_strings_and_comments(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-python-highlight-") as directory:
            base = Path(directory)
            home = self.make_home(base)
            capture = base / "provider-requests.jsonl"
            answer = ("```python\ndef greet(name):\n"
                      "    message = f\"hello {name}\" # greeting\n"
                      "    return message\n```")
            result = self.run_turn(base, home, capture, "show Python", answer)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn(b"\x1b[38;5;81m", result.stdout)  # keyword
            self.assertIn(b"\x1b[38;5;114m", result.stdout)  # string/comment
            self.assertIn(b"001", result.stdout)
            self.assertIn(b"return", result.stdout)
            self.assertIn(b"message", result.stdout)

    def test_makefile_code_fence_highlights_directives_variables_and_comments(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-make-highlight-") as directory:
            base = Path(directory)
            home = self.make_home(base)
            capture = base / "provider-requests.jsonl"
            answer = ("```makefile\n.PHONY: all\nCC ?= cc\n"
                      "all: app # default target\n\t$(CC) main.c -o $@\n```")
            result = self.run_turn(base, home, capture, "show Makefile", answer)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn(b"\x1b[38;5;81m", result.stdout)  # make directive
            self.assertIn(b"\x1b[38;5;141m", result.stdout)  # variable reference
            self.assertIn(b"\x1b[38;5;114m", result.stdout)  # comment
            self.assertIn(b".PHONY", result.stdout)
            self.assertIn(b"$(CC)", result.stdout)

    def test_auto_executes_structured_command_tool_call(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-structured-tool-") as directory:
            base = Path(directory)
            home = self.make_home(base)
            capture = base / "provider-requests.jsonl"
            marker = base / "structured-action-ran"
            response = {"choices": [{"message": {
                "content": None,
                "tool_calls": [{
                    "id": "call-1", "type": "function",
                    "function": {"name": "jash_command",
                                 "arguments": json.dumps({"command": f"touch {marker}"})},
                }],
            }}]}
            result = self.run_turn(base, home, capture, "create the marker",
                                   response, auto=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue(marker.is_file(), result.stdout.decode(errors="replace"))
            request = json.loads(capture.read_text(encoding="utf-8").splitlines()[0])
            self.assertEqual(request["tool_choice"], "auto")
            self.assertEqual(request["tools"][0]["function"]["name"], "jash_command")

    def test_auto_returns_bounded_tool_result_to_provider(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-tool-feedback-") as directory:
            base = Path(directory)
            home = self.make_home(base)
            capture = base / "provider-requests.jsonl"
            first = {"choices": [{"message": {
                "content": None,
                "tool_calls": [{"id": "feedback-1", "type": "function",
                    "function": {"name": "jash_command",
                        "arguments": json.dumps({"command": "printf feedback-marker"})}}],
            }}]}
            result = self.run_turn(base, home, capture, "inspect and report",
                                   [first, "The command returned feedback-marker."],
                                   auto=True)
            self.assertEqual(result.returncode, 0,
                             result.stdout.decode(errors="replace") +
                             result.stderr.decode(errors="replace"))
            self.assertIn(b"The command returned feedback-marker", result.stdout)
            requests = [json.loads(line) for line in capture.read_text(
                encoding="utf-8").splitlines()]
            self.assertEqual(len(requests), 2)
            feedback_prompt = requests[1]["messages"][1]["content"]
            self.assertIn("feedback-marker", feedback_prompt)
            self.assertIn("exit status", feedback_prompt)
            memory_lines = (home / ".jash/ai_memory.log").read_text(
                encoding="utf-8").splitlines()
            self.assertEqual(len(memory_lines), 2)
            self.assertNotIn("Tool results", "\n".join(memory_lines))

    def test_auto_feedback_rounds_have_a_hard_limit(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-tool-round-limit-") as directory:
            base = Path(directory)
            home = self.make_home(base)
            capture = base / "provider-requests.jsonl"
            repeat = {"choices": [{"message": {
                "content": None,
                "tool_calls": [{"id": "repeat-1", "type": "function",
                    "function": {"name": "jash_command",
                        "arguments": json.dumps({"command": "true"})}}],
            }}]}
            result = self.run_turn(base, home, capture, "keep going", repeat,
                                   auto=True)
            self.assertEqual(result.returncode, 0,
                             result.stdout.decode(errors="replace") +
                             result.stderr.decode(errors="replace"))
            requests = capture.read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(requests), 3)
            self.assertIn(b"feedback round limit", result.stdout.lower())

    def test_ai_response_output_budget_marks_truncation(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-output-budget-") as directory:
            base = Path(directory)
            home = self.make_home(base)
            capture = base / "provider-requests.jsonl"
            result = self.run_turn(base, home, capture, "long answer",
                                   "x" * 4000, output_budget=1024)
            self.assertEqual(result.returncode, 0,
                             result.stdout.decode(errors="replace") +
                             result.stderr.decode(errors="replace"))
            self.assertIn(b"AI response truncated", result.stdout)
            self.assertLess(len(result.stdout), 1600)

    def test_duplicate_read_only_inspections_run_once(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-dedupe-inspection-") as directory:
            base = Path(directory)
            home = self.make_home(base)
            capture = base / "provider-requests.jsonl"
            response = {"choices": [{"message": {
                "content": None,
                "tool_calls": [
                    {"id": "ls-1", "type": "function", "function": {
                        "name": "jash_command",
                        "arguments": json.dumps({"command": "ls --pretty"})}},
                    {"id": "ls-2", "type": "function", "function": {
                        "name": "jash_command",
                        "arguments": json.dumps({"command": "ls --pretty"})}},
                ],
            }}]}
            result = self.run_turn(base, home, capture, "inspect the directory",
                                   response, auto=True)
            self.assertEqual(result.returncode, 0,
                             result.stdout.decode(errors="replace") +
                             result.stderr.decode(errors="replace"))
            self.assertIn(b"duplicate inspection skipped", result.stdout.lower())

    def test_display_mode_never_executes_structured_tool_call(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-display-tool-") as directory:
            base = Path(directory)
            home = self.make_home(base)
            capture = base / "provider-requests.jsonl"
            marker = base / "must-not-run"
            response = {"choices": [{"message": {
                "content": None,
                "tool_calls": [{
                    "id": "call-1", "type": "function",
                    "function": {"name": "jash_command",
                                 "arguments": json.dumps({"command": f"touch {marker}"})},
                }],
            }}]}
            result = self.run_turn(base, home, capture, "create the marker",
                                   response, auto=False)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertFalse(marker.exists())
            request = json.loads(capture.read_text(encoding="utf-8").splitlines()[0])
            self.assertNotIn("tools", request)
            self.assertIn(b"display only", result.stdout.lower())

    def test_auto_creates_bounded_file_batch_and_refuses_overwrite(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-create-files-") as directory:
            base = Path(directory)
            home = self.make_home(base)
            capture = base / "provider-requests.jsonl"
            files = [
                {"path": "one.c", "content": "int one(void) { return 1; }\n"},
                {"path": "Makefile", "content": "all:\n\tcc one.c two.c -o app\n"},
            ]
            response = {"choices": [{"message": {
                "content": None,
                "tool_calls": [{"id": "files-1", "type": "function",
                    "function": {"name": "jash_create_files",
                                 "arguments": json.dumps({"files": files})}}],
            }}]}
            created = self.run_turn(base, home, capture, "create these files",
                                    response, auto=True,
                                    after="cat one.c\ncat Makefile\nrewind --session\n")
            self.assertEqual(created.returncode, 0,
                             created.stdout.decode(errors="replace") +
                             created.stderr.decode(errors="replace"))
            self.assertIn(files[0]["content"].encode(), created.stdout)
            self.assertIn(files[1]["content"].encode(), created.stdout)
            self.assertIn(b"created 2 new files", created.stdout)
            self.assertIn(b"rewind", created.stdout)
            self.assertFalse((base / "one.c").exists())
            self.assertFalse((base / "Makefile").exists())
            request = json.loads(capture.read_text(encoding="utf-8").splitlines()[0])
            tools = request["tools"]
            self.assertIn("jash_create_files",
                          [tool["function"]["name"] for tool in tools])

            protected = base / "protected.c"
            protected.write_text("keep me\n", encoding="utf-8")
            denied_files = [
                {"path": "should-not-appear.c", "content": "int x;\n"},
                {"path": "protected.c", "content": "overwritten\n"},
            ]
            denied_response = {"choices": [{"message": {
                "content": None,
                "tool_calls": [{"id": "files-2", "type": "function",
                    "function": {"name": "jash_create_files",
                                 "arguments": json.dumps({"files": denied_files})}}],
            }}]}
            denied = self.run_turn(base, home, capture, "create another batch",
                                   denied_response, auto=True)
            self.assertEqual(denied.returncode, 1,
                             denied.stdout.decode(errors="replace") +
                             denied.stderr.decode(errors="replace"))
            self.assertFalse((base / "should-not-appear.c").exists())
            self.assertEqual(protected.read_text(encoding="utf-8"), "keep me\n")


if __name__ == "__main__":
    unittest.main()
