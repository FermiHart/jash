"""MCP stdio adapter exposes only bounded native read-only Jash tools."""

import json
import os
from pathlib import Path
import select
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class MCPTests(unittest.TestCase):
    def test_stdio_initialize_discover_and_call_read_only(self):
        with tempfile.TemporaryDirectory(prefix="jash-mcp-") as temporary:
            base = Path(temporary)
            directory = base / "observed"
            directory.mkdir()
            (directory / "one").touch()
            (directory / "two").touch()
            project = base / "project"
            project.mkdir()
            (project / "fix.h").write_text(
                "static int answer(void) { return 42; }\n", encoding="utf-8")
            target = base / "not-created"
            home = base / "home"
            home.mkdir()
            (home / ".jash").mkdir(mode=0o700)
            source = base / "module-sources" / "ai"
            source.mkdir(parents=True)
            for filename in ("ai_module.c", "manifest.toml"):
                shutil.copy2(ROOT / "modules/ai" / filename, source / filename)
            env = dict(os.environ, HOME=str(home), JASH_SIGIL="0",
                       JASH_NUDGE="0", JASH_BRIEF="0", PATH="/usr/bin:/bin")
            env.pop("POOLSIDE_API_KEY", None)
            built = subprocess.run(
                ["bash", str(ROOT / "modules/build_module.sh"), str(source)],
                cwd=ROOT, env=env, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=120)
            self.assertEqual(built.returncode, 0, built.stderr)
            server = subprocess.Popen(
                ["python3", "-B", str(ROOT / "integrations/jash_mcp.py"),
                 "--jash", str(ROOT / "build/jash")], cwd=project, env=env,
                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE)
            try:
                def request(ident, method, params=None):
                    value = {"jsonrpc": "2.0", "id": ident, "method": method}
                    if params is not None:
                        value["params"] = params
                    server.stdin.write(json.dumps(value).encode() + b"\n")
                    server.stdin.flush()
                    self.assertTrue(select.select([server.stdout], [], [], 10)[0],
                                    "MCP response timed out")
                    answer = server.stdout.readline()
                    self.assertTrue(answer, "MCP server exited early")
                    parsed = json.loads(answer)
                    self.assertEqual(parsed["id"], ident)
                    return parsed

                invalid_init = request(-1, "initialize", {"protocolVersion": 5})
                self.assertEqual(invalid_init["error"]["code"], -32602)
                premature = request(0, "tools/list")
                self.assertEqual(premature["error"]["code"], -32000)
                initialized = request(1, "initialize", {
                    "protocolVersion": "2025-06-18", "capabilities": {},
                    "clientInfo": {"name": "test", "version": "1"}})
                self.assertEqual(initialized["result"]["protocolVersion"],
                                 "2025-06-18")
                catalog = request(2, "tools/list")
                self.assertEqual({tool["name"] for tool in catalog["result"]["tools"]},
                                 {"fs.list", "fs.count", "fs.observation.read",
                                  "fs.source.read"})
                counted = request(3, "tools/call", {"name": "fs.count",
                          "arguments": {"root": str(directory), "type": "file"}})
                self.assertEqual(counted["result"]["structuredContent"]["items_seen"], 2)
                self.assertFalse(counted["result"]["isError"])
                listed = request(4, "tools/call", {"name": "fs.list",
                          "arguments": {"root": str(directory)}})
                self.assertEqual(listed["result"]["structuredContent"]["items_seen"], 2)
                reference = listed["result"]["structuredContent"]["observation_ref"]
                self.assertTrue(reference.startswith("o:"))
                page = request(6, "tools/call", {"name": "fs.observation.read",
                        "arguments": {"ref": "latest", "offset": 0, "limit": 16}})
                self.assertEqual(page["result"]["structuredContent"]["bytes_returned"], 16)
                self.assertEqual(page["result"]["structuredContent"]["observation_ref"],
                                 reference)
                refreshed = request(7, "tools/call", {"name": "fs.list",
                            "arguments": {"root": str(directory)}})
                self.assertNotEqual(refreshed["result"]["structuredContent"]["observation_ref"],
                                    reference)
                stale = request(8, "tools/call", {"name": "fs.observation.read",
                        "arguments": {"ref": reference, "offset": 0, "limit": 16}})
                self.assertTrue(stale["result"]["isError"])
                source_page = request(9, "tools/call", {
                    "name": "fs.source.read",
                    "arguments": {"path": "fix.h", "limit": 256}})
                self.assertEqual(source_page["result"]["structuredContent"]["schema"],
                                 "jash.source-page.v1")
                self.assertIn("return 42", source_page["result"]["structuredContent"]["text"])
                denied = request(5, "tools/call", {"name": "exec",
                         "arguments": {"root": f"$(touch {target})"}})
                self.assertEqual(denied["error"]["code"], -32602)
                server.stdin.write(
                    b'{"jsonrpc":"2.0","id":6,"method":"tools/call",'
                    b'"params":{"name":"exec","name":"fs.count",'
                    b'"arguments":{"root":"."}}}\n')
                server.stdin.flush()
                self.assertTrue(select.select([server.stdout], [], [], 10)[0])
                duplicate = json.loads(server.stdout.readline())
                self.assertEqual(duplicate["error"]["code"], -32700)
                self.assertFalse(target.exists())
            finally:
                server.stdin.close()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.terminate()
                    server.wait(timeout=5)
                server.stdout.close()
                server.stderr.close()


if __name__ == "__main__":
    unittest.main()
