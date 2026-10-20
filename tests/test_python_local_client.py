"""The Python SDK uses the existing local capability boundary, not a new API."""

import base64
import importlib.util
import json
import os
from pathlib import Path
import secrets
import subprocess
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
AGENT = Path(os.environ.get("JASH_AGENTD_BIN", ROOT / "build/jash-agentd")).resolve()
spec = importlib.util.spec_from_file_location(
    "jash_client", ROOT / "integrations/jash_client.py")
jash_client = importlib.util.module_from_spec(spec)
spec.loader.exec_module(jash_client)


class PythonLocalClientTests(unittest.TestCase):
    def test_read_only_default_denies_mutation_and_explicit_cap_works(self):
        with tempfile.TemporaryDirectory(prefix="jash-python-client-") as temporary:
            root = Path(temporary)
            scope = root / "scope"
            scope.write_text(secrets.token_hex(32), encoding="ascii")
            scope.chmod(0o600)
            socket = root / "socket"
            env = {k: v for k, v in os.environ.items() if not k.startswith("JASH_")}
            env.update(HOME=str(root), JASH_SIGIL="0", JASH_TIMELINE="0")
            server = subprocess.Popen(
                [str(AGENT), "--listen", str(socket),
                 "--scope-file", str(scope)], cwd=root, env=env,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                deadline = time.monotonic() + 5
                while not socket.exists() and server.poll() is None:
                    if time.monotonic() >= deadline:
                        self.fail("local server did not start")
                    time.sleep(.01)
                self.assertTrue(socket.exists())
                with jash_client.JashLocalClient(socket, scope) as client:
                    read = client.request("pwd", ["pwd"])
                    self.assertEqual(read["rc"], 0, read)
                    self.assertIn(str(root), read["stdout"])
                    target = root / "changed"
                    denied = client.request("touch-denied", ["touch", str(target)])
                    self.assertNotEqual(denied["rc"], 0, denied)
                    self.assertFalse(target.exists())
                    allowed = client.request(
                        "touch-allowed", ["touch", str(target)],
                        caps=("effect:mutate",))
                    self.assertEqual(allowed["rc"], 0, allowed)
                    self.assertTrue(target.exists())
                    self.assertEqual(client.request("pwd", ["pwd"]), read)
                    encoded = base64.b64encode(b"durable-data").decode("ascii")
                    denied_import = client.control(
                        "import-denied", "data.import", transport="inline",
                        data_b64=encoded)
                    self.assertEqual(denied_import["error"], "capabilities required")
                    self.assertEqual(client.control(
                        "import-wrong-cap", "data.import", caps=("data:read",),
                        transport="inline", data_b64=encoded)["error"],
                        "capability denied")
                    imported = client.control(
                        "import-allowed", "data.import", caps=("data:create",),
                        transport="inline", data_b64=encoded)
                    self.assertTrue(imported["ok"], imported)
                    denied_promote = client.control(
                        "promote-denied", "data.promote",
                        artifact_ref=imported["artifact_ref"])
                    self.assertEqual(denied_promote["error"], "capabilities required")
                    promoted = client.control(
                        "promote-once", "data.promote", caps=("data:manage",),
                        artifact_ref=imported["artifact_ref"])
                    self.assertTrue(promoted["ok"], promoted)
                    self.assertTrue(promoted["artifact_ref"].startswith("a:"))
                    self.assertEqual(client.control(
                        "promote-once", "data.promote", caps=("data:manage",),
                        artifact_ref=imported["artifact_ref"]), promoted)
                    attached = client.control(
                        "attach-durable", "data.attach", caps=("data:read",),
                        artifact_ref=promoted["artifact_ref"])
                    self.assertTrue(attached["ok"], attached)
                    readback = client.control(
                        "read-durable", "data.read", caps=("data:read",),
                        artifact_ref=attached["artifact_ref"], offset=0, limit=12)
                    self.assertEqual(base64.b64decode(readback["data_b64"]),
                                     b"durable-data")
                    denied_job = client.control(
                        "job-denied", "job.start", caps=("job:read",),
                        argv=["/usr/bin/printf", "job-data"], background=True)
                    self.assertIn("effect:exec and job:create", denied_job["error"])
                    started = client.control(
                        "job-start", "job.start",
                        caps=("effect:exec", "job:create"),
                        argv=["/usr/bin/printf", "job-data"], background=True)
                    self.assertTrue(started["accepted"], started)
                    terminal = client.control(
                        "job-wait", "job.wait", caps=("job:read",),
                        job_id=started["job_id"], wait_ms=5000)
                    self.assertEqual(terminal["rc"], 0, terminal)
                    self.assertEqual(terminal["state"], "done")
                    self.assertTrue(terminal["receipt_ref"].endswith(".job-terminal.dsse"))
                    page = client.control(
                        "job-page", "job.read", caps=("job:read",),
                        job_id=started["job_id"], stream="stdout", offset=0,
                        limit=64)
                    self.assertEqual(base64.b64decode(page["data_b64"]), b"job-data")
                    long_job = client.control(
                        "job-cancel-start", "job.start",
                        caps=("effect:exec", "job:create"),
                        argv=["/usr/bin/sleep", "30"], background=True)
                    self.assertTrue(long_job["accepted"], long_job)
                    stopped = client.control(
                        "job-cancel", "job.cancel", caps=("job:signal",),
                        job_id=long_job["job_id"], grace_ms=0)
                    self.assertNotIn("error", stopped, stopped)
                    cancelled = client.control(
                        "job-cancel-wait", "job.wait", caps=("job:read",),
                        job_id=long_job["job_id"], wait_ms=5000)
                    self.assertTrue(cancelled["terminal"], cancelled)
                    self.assertNotEqual(cancelled["rc"], 0, cancelled)
                scope.chmod(0o644)
                with self.assertRaises(jash_client.JashProtocolError):
                    with jash_client.JashLocalClient(socket, scope):
                        self.fail("world-readable scope was accepted")
                scope.chmod(0o600)
                server.terminate()
                server.wait(timeout=5)
                server.stdout.close()
                server.stderr.close()
                server = subprocess.Popen(
                    [str(AGENT), "--listen", str(socket),
                     "--scope-file", str(scope)], cwd=root, env=env,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                with jash_client.JashLocalClient(socket, scope) as again:
                    archived = again.control(
                        "job-result-restart", "job.result", caps=("job:read",),
                        job_id=started["job_id"])
                    self.assertTrue(archived["available"], archived)
                    terminal_archive = json.loads(archived["response_json"])
                    self.assertEqual(terminal_archive["rc"], 0, archived)
                    self.assertTrue(terminal_archive["receipt_ref"].endswith(
                        ".job-terminal.dsse"), archived)
            finally:
                if server.poll() is None:
                    server.terminate()
                    try:
                        server.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        server.kill()
                        server.wait(timeout=5)
                server.stdout.close()
                server.stderr.close()


if __name__ == "__main__":
    unittest.main()
