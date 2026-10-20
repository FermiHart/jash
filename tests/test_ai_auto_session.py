"""AUTO runs without approval; its checkpoint covers only journaled effects."""

import errno
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
JASH = Path(os.environ.get("JASH", str(ROOT / "build" / "jash")))


class AutoSessionTests(unittest.TestCase):
    def test_repair_hunk_in_larger_header_has_full_hash_and_undo(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-hunk-large-") as directory:
            base = Path(directory)
            home = base / "home"
            home.mkdir()
            (home / ".jash").mkdir(mode=0o700)
            project = base / "project"
            project.mkdir()
            (project / "broken.c").write_text(
                '#include "fix.h"\nint main(void) { return answer(); }\n',
                encoding="utf-8")
            source = project / "fix.h"
            original = (b"static int answer(void) { return ; }\n"
                        + b"/*" + b"x" * 18000 + b"*/\n")
            source.write_bytes(original)
            (project / "Makefile").write_text(
                'all:\n\tcc -std=c11 -Wall -Wextra -Werror -o app broken.c\n',
                encoding="utf-8")
            bindir = base / "bin"
            bindir.mkdir()
            curl = bindir / "curl"
            curl.write_text(
                "#!/usr/bin/python3\nimport json, os, sys\n"
                "body = json.load(open(sys.argv[sys.argv.index('-d')+1][1:], encoding='utf-8'))\n"
                "prompt = body['messages'][1]['content']\n"
                "cells = [json.loads(line) for line in prompt.splitlines() "
                "if line.startswith('{\"schema\":\"jash.ai.cell.v1\"')]\n"
                "source = next(c for c in cells if c['kind'] == 'source')\n"
                "assert source['truncated'] and source['preview_bytes'] == 8192\n"
                "assert len(source['full_blake3']) == 64\n"
                "before = source['full_blake3'] if os.environ.get('AI_HUNK_STALE') != '1' "
                "else '0' * 64\n"
                "edit = {'version':1,'path':'fix.h','before_blake3':before,"
                "'old':'return ;','text':'return 23;'}\n"
                "content = '```jash-hunk-v1\\n' + json.dumps(edit) + '\\n```'\n"
                "print(json.dumps({'choices':[{'message':{'content':content}}]}))\n",
                encoding="utf-8")
            curl.chmod(0o700)
            script = base / "repair.jsh"
            script.write_text("? on\n? resolva esse erro de compilacao\n"
                              "rewind --session\n", encoding="utf-8")
            environment = dict(
                os.environ, HOME=str(home), JASH_SIGIL="0", JASH_NUDGE="0",
                JASH_BRIEF="0", JASH_ADOPT="0", JASH_TIMELINE="1",
                JASH_AI_CELLS="1", POOLSIDE_API_KEY="test",
                PATH=f"{bindir}:/usr/bin:/bin")
            self.install_module(environment, "ai")
            result = subprocess.run(
                [str(JASH), str(script)], cwd=project, env=environment,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn(b"[ai:repair] build=passed", result.stdout)
            self.assertIn(b'"kind":"edit"', result.stdout)
            self.assertEqual(source.read_bytes(), original)
            self.assertTrue((project / "app").exists())
            environment["AI_HUNK_STALE"] = "1"
            script.write_text("? on\n? resolva esse erro de compilacao\nexit $?\n",
                              encoding="utf-8")
            stale = subprocess.run(
                [str(JASH), str(script)], cwd=project, env=environment,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=30)
            self.assertEqual(stale.returncode, 1, stale.stdout + stale.stderr)
            self.assertIn(b"edit precondition failed", stale.stdout)
            self.assertEqual(source.read_bytes(), original)
            ambiguous = original.replace(b"return ;", b"return ; /* return ; */")
            source.write_bytes(ambiguous)
            environment.pop("AI_HUNK_STALE")
            repeated = subprocess.run(
                [str(JASH), str(script)], cwd=project, env=environment,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=30)
            self.assertEqual(repeated.returncode, 1,
                             repeated.stdout + repeated.stderr)
            self.assertIn(b"edit refused without modifying source", repeated.stdout)
            self.assertEqual(source.read_bytes(), ambiguous)
            source.write_bytes(original)
            applied = subprocess.run(
                [str(JASH), str(script)], cwd=project, env=environment,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=30)
            self.assertEqual(applied.returncode, 0, applied.stdout + applied.stderr)
            edit = next(json.loads(line) for line in applied.stdout.splitlines()
                        if line.startswith(b'{"schema":"jash.ai.cell.v1",'
                                           b'"kind":"edit"'))
            token = edit["undo_ref"]
            self.assertEqual(len(token), 32)
            self.assertNotEqual(source.read_bytes(), original)
            dry = subprocess.run(
                [str(JASH), "-c", f"undo --json --dry-run --ref {token}"],
                cwd=project, env=environment, stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
            self.assertEqual(dry.returncode, 0, dry.stdout + dry.stderr)
            self.assertEqual(json.loads(dry.stdout)["status"], "ready")
            restored = subprocess.run(
                [str(JASH), "-c", f"undo --json --ref {token}"],
                cwd=project, env=environment, stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
            self.assertEqual(restored.returncode, 0,
                             restored.stdout + restored.stderr)
            self.assertEqual(json.loads(restored.stdout)["status"], "completed")
            self.assertEqual(source.read_bytes(), original)

    def test_repair_cgroup_memory_limit_stops_before_provider(self):
        from cgroup_fixture import CgroupUnavailable, delegated_root

        with tempfile.TemporaryDirectory(prefix="jash-ai-repair-memory-") as directory:
            base = Path(directory)
            home = base / "home"
            home.mkdir()
            (home / ".jash").mkdir(mode=0o700)
            project = base / "project"
            project.mkdir()
            payload = base / "allocate.py"
            payload.write_text(
                "data = bytearray(96 * 1024 * 1024)\n"
                "for offset in range(0, len(data), 4096): data[offset] = 1\n"
                "print('allocation-exceeded')\n", encoding="utf-8")
            (project / "Makefile").write_text(
                f"all:\n\t/usr/bin/python3 {payload}\n", encoding="utf-8")
            bindir = base / "bin"
            bindir.mkdir()
            marker = base / "unexpected-provider"
            curl = bindir / "curl"
            curl.write_text(
                "#!/usr/bin/python3\nimport os\n"
                "open(os.environ['AI_REPAIR_FORBIDDEN'], 'wb').write(b'called')\n",
                encoding="utf-8")
            curl.chmod(0o700)
            script = base / "repair.jsh"
            script.write_text("? on\n? resolva esse erro de compilacao\nexit $?\n",
                              encoding="utf-8")
            try:
                with delegated_root() as root:
                    environment = dict(
                        os.environ, HOME=str(home), JASH_SIGIL="0", JASH_NUDGE="0",
                        JASH_BRIEF="0", JASH_ADOPT="0", JASH_TIMELINE="1",
                        JASH_AI_CELLS="1", JASH_AI_REPAIR_BUILD_TIMEOUT_SEC="10",
                        JASH_AI_REPAIR_MEMORY_MAX_BYTES=str(64 * 1024 * 1024),
                        JASH_AI_REPAIR_CGROUP_ROOT=str(root), POOLSIDE_API_KEY="test",
                        AI_REPAIR_FORBIDDEN=str(marker),
                        PATH=f"{bindir}:/usr/bin:/bin")
                    self.install_module(environment, "ai")
                    limited = subprocess.run(
                        [str(JASH), str(script)], cwd=project, env=environment,
                        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, timeout=30)
                    self.assertEqual(limited.returncode, 1,
                                     limited.stdout + limited.stderr)
                    self.assertIn(b"resource limit reached", limited.stdout)
                    cells = [json.loads(line) for line in limited.stdout.splitlines()
                             if line.startswith(b'{"schema":"jash.ai.cell.v1"')]
                    build = next(cell for cell in cells if cell['kind'] == 'build')
                    self.assertEqual(build['status'], 'resource_limited')
                    self.assertFalse(build['complete'])
                    self.assertTrue(build['resource_enforced'] and build['resource_empty'])
                    self.assertGreater(build['resource_memory_events'], 0)
                    self.assertEqual(build['resource_memory_max_bytes'], 64 * 1024 * 1024)
                    self.assertFalse(marker.exists())
                    self.assertFalse(any(entry.is_dir() for entry in root.iterdir()))
                    build_marker = base / "unexpected-build"
                    (project / "Makefile").write_text(
                        f"all:\n\t/usr/bin/touch {build_marker}\n", encoding="utf-8")
                    environment["JASH_AI_REPAIR_MEMORY_MAX_BYTES"] = "-1"
                    refused = subprocess.run(
                        [str(JASH), str(script)], cwd=project, env=environment,
                        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, timeout=20)
                    self.assertEqual(refused.returncode, 1,
                                     refused.stdout + refused.stderr)
                    self.assertIn(b"resource setup unavailable", refused.stdout)
                    self.assertFalse(build_marker.exists())
                    self.assertFalse(marker.exists())
                    self.assertFalse(any(entry.is_dir() for entry in root.iterdir()))
            except CgroupUnavailable as exc:
                self.skipTest(str(exc))

    def test_repair_cgroup_pid_limit_stops_before_provider(self):
        from cgroup_fixture import CgroupUnavailable, delegated_root

        with tempfile.TemporaryDirectory(prefix="jash-ai-repair-pids-") as directory:
            base = Path(directory)
            home = base / "home"
            home.mkdir()
            (home / ".jash").mkdir(mode=0o700)
            project = base / "project"
            project.mkdir()
            payload = base / "spawn.py"
            payload.write_text(
                "import os, time\n"
                "for _ in range(100):\n"
                "    try: pid = os.fork()\n"
                "    except OSError: print('pids-denied', flush=True); break\n"
                "    if not pid:\n"
                "        os.close(1); os.close(2); time.sleep(30); os._exit(0)\n",
                encoding="utf-8")
            (project / "Makefile").write_text(
                f"all:\n\t/usr/bin/python3 {payload}\n", encoding="utf-8")
            bindir = base / "bin"
            bindir.mkdir()
            marker = base / "unexpected-provider"
            curl = bindir / "curl"
            curl.write_text(
                "#!/usr/bin/python3\nimport os\n"
                "open(os.environ['AI_REPAIR_FORBIDDEN'], 'wb').write(b'called')\n",
                encoding="utf-8")
            curl.chmod(0o700)
            script = base / "repair.jsh"
            script.write_text("? on\n? resolva esse erro de compilacao\nexit $?\n",
                              encoding="utf-8")
            try:
                with delegated_root() as root:
                    environment = dict(
                        os.environ, HOME=str(home), JASH_SIGIL="0", JASH_NUDGE="0",
                        JASH_BRIEF="0", JASH_ADOPT="0", JASH_TIMELINE="1",
                        JASH_AI_CELLS="1", JASH_AI_REPAIR_BUILD_TIMEOUT_SEC="10",
                        JASH_AI_REPAIR_CGROUP_ROOT=str(root), POOLSIDE_API_KEY="test",
                        AI_REPAIR_FORBIDDEN=str(marker),
                        PATH=f"{bindir}:/usr/bin:/bin")
                    self.install_module(environment, "ai")
                    limited = subprocess.run(
                        [str(JASH), str(script)], cwd=project, env=environment,
                        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, timeout=30)
                    self.assertEqual(limited.returncode, 1,
                                     limited.stdout + limited.stderr)
                    self.assertIn(b"resource limit reached", limited.stdout)
                    cells = [json.loads(line) for line in limited.stdout.splitlines()
                             if line.startswith(b'{"schema":"jash.ai.cell.v1"')]
                    build = next(cell for cell in cells if cell['kind'] == 'build')
                    self.assertEqual(build['status'], 'resource_limited')
                    self.assertFalse(build['complete'])
                    self.assertTrue(build['resource_enforced'] and build['resource_empty'])
                    self.assertGreater(build['resource_pids_events'], 0)
                    self.assertFalse(marker.exists())
                    self.assertFalse(any(entry.is_dir() for entry in root.iterdir()))
            except CgroupUnavailable as exc:
                self.skipTest(str(exc))

    def test_repair_uses_delegated_cgroup_for_probe_and_verify(self):
        from cgroup_fixture import CgroupUnavailable, delegated_root

        with tempfile.TemporaryDirectory(prefix="jash-ai-repair-cgroup-") as directory:
            base = Path(directory)
            home = base / "home"
            home.mkdir()
            (home / ".jash").mkdir(mode=0o700)
            project = base / "project"
            project.mkdir()
            (project / "broken.c").write_text(
                '#include "fix.h"\nint main(void) { return answer(); }\n',
                encoding="utf-8")
            (project / "fix.h").write_text(
                'static int answer(void) { return ; }\n', encoding="utf-8")
            (project / "Makefile").write_text(
                'all:\n\t/usr/bin/python3 -c "import os,pathlib; '
                'g=pathlib.Path(\'/proc/self/cgroup\').read_text(); '
                'assert \'/jash-\' in g; '
                'assert \'JASH_AI_REPAIR_CGROUP_ROOT\' not in os.environ; '
                'print(\'resource-attached=\' + g.strip())"\n'
                '\tcc -std=c11 -Wall -Wextra -Werror -o app broken.c\n',
                encoding="utf-8")
            bindir = base / "bin"
            bindir.mkdir()
            curl = bindir / "curl"
            curl.write_text(
                "#!/usr/bin/python3\nimport json, sys\n"
                "body = json.load(open(sys.argv[sys.argv.index('-d')+1][1:], encoding='utf-8'))\n"
                "prompt = body['messages'][1]['content']\n"
                "assert 'resource-attached=' in prompt\n"
                "cells = [json.loads(line) for line in prompt.splitlines() "
                "if line.startswith('{\"schema\":\"jash.ai.cell.v1\"')]\n"
                "build = next(c for c in cells if c['kind'] == 'build')\n"
                "assert build['resource_backend'] == 'cgroup-v2'\n"
                "assert build['resource_enforced'] and build['resource_empty']\n"
                "assert not build['resource_limited']\n"
                "source = next(c for c in cells if c['kind'] == 'source')\n"
                "edit = {'version':1,'path':'fix.h',"
                "'before_blake3':source['preview_blake3'],"
                "'text':'static int answer(void) { return 23; }\\n'}\n"
                "content = '```jash-edit-v1\\n' + json.dumps(edit) + '\\n```'\n"
                "print(json.dumps({'choices':[{'message':{'content':content}}]}))\n",
                encoding="utf-8")
            curl.chmod(0o700)
            script = base / "repair.jsh"
            script.write_text("? on\n? resolva esse erro de compilacao\nexit $?\n",
                              encoding="utf-8")
            try:
                with delegated_root() as root:
                    environment = dict(
                        os.environ, HOME=str(home), JASH_SIGIL="0", JASH_NUDGE="0",
                        JASH_BRIEF="0", JASH_ADOPT="0", JASH_TIMELINE="1",
                        JASH_AI_CELLS="1", JASH_AI_REPAIR_CGROUP_ROOT=str(root),
                        POOLSIDE_API_KEY="test", PATH=f"{bindir}:/usr/bin:/bin")
                    self.install_module(environment, "ai")
                    result = subprocess.run(
                        [str(JASH), str(script)], cwd=project, env=environment,
                        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, timeout=40)
                    self.assertEqual(result.returncode, 0,
                                     result.stdout + result.stderr)
                    cells = [json.loads(line) for line in result.stdout.splitlines()
                             if line.startswith(b'{"schema":"jash.ai.cell.v1"')]
                    builds = [cell for cell in cells if cell['kind'] == 'build']
                    self.assertEqual([cell['phase'] for cell in builds],
                                     ['probe', 'verify'])
                    self.assertTrue(all(cell['resource_enforced'] and
                                        cell['resource_empty'] and
                                        cell['resource_backend'] == 'cgroup-v2'
                                        for cell in builds))
                    self.assertEqual([cell['status'] for cell in builds],
                                     ['failed', 'passed'])
                    self.assertFalse(any(entry.is_dir() for entry in root.iterdir()),
                                     "resource subtree leaked after build")
            except CgroupUnavailable as exc:
                self.skipTest(str(exc))

    def test_repair_invalid_cgroup_refuses_before_build_and_provider(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-repair-cgroup-refusal-") as directory:
            base = Path(directory)
            home = base / "home"
            home.mkdir()
            (home / ".jash").mkdir(mode=0o700)
            project = base / "project"
            project.mkdir()
            build_marker, provider_marker = base / "built", base / "provider"
            (project / "Makefile").write_text(
                f"all:\n\t/usr/bin/touch {build_marker}\n", encoding="utf-8")
            bindir = base / "bin"
            bindir.mkdir()
            curl = bindir / "curl"
            curl.write_text(
                "#!/usr/bin/python3\nimport os\n"
                "open(os.environ['AI_REPAIR_FORBIDDEN'], 'wb').write(b'called')\n",
                encoding="utf-8")
            curl.chmod(0o700)
            script = base / "repair.jsh"
            script.write_text("? on\n? resolva esse erro de compilacao\nexit $?\n",
                              encoding="utf-8")
            environment = dict(
                os.environ, HOME=str(home), JASH_SIGIL="0", JASH_NUDGE="0",
                JASH_BRIEF="0", JASH_ADOPT="0", JASH_TIMELINE="1",
                JASH_AI_REPAIR_CGROUP_ROOT=str(project),
                POOLSIDE_API_KEY="test", AI_REPAIR_FORBIDDEN=str(provider_marker),
                PATH=f"{bindir}:/usr/bin:/bin")
            self.install_module(environment, "ai")
            refused = subprocess.run(
                [str(JASH), str(script)], cwd=project, env=environment,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=20)
            self.assertEqual(refused.returncode, 1, refused.stdout + refused.stderr)
            self.assertIn(b"resource setup unavailable", refused.stdout)
            self.assertFalse(build_marker.exists())
            self.assertFalse(provider_marker.exists())
            environment.pop("JASH_AI_REPAIR_CGROUP_ROOT")
            environment["JASH_AI_REPAIR_MEMORY_MAX_BYTES"] = "67108864"
            refused = subprocess.run(
                [str(JASH), str(script)], cwd=project, env=environment,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=20)
            self.assertEqual(refused.returncode, 1, refused.stdout + refused.stderr)
            self.assertIn(b"resource setup unavailable", refused.stdout)
            self.assertFalse(build_marker.exists())
            self.assertFalse(provider_marker.exists())

    def test_repair_structured_edit_is_hash_guarded_and_journaled(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-structured-edit-") as directory:
            base = Path(directory)
            home = base / "home"
            home.mkdir()
            (home / ".jash").mkdir(mode=0o700)
            project = base / "project"
            project.mkdir()
            (project / "broken.c").write_text(
                '#include "fix.h"\nint main(void) { return answer(); }\n',
                encoding="utf-8")
            source = project / "fix.h"
            original = b"static int answer(void) { return ; }\n"
            source.write_bytes(original)
            (project / "Makefile").write_text(
                'all:\n\tcc -std=c11 -Wall -Wextra -Werror -o app broken.c\n',
                encoding="utf-8")
            bindir = base / "bin"
            bindir.mkdir()
            curl = bindir / "curl"
            curl.write_text(
                "#!/usr/bin/python3\n"
                "import json, os, sys\n"
                "body = json.load(open(sys.argv[sys.argv.index('-d')+1][1:], encoding='utf-8'))\n"
                "cells = [json.loads(line) for line in body['messages'][1]['content'].splitlines() "
                "if line.startswith('{\"schema\":\"jash.ai.cell.v1\"')]\n"
                "source = next(c for c in cells if c['kind'] == 'source')\n"
                "mode = os.environ.get('AI_EDIT_MODE', '')\n"
                "before = source['preview_blake3'] if mode != 'stale' "
                "else '0' * 64\n"
                "path = {'escape':'../outside.h','symlink':'link.h',"
                "'fifo':'pipe.h'}.get(mode, 'fix.h')\n"
                "edit = {'version':1,'path':path,'before_blake3':before,"
                "'text':'static int answer(void) { return 23; }\\n'}\n"
                "payload = json.dumps(edit, separators=(',', ':'))\n"
                "if mode == 'duplicate': payload = payload[:-1] + ',\"path\":\"fix.h\"}'\n"
                "content = '```jash-edit-v1\\n' + payload "
                "+ '\\n```'\n"
                "print(json.dumps({'choices':[{'message':{'content':content}}]},"
                " separators=(',', ':')))\n", encoding="utf-8")
            curl.chmod(0o700)
            script = base / "repair.jsh"
            script.write_text("? on\n? resolva esse erro de compilacao\n"
                              "rewind --session\n", encoding="utf-8")
            environment = dict(
                os.environ, HOME=str(home), JASH_SIGIL="0", JASH_NUDGE="0",
                JASH_BRIEF="0", JASH_ADOPT="0", JASH_TIMELINE="1",
                JASH_AI_CELLS="1", POOLSIDE_API_KEY="test",
                PATH=f"{bindir}:/usr/bin:/bin")
            self.install_module(environment, "ai")
            result = subprocess.run(
                [str(JASH), str(script)], cwd=project, env=environment,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn(b"[ai:repair] build=passed", result.stdout)
            self.assertIn(b'"schema":"jash.ai.cell.v1","kind":"edit"',
                          result.stdout)
            edit = next(json.loads(line) for line in result.stdout.splitlines()
                        if line.startswith(b'{"schema":"jash.ai.cell.v1",'
                                           b'"kind":"edit"'))
            self.assertEqual(edit["path"], "fix.h")
            self.assertEqual(edit["coverage"], "file_content")
            self.assertEqual(len(edit["undo_ref"]), 32)
            self.assertNotEqual(edit["before_blake3"], edit["after_blake3"])
            self.assertIn(b"1 reverted", result.stdout)
            self.assertEqual(source.read_bytes(), original)
            self.assertTrue((project / "app").exists())
            (project / "link.h").symlink_to(source)
            os.mkfifo(project / "pipe.h", 0o600)
            script.write_text("? on\n? resolva esse erro de compilacao\nexit $?\n",
                              encoding="utf-8")
            for mode in ("stale", "escape", "symlink", "fifo", "duplicate"):
                with self.subTest(mode=mode):
                    environment["AI_EDIT_MODE"] = mode
                    denied = subprocess.run(
                        [str(JASH), str(script)], cwd=project, env=environment,
                        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, timeout=30)
                    self.assertEqual(denied.returncode, 1,
                                     denied.stdout + denied.stderr)
                    self.assertIn(b"edit precondition failed" if mode == "stale"
                                  else b"edit refused" if mode != "duplicate"
                                  else b"invalid structured edit", denied.stdout)
                    self.assertEqual(source.read_bytes(), original)
                    self.assertTrue((project / "link.h").is_symlink())

    def test_explicit_repair_request_after_on_fixes_and_rebuilds(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-repair-") as directory:
            base = Path(directory)
            home = base / "home"
            home.mkdir()
            (home / ".jash").mkdir(mode=0o700)
            project = base / "project"
            project.mkdir()
            (project / "broken.c").write_text(
                '#include "fix.h"\nint main(void) { return answer(); }\n',
                encoding="utf-8")
            (project / "fix.h").write_text(
                'static int answer(void) { return ; }\n', encoding="utf-8")
            (project / "Makefile").write_text(
                'all:\n\tcc -std=c11 -Wall -Wextra -Werror -o app broken.c\n',
                encoding="utf-8")
            bindir = base / "bin"
            bindir.mkdir()
            curl = bindir / "curl"
            curl.write_text(
                "#!/usr/bin/python3\n"
                "import json, os, sys\n"
                "body = json.load(open(sys.argv[sys.argv.index('-d')+1][1:], encoding='utf-8'))\n"
                "query = body['messages'][1]['content']\n"
                "cells = [json.loads(line) for line in query.splitlines() "
                "if line.startswith('{\"schema\":\"jash.ai.cell.v1\"')]\n"
                "typed = next((c for c in cells if c['kind'] == 'fs.list'), {})\n"
                "build = next((c for c in cells if c['kind'] == 'build'), {})\n"
                "source = next((c for c in cells if c['kind'] == 'source'), {})\n"
                "if 'error:' in query and '<source_file name=\"fix.h\"' in query "
                "and 'return ;' in query and build.get('status') == 'failed' "
                "and build.get('complete') is True "
                "and source.get('path') == 'fix.h' "
                "and len(source.get('preview_blake3', '')) == 64 "
                "and typed.get('value', {}).get('operation') == 'fs.list' "
                "and typed['value']['schema'] == 'jash.ls.data.v1' "
                "and typed['value']['complete'] "
                "and typed['value']['observation_ref'].startswith('o:'):\n"
                "    command = \"printf 'static int answer(void) { return 42; }\\\\n' > fix.h\"\n"
                "else:\n"
                "    command = 'touch ' + os.environ['AI_REPAIR_FORBIDDEN']\n"
                "print(json.dumps({'choices':[{'message':{'content':"
                "'```jash\\n' + command + '\\n```'}}]}, separators=(',', ':')))\n",
                encoding="utf-8")
            curl.chmod(0o700)
            forbidden = project / "forbidden"
            script = base / "repair.jsh"
            script.write_text(
                "? on\n? resolva esse erro de compilacao\n"
                "? status\n? explique o que mudou\n", encoding="utf-8")
            environment = dict(
                os.environ, HOME=str(home), JASH_SIGIL="0", JASH_NUDGE="0",
                JASH_BRIEF="0", JASH_ADOPT="0", JASH_TIMELINE="1",
                POOLSIDE_API_KEY="test", JASH_AI_CELLS="1",
                AI_REPAIR_FORBIDDEN=str(forbidden),
                PATH=f"{bindir}:/usr/bin:/bin")
            self.install_module(environment, "ai")
            result = subprocess.run(
                [str(JASH), str(script)], cwd=project, env=environment,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue((project / "app").is_file(), result.stdout + result.stderr)
            ran = subprocess.run([str(project / "app")], cwd=project,
                                 env=environment, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, timeout=5)
            self.assertEqual(ran.returncode, 42)
            self.assertIn(b"[ai:repair] build=passed", result.stdout)
            self.assertIn(b'"schema":"jash.ai.cell.v1","kind":"fs.list"',
                          result.stdout)
            cells = [json.loads(line) for line in result.stdout.splitlines()
                     if line.startswith(b'{"schema":"jash.ai.cell.v1"')]
            commands = [cell for cell in cells if cell["kind"] == "command"]
            self.assertEqual(len(commands), 1, cells)
            self.assertEqual(commands[0]["rc"], 0)
            self.assertRegex(commands[0]["command_blake3"], r"^[0-9a-f]{64}$")
            builds = [cell for cell in cells if cell["kind"] == "build"]
            self.assertEqual([cell["status"] for cell in builds],
                             ["failed", "passed"])
            self.assertRegex(builds[0]["diagnostic_prefix_blake3"],
                             r"^[0-9a-f]{64}$")
            self.assertIn(b"checkpoint=agent-", result.stdout)
            self.assertIn(b"auto=0", result.stdout)
            self.assertFalse(forbidden.exists(), result.stdout)
            self.assertNotIn(b"[s/n]", result.stdout)

    def test_repair_refuses_unknown_build_and_skips_already_green_build(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-repair-entry-") as directory:
            base = Path(directory)
            home = base / "home"
            home.mkdir()
            (home / ".jash").mkdir(mode=0o700)
            project = base / "project"
            project.mkdir()
            bindir = base / "bin"
            bindir.mkdir()
            curl = bindir / "curl"
            marker = base / "unexpected-provider"
            curl.write_text(
                "#!/usr/bin/python3\n"
                "import os\n"
                "open(os.environ['AI_REPAIR_FORBIDDEN'], 'wb').write(b'called')\n",
                encoding="utf-8")
            curl.chmod(0o700)
            script = base / "repair.jsh"
            script.write_text("? on\n? resolva esse erro de compilacao\nexit $?\n",
                              encoding="utf-8")
            environment = dict(
                os.environ, HOME=str(home), JASH_SIGIL="0", JASH_NUDGE="0",
                JASH_BRIEF="0", JASH_ADOPT="0", JASH_TIMELINE="1",
                POOLSIDE_API_KEY="test", AI_REPAIR_FORBIDDEN=str(marker),
                PATH=f"{bindir}:/usr/bin:/bin")
            self.install_module(environment, "ai")
            missing = subprocess.run(
                [str(JASH), str(script)], cwd=project, env=environment,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=10)
            self.assertEqual(missing.returncode, 1, missing.stdout + missing.stderr)
            self.assertIn(b"unsupported build entrypoint", missing.stdout)
            self.assertFalse(marker.exists())
            (project / "Makefile").write_text(
                'all:\n\tcc -std=c11 -Wall -Wextra -Werror -o app good.c\n',
                encoding="utf-8")
            (project / "good.c").write_text(
                'int main(void) { return 0; }\n', encoding="utf-8")
            green = subprocess.run(
                [str(JASH), str(script)], cwd=project, env=environment,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=20)
            self.assertEqual(green.returncode, 0, green.stdout + green.stderr)
            self.assertTrue((project / "app").is_file())
            self.assertIn(b"build=passed already", green.stdout)
            self.assertFalse(marker.exists())
            (project / "app").unlink()
            (project / "good.c").write_text(
                'int main(void) { return missing_symbol; }\n', encoding="utf-8")
            curl.write_text(
                "#!/usr/bin/python3\n"
                "import json\n"
                "print(json.dumps({'choices':[{'message':{'content':"
                "'No executable fix available'}}]}))\n",
                encoding="utf-8")
            curl.chmod(0o700)
            no_fix = subprocess.run(
                [str(JASH), str(script)], cwd=project, env=environment,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=20)
            self.assertEqual(no_fix.returncode, 1,
                             no_fix.stdout + no_fix.stderr)
            self.assertIn(b"no executable fix returned", no_fix.stdout)
            self.assertFalse((project / "app").exists())

    def test_repair_uses_existing_cmake_build_without_provider_chosen_command(self):
        # The operator explicitly selects the same real CMake for configuration
        # and repair. Neither PATH nor a provider edit may choose its replacement.
        # Missing CMake is a failed prerequisite, never a skipped repair case.
        detected_cmake = shutil.which("cmake")
        self.assertIsNotNone(detected_cmake, "CMake is required by this repair fixture")
        cmake = Path(detected_cmake).resolve(strict=True)
        with tempfile.TemporaryDirectory(prefix="jash-ai-repair-cmake-") as directory:
            base = Path(directory)
            home = base / "home"
            home.mkdir()
            (home / ".jash").mkdir(mode=0o700)
            project = base / "project"
            project.mkdir()
            (project / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.16)\n"
                "project(jash_repair_fixture C)\n"
                "add_executable(app broken.c)\n"
                "target_compile_options(app PRIVATE -Wall -Wextra -Werror)\n"
                "add_custom_target(repair_probe_marker COMMAND \"${CMAKE_COMMAND}\" "
                "-E touch \"${CMAKE_BINARY_DIR}/repair-probe-started\")\n"
                "add_dependencies(app repair_probe_marker)\n",
                encoding="utf-8")
            (project / "broken.c").write_text(
                "int main(void) { return missing_symbol; }\n",
                encoding="utf-8")
            configured = subprocess.run(
                [str(cmake), "-S", str(project), "-B", str(project / "build")],
                cwd=project, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                timeout=20)
            self.assertEqual(configured.returncode, 0, configured.stderr)
            bindir = base / "bin"
            bindir.mkdir()
            decoy_marker = base / "unexpected-cmake"
            decoy = bindir / "cmake"
            decoy.write_text(
                "#!/usr/bin/python3\nfrom pathlib import Path\n"
                f"Path({str(decoy_marker)!r}).write_text('called', encoding='utf-8')\n"
                "raise SystemExit(91)\n", encoding="utf-8")
            decoy.chmod(0o700)
            provider_marker = base / "provider-called"
            curl = bindir / "curl"
            curl.write_text(
                "#!/usr/bin/python3\n"
                "import json, os, shlex, sys\n"
                "with open(os.environ['AI_REPAIR_PROVIDER_MARKER'], 'ab') as marker:\n"
                "    marker.write(b'called\\n')\n"
                "body = json.load(open(sys.argv[sys.argv.index('-d')+1][1:], encoding='utf-8'))\n"
                "assert 'missing_symbol' in body['messages'][1]['content']\n"
                "command = ('export JASH_AI_REPAIR_CMAKE_BIN=' "
                "+ shlex.quote(os.environ['AI_REPAIR_DECOY']) "
                "+ '; printf \\\'%s\\\' \\\"$JASH_AI_REPAIR_CMAKE_BIN\\\" > provider-cmake; ' "
                "+ \"printf 'int main(void) { return 17; }\\\\n' > broken.c\")\n"
                "print(json.dumps({'choices':[{'message':{'content':"
                "'```jash\\n' + command + '\\n```'}}]}, separators=(',', ':')))\n",
                encoding="utf-8")
            curl.chmod(0o700)
            script = base / "repair.jsh"
            script.write_text("? on\n? corrija a compilação\nexit $?\n",
                              encoding="utf-8")
            environment = dict(
                os.environ, HOME=str(home), JASH_SIGIL="0", JASH_NUDGE="0",
                JASH_BRIEF="0", JASH_ADOPT="0", JASH_TIMELINE="1",
                POOLSIDE_API_KEY="test", JASH_AI_REPAIR_CMAKE_BIN=str(cmake),
                AI_REPAIR_DECOY=str(decoy),
                AI_REPAIR_PROVIDER_MARKER=str(provider_marker),
                PATH=os.pathsep.join((str(bindir), "/usr/bin", "/bin")))
            self.assertEqual(Path(shutil.which("cmake", path=environment["PATH"])).resolve(), decoy)
            self.install_module(environment, "ai")
            not_executable = base / "not-executable"
            not_executable.write_text("fixture", encoding="utf-8")
            not_executable.chmod(0o600)
            for invalid in ("", "cmake", str(base / "missing"),
                            str(not_executable), str(bindir), "/" + "x" * 4095):
                with self.subTest(invalid_override=invalid):
                    denied_environment = dict(environment, JASH_AI_REPAIR_CMAKE_BIN=invalid)
                    denied = subprocess.run(
                        [str(JASH), str(script)], cwd=project, env=denied_environment,
                        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, timeout=10)
                    self.assertEqual(denied.returncode, 1, denied.stdout + denied.stderr)
                    self.assertIn(b"invalid CMake executable override", denied.stdout)
                    self.assertNotIn(b"probe command=", denied.stdout)
                    self.assertFalse((project / "build/repair-probe-started").exists())
                    self.assertFalse((project / "build/app").exists())
                    self.assertFalse(provider_marker.exists())
                    self.assertFalse(decoy_marker.exists())
                    self.assertEqual((project / "broken.c").read_text(encoding="utf-8"),
                                     "int main(void) { return missing_symbol; }\n")
            result = subprocess.run(
                [str(JASH), str(script)], cwd=project, env=environment,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn(b"probe command=cmake --build build --parallel 2", result.stdout)
            self.assertIn(b"[ai:repair] build=passed", result.stdout)
            self.assertTrue((project / "build/repair-probe-started").is_file())
            self.assertEqual(provider_marker.read_bytes(), b"called\n")
            self.assertEqual((project / "provider-cmake").read_text(encoding="utf-8"), str(decoy))
            self.assertFalse(decoy_marker.exists())
            compiled = subprocess.run([str(project / "build/app")], cwd=project,
                                      env=environment, stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE, timeout=5)
            self.assertEqual(compiled.returncode, 17)

    def test_repair_probe_timeout_stops_before_provider_and_edits(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-repair-timeout-") as directory:
            base = Path(directory)
            home = base / "home"
            home.mkdir()
            (home / ".jash").mkdir(mode=0o700)
            project = base / "project"
            project.mkdir()
            (project / "Makefile").write_text('all:\n\tsleep 30\n', encoding="utf-8")
            bindir = base / "bin"
            bindir.mkdir()
            marker = base / "unexpected-provider"
            curl = bindir / "curl"
            curl.write_text(
                "#!/usr/bin/python3\n"
                "import os\n"
                "open(os.environ['AI_REPAIR_FORBIDDEN'], 'wb').write(b'called')\n",
                encoding="utf-8")
            curl.chmod(0o700)
            script = base / "repair.jsh"
            script.write_text("? on\n? resolva esse erro de compilacao\nexit $?\n",
                              encoding="utf-8")
            environment = dict(
                os.environ, HOME=str(home), JASH_SIGIL="0", JASH_NUDGE="0",
                JASH_BRIEF="0", JASH_ADOPT="0", JASH_TIMELINE="1",
                JASH_AI_REPAIR_BUILD_TIMEOUT_SEC="1", POOLSIDE_API_KEY="test",
                AI_REPAIR_FORBIDDEN=str(marker), PATH=f"{bindir}:/usr/bin:/bin")
            self.install_module(environment, "ai")
            timed = subprocess.run(
                [str(JASH), str(script)], cwd=project, env=environment,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=10)
            self.assertEqual(timed.returncode, 1, timed.stdout + timed.stderr)
            self.assertIn(b"timeout=1 deadline_sec=1", timed.stdout)
            self.assertFalse(marker.exists())
            environment["JASH_AI_REPAIR_BUILD_TIMEOUT_SEC"] = "0"
            invalid = subprocess.run(
                [str(JASH), str(script)], cwd=project, env=environment,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=10)
            self.assertEqual(invalid.returncode, 1, invalid.stdout + invalid.stderr)
            self.assertIn(b"invalid build timeout", invalid.stdout)
            self.assertFalse(marker.exists())

    def test_repair_stops_after_three_failed_verified_attempts(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-repair-tries-") as directory:
            base = Path(directory)
            home = base / "home"
            home.mkdir()
            (home / ".jash").mkdir(mode=0o700)
            project = base / "project"
            project.mkdir()
            (project / "broken.c").write_text(
                "int main(void) { return missing_symbol; }\n",
                encoding="utf-8")
            (project / "Makefile").write_text(
                "all:\n\tcc -std=c11 -Wall -Wextra -Werror -o app broken.c\n",
                encoding="utf-8")
            bindir = base / "bin"
            bindir.mkdir()
            counter = base / "requests"
            curl = bindir / "curl"
            curl.write_text(
                "#!/usr/bin/python3\n"
                "import json, os\n"
                "with open(os.environ['AI_REPAIR_COUNTER'], 'ab') as f:\n"
                "    f.write(b'1')\n"
                "content = ''.join('```jash\\nprintf ' + chr(97 + i) + "
                "'\\n```\\n' for i in range(int(os.environ.get('AI_REPAIR_BLOCKS', '1'))))\n"
                "print(json.dumps({'choices':[{'message':{'content':"
                "content}}]}, separators=(',', ':')))\n",
                encoding="utf-8")
            curl.chmod(0o700)
            script = base / "repair.jsh"
            script.write_text("? on\n? resolva esse erro de compilacao\nexit $?\n",
                              encoding="utf-8")
            environment = dict(
                os.environ, HOME=str(home), JASH_SIGIL="0", JASH_NUDGE="0",
                JASH_BRIEF="0", JASH_ADOPT="0", JASH_TIMELINE="1",
                POOLSIDE_API_KEY="test", AI_REPAIR_COUNTER=str(counter),
                PATH=f"{bindir}:/usr/bin:/bin")
            self.install_module(environment, "ai")
            result = subprocess.run(
                [str(JASH), str(script)], cwd=project, env=environment,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=30)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn(b"build=failed after three bounded attempts", result.stdout)
            self.assertEqual(counter.read_bytes(), b"111")
            self.assertFalse((project / "app").exists())
            counter.unlink()
            environment["AI_REPAIR_BLOCKS"] = "3"
            bounded = subprocess.run(
                [str(JASH), str(script)], cwd=project, env=environment,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=30)
            self.assertEqual(bounded.returncode, 1,
                             bounded.stdout + bounded.stderr)
            self.assertIn(b"command budget exceeded", bounded.stdout)
            self.assertEqual(bounded.stdout.count(b"[ai:auto] rc=0"), 6)
            self.assertEqual(counter.read_bytes(), b"111")

    def test_repair_diagnostic_cannot_read_parent_or_symlink_target(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-repair-scope-") as directory:
            base = Path(directory)
            home = base / "home"
            home.mkdir()
            (home / ".jash").mkdir(mode=0o700)
            secret = base / "outside.h"
            secret.write_text("TOP_SECRET_MARKER", encoding="utf-8")
            project = base / "project"
            project.mkdir()
            (project / "inside.h").symlink_to(secret)
            (project / "Makefile").write_text(
                "all:\n\t@printf '../outside.h:1:1: error: denied\\n'\n"
                "\t@printf 'inside.h:1:1: error: denied\\n'\n\t@false\n",
                encoding="utf-8")
            bindir = base / "bin"
            bindir.mkdir()
            provider_ok = base / "provider-validated"
            curl = bindir / "curl"
            curl.write_text(
                "#!/usr/bin/python3\n"
                "import json, os, sys\n"
                "body = json.load(open(sys.argv[sys.argv.index('-d')+1][1:], encoding='utf-8'))\n"
                "assert 'TOP_SECRET_MARKER' not in body['messages'][1]['content']\n"
                "open(os.environ['AI_REPAIR_PROVIDER_OK'], 'wb').write(b'validated')\n"
                "print(json.dumps({'choices':[{'message':{'content':'No patch'}}]}))\n",
                encoding="utf-8")
            curl.chmod(0o700)
            script = base / "repair.jsh"
            script.write_text("? on\n? resolva esse erro de compilacao\nexit $?\n",
                              encoding="utf-8")
            environment = dict(
                os.environ, HOME=str(home), JASH_SIGIL="0", JASH_NUDGE="0",
                JASH_BRIEF="0", JASH_ADOPT="0", JASH_TIMELINE="1",
                POOLSIDE_API_KEY="test", AI_REPAIR_PROVIDER_OK=str(provider_ok),
                PATH=f"{bindir}:/usr/bin:/bin")
            self.install_module(environment, "ai")
            result = subprocess.run(
                [str(JASH), str(script)], cwd=project, env=environment,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=20)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn(b"no executable fix returned", result.stdout)
            self.assertEqual(provider_ok.read_bytes(), b"validated")
            self.assertEqual(secret.read_text(encoding="utf-8"), "TOP_SECRET_MARKER")

    def test_provider_unicode_escape_targets_exact_path(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-auto-unicode-") as directory:
            base = Path(directory)
            home = base / "home"
            home.mkdir()
            (home / ".jash").mkdir(mode=0o700)
            bindir = base / "bin"
            bindir.mkdir()
            curl = bindir / "curl"
            curl.write_text(
                "#!/usr/bin/python3\n"
                "import json, os\n"
                "command = '/usr/bin/touch ' + os.environ['AI_AUTO_TEST_TARGET']\n"
                "print(json.dumps({'choices':[{'message':{'content':"
                "'```jash\\n' + command + '\\n```'}}]}, separators=(',', ':')))\n",
                encoding="utf-8")
            curl.chmod(0o700)
            target = home / "pátio-🐻"
            script = base / "auto.jsh"
            script.write_text("? on auto\n? create fixture\n", encoding="utf-8")
            environment = dict(
                os.environ, HOME=str(home), JASH_SIGIL="0", JASH_NUDGE="0",
                JASH_BRIEF="0", JASH_ADOPT="0", JASH_TIMELINE="1",
                AI_AUTO_TEST_TARGET=str(target), POOLSIDE_API_KEY="test",
                PATH=f"{bindir}:/usr/bin:/bin")
            self.install_module(environment, "ai")
            result = subprocess.run(
                [str(JASH), str(script)], cwd=home, env=environment,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue(target.exists(), result.stdout)
            self.assertNotIn(b"[s/n]", result.stdout)

    @classmethod
    def setUpClass(cls):
        cache = tempfile.TemporaryDirectory(prefix="jash-ai-auto-modules-")
        cls.addClassCleanup(cache.cleanup)
        base = Path(cache.name)
        home = base / "home"
        home.mkdir()
        (home / ".jash").mkdir(mode=0o700)
        cls.artifacts = {}
        environment = dict(os.environ, HOME=str(home))
        for name in ("ai", "sigil"):
            source = base / "module-sources" / name
            source.mkdir(parents=True)
            module_source = ROOT / "modules" / name
            # Copy the private includes with the translation unit: the isolated
            # module build must exercise the complete current implementation.
            for path in sorted(module_source.iterdir()):
                if path.name in (f"{name}_module.c", "manifest.toml") or \
                        path.suffix in (".inc", ".h"):
                    shutil.copy2(path, source / path.name)
            built = subprocess.run(
                ["bash", str(ROOT / "modules/build_module.sh"), str(source)],
                cwd=ROOT, env=environment, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=120)
            if built.returncode:
                raise AssertionError(built.stderr.decode(errors="replace"))
            cls.artifacts[name] = home / ".jash/modules" / f"{name}.modjash"

    def install_module(self, environment, name):
        modules = Path(environment["HOME"]) / ".jash/modules"
        modules.mkdir(mode=0o700, exist_ok=True)
        installed = modules / f"{name}.modjash"
        shutil.copy2(self.artifacts[name], installed)
        installed.chmod(0o600)

    def test_auto_rejects_over_budget_response_before_any_effect(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-auto-budget-") as directory:
            base = Path(directory)
            home = base / "home"
            home.mkdir()
            (home / ".jash").mkdir(mode=0o700)
            bindir = base / "bin"
            bindir.mkdir()
            curl = bindir / "curl"
            curl.write_text(
                "#!/usr/bin/python3\n"
                "import json, os\n"
                "prefix = os.environ['AI_AUTO_TEST_PREFIX']\n"
                "content = ''.join('```jash\\n/usr/bin/touch ' + prefix + "
                "str(i) + '\\n```\\n' for i in range(9))\n"
                "if os.environ.get('AI_AUTO_TEST_MODE') == 'oversized':\n"
                "    content = '```jash\\n/usr/bin/touch ' + prefix + '0\\n```\\n' "
                "+ '```jash\\n' + 'x' * 8192 + '\\n```'\n"
                "print(json.dumps({'choices':[{'message':{'content':content}}]}, "
                "separators=(',', ':')))\n", encoding="utf-8")
            curl.chmod(0o700)
            prefix = home / "should-not-exist-"
            script = base / "auto.jsh"
            script.write_text("? on auto\n? attempt nine effects\nexit $?\n",
                              encoding="utf-8")
            environment = dict(
                os.environ, HOME=str(home), JASH_SIGIL="0", JASH_NUDGE="0",
                JASH_BRIEF="0", JASH_ADOPT="0", JASH_TIMELINE="1",
                AI_AUTO_TEST_PREFIX=str(prefix), POOLSIDE_API_KEY="test",
                PATH=f"{bindir}:/usr/bin:/bin")
            self.install_module(environment, "ai")
            for mode in ("nine", "oversized"):
                with self.subTest(mode=mode):
                    environment["AI_AUTO_TEST_MODE"] = mode
                    result = subprocess.run(
                        [str(JASH), str(script)], cwd=home, env=environment,
                        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, timeout=20)
                    self.assertEqual(result.returncode, 1,
                                     result.stdout + result.stderr)
                    self.assertIn(b"[ai] command budget exceeded", result.stdout)
                    self.assertNotIn(b"checkpoint=", result.stdout)
                    self.assertFalse(any(home.glob("should-not-exist-*")))

    def test_rm_runs_without_prompt_and_session_rewind_restores_it(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-auto-") as directory:
            base = Path(directory)
            home = base / "home"
            home.mkdir()
            (home / ".jash").mkdir(mode=0o700)
            bindir = base / "bin"
            bindir.mkdir()
            curl = bindir / "curl"
            curl.write_text(
                "#!/usr/bin/python3\n"
                "import json, os\n"
                "command = 'rm -- ' + os.environ['AI_AUTO_TEST_TARGET']\n"
                "print(json.dumps({'choices':[{'message':{'content':"
                "'Ol\u00e1 \U0001f44b\\n```jash\\n' + command + '\\n```'}}]}, separators=(',', ':')))\n",
                encoding="utf-8")
            curl.chmod(0o700)
            target = home / "precious.txt"
            target.write_bytes(b"preserve this data\n")
            script = base / "auto.jsh"
            script.write_text(
                "trash on\n? on auto\n? remove the fixture\nrewind --session\n",
                encoding="utf-8")
            environment = dict(
                os.environ, HOME=str(home), JASH_SIGIL="0", JASH_NUDGE="0",
                JASH_TIMELINE="1", JASH_BRIEF="0", JASH_ADOPT="0",
                AI_AUTO_TEST_TARGET=str(target), POOLSIDE_API_KEY="test",
                PATH=f"{bindir}:/usr/bin:/bin")
            self.install_module(environment, "ai")
            probe = subprocess.run(
                [str(JASH), "-c", "checkpoint probe"], cwd=ROOT,
                env=environment, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=10)
            self.assertEqual(probe.returncode, 0, probe.stderr)
            result = subprocess.run(
                [str(JASH), str(script)], cwd=ROOT, env=environment,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(target.read_bytes(), b"preserve this data\n")
            self.assertNotIn(b"[s/n]", result.stdout)
            self.assertRegex(result.stdout, rb"rewind.*1 reverted")
            self.assertRegex(result.stdout, rb"agent-[0-9a-f-]+")

    def test_named_auto_marker_survives_recorded_agent_calls(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-auto-recorded-") as directory:
            base = Path(directory)
            home = base / "home"
            home.mkdir()
            (home / ".jash").mkdir(mode=0o700)
            bindir = base / "bin"
            bindir.mkdir()
            curl = bindir / "curl"
            curl.write_text(
                "#!/usr/bin/python3\n"
                "import json, os\n"
                "print(json.dumps({'choices':[{'message':{'content':"
                "'```jash\\nrm -- ' + os.environ['AI_AUTO_TEST_TARGET'] + "
                "'\\n```'}}]}, separators=(',', ':')))\n", encoding="utf-8")
            curl.chmod(0o700)
            target = home / "recover-me"
            target.write_bytes(b"must survive\n")
            script = base / "auto.jsh"
            script.write_text("trash on\n? on auto\n? remove fixture\n",
                              encoding="utf-8")
            environment = dict(
                os.environ, HOME=str(home), JASH_SIGIL="2", JASH_NUDGE="0",
                JASH_BRIEF="0", JASH_ADOPT="0", JASH_TIMELINE="1",
                AI_AUTO_TEST_TARGET=str(target), POOLSIDE_API_KEY="test",
                PATH=f"{bindir}:/usr/bin:/bin")
            self.install_module(environment, "sigil")
            self.install_module(environment, "ai")
            first = subprocess.run(
                [str(JASH), str(script)], cwd=ROOT, env=environment,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=20)
            self.assertEqual(first.returncode, 0, first.stderr)
            self.assertFalse(target.exists(), first.stdout)
            match = re.search(rb"checkpoint=(agent-[0-9a-f-]+)", first.stdout)
            self.assertIsNotNone(match, first.stdout)
            name = match.group(1).decode("ascii")
            second = subprocess.run(
                [str(JASH), "-c", f"rewind {name}"], cwd=ROOT,
                env=environment, stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)
            self.assertEqual(second.returncode, 0, second.stderr)
            self.assertEqual(target.read_bytes(), b"must survive\n")
            self.assertIn(b"1 reverted", second.stdout)
            verified = subprocess.run(
                [str(JASH), "-c", "sigil chain verify"], cwd=ROOT,
                env=environment, stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)
            self.assertEqual(verified.returncode, 0, verified.stderr)
            emitted = subprocess.run(
                [str(JASH), "-c", "sigil proof emit"], cwd=ROOT,
                env=environment, stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)
            self.assertEqual(emitted.returncode, 0, emitted.stderr)
            digest = re.search(rb"command_blake3=([0-9a-f]{64})", first.stdout)
            self.assertIsNotNone(digest, first.stdout)
            chains = list((home / ".jash").glob("chain*.log"))
            self.assertEqual(len(chains), 1, chains)
            event = b"ai.auto: checkpoint=" + name.encode() + \
                    b" command_blake3=" + digest.group(1)
            self.assertIn(event, chains[0].read_bytes())
            verifier = base / "jef_verify"
            sources = (
                "libjef/jef_verify_main.c", "libjef/jef_verify.c",
                "libjef/ed25519.c", "c/module/blake3.c",
                "c/module/jash_crypto.c")
            built = subprocess.run(
                ["cc", "-std=gnu11", "-O1", "-I", str(ROOT / "c"),
                 "-I", str(ROOT / "libjef"),
                 *(str(ROOT / path) for path in sources), "-o", str(verifier)],
                cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                timeout=120)
            self.assertEqual(built.returncode, 0, built.stderr)
            proof = subprocess.run(
                [str(verifier), "proof", str(home / ".jash/session.jashproof")],
                cwd=ROOT, env=environment, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=20)
            self.assertEqual(proof.returncode, 0, proof.stdout + proof.stderr)

    def test_checkpoint_refusal_does_not_masquerade_as_snapshot_or_pause(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-auto-refusal-") as directory:
            base = Path(directory)
            home = base / "home"
            home.mkdir()
            (home / ".jash").mkdir(mode=0o700)
            bindir = base / "bin"
            bindir.mkdir()
            curl = bindir / "curl"
            curl.write_text(
                "#!/usr/bin/python3\n"
                "import json, os\n"
                "command = 'printf payload > ' + os.environ['AI_AUTO_TEST_TARGET']\n"
                "print(json.dumps({'choices':[{'message':{'content':"
                "'```jash\\n' + command + '\\n```'}}]}, separators=(',', ':')))\n",
                encoding="utf-8")
            curl.chmod(0o700)
            target = home / "created"
            environment = dict(
                os.environ, HOME=str(home), JASH_SIGIL="0", JASH_NUDGE="0",
                JASH_BRIEF="0", JASH_ADOPT="0", JASH_TIMELINE="1",
                AI_AUTO_TEST_TARGET=str(target), POOLSIDE_API_KEY="test",
                PATH=f"{bindir}:/usr/bin:/bin")
            self.install_module(environment, "ai")
            probe = subprocess.run(
                [str(JASH), "-c", "checkpoint probe"], cwd=ROOT,
                env=environment, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=10)
            self.assertEqual(probe.returncode, 0, probe.stderr)
            checkpoints = list((home / ".jash").glob("checkpoints*.tsv"))
            self.assertEqual(len(checkpoints), 1)
            checkpoints[0].chmod(0o666)  # reject untrusted checkpoint metadata
            script = base / "auto.jsh"
            script.write_text("? on auto\n? create marker\n", encoding="utf-8")
            result = subprocess.run(
                [str(JASH), str(script)], cwd=ROOT, env=environment,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(target.read_bytes(), b"payload")
            self.assertIn(b"checkpoint=unavailable", result.stdout)
            self.assertIn(f"checkpoint=unavailable errno={errno.EBADMSG}".encode(),
                          result.stdout)
            self.assertIn(b"recovery=journaled-only", result.stdout)
            self.assertNotIn(b"[s/n]", result.stdout)

    def test_incomplete_provider_response_cannot_execute(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-auto-partial-") as directory:
            base = Path(directory)
            home = base / "home"
            home.mkdir()
            (home / ".jash").mkdir(mode=0o700)
            bindir = base / "bin"
            bindir.mkdir()
            curl = bindir / "curl"
            curl.write_text(
                "#!/usr/bin/python3\n"
                "import json, os, sys\n"
                "command = 'printf payload > ' + os.environ['AI_AUTO_TEST_TARGET']\n"
                "body = json.dumps({'choices':[{'message':{'content':"
                "'```jash\\n' + command + '\\n```'}}]}, separators=(',', ':'))\n"
                "if os.environ['AI_RESPONSE_MODE'] == 'truncated':\n"
                "    sys.stdout.write(body[:-4])\n"
                "elif os.environ['AI_RESPONSE_MODE'] == 'unclosed':\n"
                "    print(json.dumps({'choices':[{'message':{'content':"
                "        '```jash\\n' + command + '\\n'}}]}, separators=(',', ':')))\n"
                "elif os.environ['AI_RESPONSE_MODE'] == 'misplaced':\n"
                "    print(json.dumps({'content':'```jash\\n' + command + "
                "'\\n```', 'choices':[]}, separators=(',', ':')))\n"
                "elif os.environ['AI_RESPONSE_MODE'] == 'decoy':\n"
                "    print(json.dumps({'content':'```jash\\n' + command + "
                "'\\n```', 'choices':[{'message':{'content':"
                "'plain answer'}}]}, separators=(',', ':')))\n"
                "else:\n"
                "    sys.stdout.write(body)\n"
                "    sys.exit(7)\n", encoding="utf-8")
            curl.chmod(0o700)
            target = home / "unobserved-effect"
            script = base / "auto.jsh"
            script.write_text("? on auto\n? attempt write\nexit $?\n",
                              encoding="utf-8")
            environment = dict(
                os.environ, HOME=str(home), JASH_SIGIL="0", JASH_NUDGE="0",
                JASH_BRIEF="0", JASH_ADOPT="0", JASH_TIMELINE="1",
                AI_AUTO_TEST_TARGET=str(target), POOLSIDE_API_KEY="test",
                PATH=f"{bindir}:/usr/bin:/bin")
            self.install_module(environment, "ai")
            for mode in ("failed", "truncated", "unclosed", "misplaced", "decoy"):
                with self.subTest(mode=mode):
                    environment["AI_RESPONSE_MODE"] = mode
                    result = subprocess.run(
                        [str(JASH), str(script)], cwd=ROOT, env=environment,
                        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, timeout=20)
                    self.assertFalse(target.exists(), result.stdout + result.stderr)
                    if mode == "unclosed":
                        self.assertNotIn(b"checkpoint=", result.stdout)
                    elif mode == "misplaced":
                        self.assertIn(b"[ai] Sem resposta do AI", result.stdout)
                        self.assertNotEqual(result.returncode, 0, result.stdout)
                    elif mode == "decoy":
                        self.assertIn(b"plain answer", result.stdout)
                        self.assertNotIn(b"checkpoint=", result.stdout)
                    else:
                        self.assertIn(b"[ai] incomplete provider response", result.stdout)
                        self.assertNotEqual(result.returncode, 0, result.stdout)

    def test_broken_output_prevents_auto_execution(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-auto-output-") as directory:
            base = Path(directory)
            home = base / "home"
            home.mkdir()
            (home / ".jash").mkdir(mode=0o700)
            bindir = base / "bin"
            bindir.mkdir()
            curl = bindir / "curl"
            curl.write_text(
                "#!/usr/bin/python3\n"
                "import json, os\n"
                "command = 'printf payload > ' + os.environ['AI_AUTO_TEST_TARGET']\n"
                "print(json.dumps({'choices':[{'message':{'content':"
                "'```jash\\n' + command + '\\n```'}}]}, separators=(',', ':')))\n",
                encoding="utf-8")
            curl.chmod(0o700)
            target = home / "unobserved-effect"
            script = base / "auto.jsh"
            script.write_text("? on auto\n? attempt write\n", encoding="utf-8")
            environment = dict(
                os.environ, HOME=str(home), JASH_SIGIL="0", JASH_NUDGE="0",
                JASH_BRIEF="0", JASH_ADOPT="0", JASH_TIMELINE="1",
                AI_AUTO_TEST_TARGET=str(target), POOLSIDE_API_KEY="test",
                PATH=f"{bindir}:/usr/bin:/bin")
            self.install_module(environment, "ai")
            with open("/dev/full", "wb") as full:
                result = subprocess.run(
                    [str(JASH), str(script)], cwd=ROOT, env=environment,
                    stdin=subprocess.DEVNULL, stdout=full,
                    stderr=subprocess.PIPE, timeout=20)
            self.assertFalse(target.exists(), result.stderr)

            # A writer can report a positive short write and recover before
            # the checkpoint banner. Losing part of the answer must still
            # prevent execution.
            source = base / "short_writer.c"
            library = base / "short_writer.so"
            source.write_text(
                "#define _GNU_SOURCE\n"
                "#include <dlfcn.h>\n"
                "#include <string.h>\n"
                "#include <unistd.h>\n"
                "ssize_t write(int fd, const void *buf, size_t n) {\n"
                "  static ssize_t (*next)(int, const void *, size_t);\n"
                "  static int shortened;\n"
                "  if (!next) next = dlsym(RTLD_NEXT, \"write\");\n"
                "  if (fd == 1 && !shortened && memmem(buf, n, \"Resposta\", 8)) {\n"
                "    shortened = 1;\n"
                "    return next(fd, buf, n / 2);\n"
                "  }\n"
                "  return next(fd, buf, n);\n"
                "}\n", encoding="utf-8")
            built = subprocess.run(
                ["cc", "-std=gnu11", "-shared", "-fPIC", str(source),
                 "-o", str(library), "-ldl"], cwd=ROOT,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
            self.assertEqual(built.returncode, 0, built.stderr)
            environment["LD_PRELOAD"] = str(library)
            result = subprocess.run(
                [str(JASH), str(script)], cwd=ROOT, env=environment,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=20)
            self.assertFalse(target.exists(), result.stdout + result.stderr)

    def test_unavailable_sigil_does_not_prevent_auto_or_claim_a_proof(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-auto-sigil-failure-") as directory:
            base = Path(directory)
            home = base / "home"
            home.mkdir()
            state = home / ".jash"
            state.mkdir(mode=0o700)
            # In a non-repo cwd this is Sigil's chain path. A directory
            # prevents an append without interfering with checkpoint storage.
            (state / "chain.log").mkdir(mode=0o700)
            bindir = base / "bin"
            bindir.mkdir()
            curl = bindir / "curl"
            curl.write_text(
                "#!/usr/bin/python3\n"
                "import json, os\n"
                "command = '/usr/bin/touch ' + os.environ['AI_AUTO_TEST_TARGET']\n"
                "print(json.dumps({'choices':[{'message':{'content':"
                "'```jash\\n' + command + '\\n```'}}]}, separators=(',', ':')))\n",
                encoding="utf-8")
            curl.chmod(0o700)
            target = home / "ran-without-chain"
            script = base / "auto.jsh"
            script.write_text("? on auto\n? create fixture\n", encoding="utf-8")
            environment = dict(
                os.environ, HOME=str(home), JASH_SIGIL="2", JASH_NUDGE="0",
                JASH_BRIEF="0", JASH_ADOPT="0", JASH_TIMELINE="1",
                AI_AUTO_TEST_TARGET=str(target), POOLSIDE_API_KEY="test",
                PATH=f"{bindir}:/usr/bin:/bin")
            self.install_module(environment, "ai")
            self.install_module(environment, "sigil")
            result = subprocess.run(
                [str(JASH), str(script)], cwd=home, env=environment,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(target.is_file())
            self.assertRegex(result.stdout, rb"checkpoint=agent-[0-9a-f-]+")
            self.assertIn(b"recovery=journaled-only", result.stdout)
            self.assertNotIn(b"proof=", result.stdout)
            self.assertTrue((state / "chain.log").is_dir())
            self.assertFalse(list(state.glob("chain-*.log")))
            self.assertFalse((state / "session.jashproof").exists())

    def test_native_copy_move_and_mode_have_observed_rewind_coverage(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-auto-effects-") as directory:
            base = Path(directory)
            home = base / "home"
            home.mkdir()
            (home / ".jash").mkdir(mode=0o700)
            bindir = base / "bin"
            bindir.mkdir()
            curl = bindir / "curl"
            curl.write_text(
                "#!/usr/bin/python3\n"
                "import json, os\n"
                "command = os.environ['AI_AUTO_TEST_COMMAND']\n"
                "print(json.dumps({'choices':[{'message':{'content':"
                "'```jash\\n' + command + '\\n```'}}]}, separators=(',', ':')))\n",
                encoding="utf-8")
            curl.chmod(0o700)
            source = home / "source"
            source.write_bytes(b"new content")
            existing = home / "existing"
            existing.write_bytes(b"old content")
            created = home / "copy-created"
            moving = home / "move-source"
            moving.write_bytes(b"move content")
            moved = home / "move-dest"
            meta = home / "mode-target"
            meta.write_bytes(b"mode content")
            meta.chmod(0o644)
            tracked = (source, existing, created, moving, moved, meta)
            script = base / "auto.jsh"
            script.write_text("? on auto\n? change fixture\nrewind --session\n",
                              encoding="utf-8")
            environment = dict(
                os.environ, HOME=str(home), JASH_SIGIL="0", JASH_NUDGE="0",
                JASH_BRIEF="0", JASH_ADOPT="0", JASH_TIMELINE="1",
                POOLSIDE_API_KEY="test", PATH=f"{bindir}:/usr/bin:/bin")
            self.install_module(environment, "ai")

            def snapshot():
                return {str(path): (path.read_bytes(), path.stat().st_mode & 0o777)
                        if path.exists() else None for path in tracked}

            for kind, command in (
                    ("copy-create", f"cp -- {source} {created}"),
                    ("copy-overwrite", f"cp -- {source} {existing}"),
                    ("move", f"mv -- {moving} {moved}"),
                    ("chmod", f"chmod 600 {meta}")):
                with self.subTest(effect=kind):
                    before = snapshot()
                    environment["AI_AUTO_TEST_COMMAND"] = command
                    result = subprocess.run(
                        [str(JASH), str(script)], cwd=home, env=environment,
                        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, timeout=20)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn(b"timeline=0->1", result.stdout)
                    self.assertIn(b"1 reverted", result.stdout)
                    self.assertEqual(snapshot(), before, result.stdout)

    def test_failed_auto_command_propagates_status_without_approval(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-auto-status-") as directory:
            base = Path(directory)
            home = base / "home"
            home.mkdir()
            (home / ".jash").mkdir(mode=0o700)
            bindir = base / "bin"
            bindir.mkdir()
            curl = bindir / "curl"
            curl.write_text(
                "#!/usr/bin/python3\n"
                "import json\n"
                "print(json.dumps({'choices':[{'message':{'content':"
                "'```jash\\n/usr/bin/false\\n```'}}]}, separators=(',', ':')))\n",
                encoding="utf-8")
            curl.chmod(0o700)
            script = base / "auto.jsh"
            script.write_text("? on auto\n? attempt fixture\nexit $?\n",
                              encoding="utf-8")
            environment = dict(
                os.environ, HOME=str(home), JASH_SIGIL="0", JASH_NUDGE="0",
                JASH_BRIEF="0", JASH_ADOPT="0", JASH_TIMELINE="1",
                POOLSIDE_API_KEY="test", PATH=f"{bindir}:/usr/bin:/bin")
            self.install_module(environment, "ai")
            result = subprocess.run(
                [str(JASH), str(script)], cwd=home, env=environment,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=20)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn(b"[ai:auto] rc=1 timeline=0->0", result.stdout)
            self.assertNotIn(b"[s/n]", result.stdout)

    def test_external_write_is_not_claimed_as_undoable(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-auto-external-") as directory:
            base = Path(directory)
            home = base / "home"
            home.mkdir()
            (home / ".jash").mkdir(mode=0o700)
            bindir = base / "bin"
            bindir.mkdir()
            curl = bindir / "curl"
            curl.write_text(
                "#!/usr/bin/python3\n"
                "import json, os\n"
                "command = '/usr/bin/touch ' + os.environ['AI_AUTO_TEST_TARGET']\n"
                "print(json.dumps({'choices':[{'message':{'content':"
                "'```jash\\n' + command + '\\n```'}}]}, separators=(',', ':')))\n",
                encoding="utf-8")
            curl.chmod(0o700)
            target = home / "external-created"
            script = base / "auto.jsh"
            script.write_text(
                "? on auto\n? create fixture\nrewind --session\n",
                encoding="utf-8")
            environment = dict(
                os.environ, HOME=str(home), JASH_SIGIL="0", JASH_NUDGE="0",
                JASH_TIMELINE="1", JASH_BRIEF="0", JASH_ADOPT="0",
                AI_AUTO_TEST_TARGET=str(target), POOLSIDE_API_KEY="test",
                PATH=f"{bindir}:/usr/bin:/bin")
            self.install_module(environment, "ai")
            result = subprocess.run(
                [str(JASH), str(script)], cwd=ROOT, env=environment,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(target.is_file())
            self.assertIn(b"recovery=journaled-only", result.stdout)
            self.assertIn(b"timeline=0->0", result.stdout)
            self.assertNotIn(b"[s/n]", result.stdout)

    def test_explicit_trash_off_is_not_overridden_by_auto(self):
        with tempfile.TemporaryDirectory(prefix="jash-ai-auto-trash-off-") as directory:
            base = Path(directory)
            home = base / "home"
            home.mkdir()
            (home / ".jash").mkdir(mode=0o700)
            bindir = base / "bin"
            bindir.mkdir()
            curl = bindir / "curl"
            curl.write_text(
                "#!/usr/bin/python3\n"
                "import json, os\n"
                "command = 'rm -- ' + os.environ['AI_AUTO_TEST_TARGET']\n"
                "print(json.dumps({'choices':[{'message':{'content':"
                "'```jash\\n' + command + '\\n```'}}]}, separators=(',', ':')))\n",
                encoding="utf-8")
            curl.chmod(0o700)
            target = home / "user-chose-permanent"
            target.write_bytes(b"fixture only")
            script = base / "auto.jsh"
            script.write_text(
                "trash off\n? on auto\n? remove fixture\nrewind --session\n",
                encoding="utf-8")
            environment = dict(
                os.environ, HOME=str(home), JASH_SIGIL="0", JASH_NUDGE="0",
                JASH_TIMELINE="1", JASH_BRIEF="0", JASH_ADOPT="0",
                AI_AUTO_TEST_TARGET=str(target), POOLSIDE_API_KEY="test",
                PATH=f"{bindir}:/usr/bin:/bin")
            self.install_module(environment, "ai")
            result = subprocess.run(
                [str(JASH), str(script)], cwd=ROOT, env=environment,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(target.exists())
            self.assertIn(b"trash=off", result.stdout)
            self.assertIn(b"timeline=0->0", result.stdout)
            self.assertNotIn(b"[s/n]", result.stdout)


if __name__ == "__main__":
    unittest.main()
