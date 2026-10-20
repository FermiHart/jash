#!/usr/bin/env python3
"""Real delegated cgroup jobs and independently checked resource receipts."""
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time
import unittest

from cgroup_fixture import delegated_root, CgroupUnavailable

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("JASH_BUILD_DIR", ROOT / "build"))
BIN = os.environ.get("JASH_AGENTD_BIN", str(BUILD / "jash-agentd"))
VERIFIER = os.environ.get("JASH_VERIFIER_BIN", str(BUILD / "jef_verify"))
LIMITS = dict(memory_max_bytes=64 * 1024 * 1024, pids_max=16,
              cpu_quota_us=50000, cpu_period_us=100000, wall_timeout_ms=2000)


class ResourceProtocol(unittest.TestCase):
    def setUp(self):
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.root = Path(self.stack.enter_context(tempfile.TemporaryDirectory(prefix="resource-protocol-")))
        try:
            self.cgroup = self.stack.enter_context(delegated_root())
        except CgroupUnavailable as exc:
            self.skipTest(str(exc))
        self.start_daemon(self.cgroup)

    def start_daemon(self, root):
        self.home = self.root / ("home-" + str(len(list(self.root.iterdir()))))
        self.home.mkdir()
        env = dict(HOME=str(self.home), PATH="/usr/bin:/bin", LANG="C")
        if root is not None:
            env["JASH_AGENT_CGROUP_ROOT"] = str(root)
        self.proc = subprocess.Popen([BIN], cwd=self.root, env=env, text=True,
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE)
        self.addCleanup(self.close_daemon, self.proc)
        self.hello = json.loads(self.line())
        self.key = self.hello["receipts"]["pubkey"]

    @staticmethod
    def close_daemon(proc):
        if proc.poll() is None:
            proc.stdin.close()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
        proc.stdout.close()
        proc.stderr.close()

    def line(self):
        self.assertTrue(select.select([self.proc.stdout], [], [], 10)[0], "daemon response timeout")
        raw = self.proc.stdout.readline()
        self.assertTrue(raw, "daemon exited")
        return raw

    def exchange(self, body, raw=None):
        raw = raw or json.dumps(body, separators=(",", ":"))
        self.proc.stdin.write(raw + "\n")
        self.proc.stdin.flush()
        response = self.line()
        prefix = self.root / body["id"]
        request_path = prefix.with_suffix(".request.json")
        response_path = prefix.with_suffix(".response.json")
        request_path.write_text(raw + "\n")
        response_path.write_text(response)
        return json.loads(response), request_path, response_path

    def test_requested_limits_are_effective_and_receipt_bound(self):
        spec = self.root / "spec.json"
        spec.write_text(json.dumps({"resources": LIMITS, "purpose": "bounded test"}))
        body = dict(id="quota", protocol_version=2, op="job.start", argv=["/bin/true"],
                    background=True, resources=LIMITS, caps=["effect:exec", "job:create"],
                    execution_spec_sha256="sha256:" + hashlib.sha256(spec.read_bytes()).hexdigest())
        start, request_path, start_response = self.exchange(body)
        self.assertNotIn("error", start, start)
        self.assertEqual(start["resources"], LIMITS)
        self.assertIs(start["resource_enforced"], True)
        terminal, wait_request, terminal_response = self.exchange(dict(
            id="quota-wait", protocol_version=2, op="job.wait", job_id=start["job_id"],
            wait_ms=5000, caps=["job:read"]))
        self.assertNotIn("error", terminal, terminal)
        self.assertEqual(terminal["rc"], 0)
        self.assertEqual(terminal["resources"], LIMITS)
        self.assertIs(terminal["resource_empty"], True)
        self.assertIs(terminal["resource_violation"], False)
        receipts = self.home / ".jash/agent_receipts"
        args = [VERIFIER, "job-spec", self.key, str(spec), str(request_path),
                str(start_response), str(receipts / start["receipt_ref"]), str(wait_request),
                str(terminal_response), str(receipts / terminal["receipt_ref"])]
        self.assertEqual(subprocess.run(args, capture_output=True, timeout=10).returncode, 0)
        tampered = terminal_response.read_text().replace('"resource_empty":true', '"resource_empty":false')
        self.assertNotEqual(tampered, terminal_response.read_text())
        terminal_response.write_text(tampered)
        self.assertNotEqual(subprocess.run(args, capture_output=True, timeout=10).returncode, 0)

    def test_invalid_resource_profiles_refuse_before_payload(self):
        marker = self.root / "must-not-exist"
        base = dict(protocol_version=2, op="job.start", background=True,
                    argv=["/usr/bin/touch", str(marker)], caps=["effect:exec", "job:create"])
        invalid = [None, {}, [], {**LIMITS, "unknown": 1},
                   {k: v for k, v in LIMITS.items() if k != "pids_max"}]
        for field in LIMITS:
            invalid.extend({**LIMITS, field: v} for v in (True, "10", 1.5, 0, -1, 2 ** 64))
        for i, resources in enumerate(invalid):
            response, _, _ = self.exchange(dict(base, id="bad" + str(i), resources=resources))
            self.assertIn("error", response, response)
            self.assertFalse(marker.exists())
        for i, bad in enumerate((
                json.dumps(LIMITS).replace('"pids_max": 16', '"pids_max": 016'),
                json.dumps(LIMITS).replace('"pids_max": 16', '"pids_max": 16,"pids_max":16'),
                json.dumps(LIMITS).replace('"pids_max": 16', '"pids_max": 16,"pids_\\u006dax":16'))):
            body = dict(base, id="duplicate" + str(i), resources=LIMITS)
            raw = json.dumps(body).replace(json.dumps(LIMITS), bad)
            response, _, _ = self.exchange(body, raw=raw)
            self.assertIn("error", response, response)
            self.assertFalse(marker.exists())

    def test_resources_require_delegation_and_cannot_override_config(self):
        marker = self.root / "must-not-exist"
        base = dict(id="override", protocol_version=2, op="job.start", background=True,
                    argv=["/usr/bin/touch", str(marker)], caps=["effect:exec", "job:create"],
                    resources=LIMITS)
        response, _, _ = self.exchange(dict(base, env={"JASH_AGENT_CGROUP_ROOT": "/tmp"}))
        self.assertIn("protected env", response["error"])
        self.close_daemon(self.proc)
        self.start_daemon(None)
        self.assertFalse(self.hello["jobs"]["resources"]["configured"])
        response, _, _ = self.exchange(dict(base, id="no-delegation"))
        self.assertIn("error", response)
        self.assertFalse(marker.exists())
        legacy = {k: v for k, v in base.items() if k != "resources"}
        legacy.update(id="legacy", argv=["/bin/true"])
        response, _, _ = self.exchange(legacy)
        self.assertNotIn("error", response)
        self.assertNotIn("resource_enforced", response)

    def test_normal_directory_is_not_a_resource_backend(self):
        self.close_daemon(self.proc)
        directory = self.root / "not-cgroup"
        directory.mkdir()
        self.start_daemon(directory)
        marker = self.root / "not-run"
        response, _, _ = self.exchange(dict(
            id="fake", protocol_version=2, op="job.start", background=True,
            argv=["/usr/bin/touch", str(marker)], caps=["effect:exec", "job:create"], resources=LIMITS))
        self.assertIn("error", response)
        self.assertFalse(marker.exists())
        self.assertEqual(list(directory.iterdir()), [])

    def test_deadline_fires_without_client_polling(self):
        marker = self.root / "late"
        start, _, _ = self.exchange(dict(
            id="deadline", protocol_version=2, op="job.start", background=True,
            argv=["/usr/bin/python3", "-c",
                  "import time,pathlib;time.sleep(1);pathlib.Path(" + repr(str(marker)) + ").touch()"],
            caps=["effect:exec", "job:create"], resources={**LIMITS, "wall_timeout_ms": 100}))
        self.assertNotIn("error", start, start)
        time.sleep(0.35)
        terminal, _, _ = self.exchange(dict(
            id="deadline-wait", protocol_version=2, op="job.wait", job_id=start["job_id"],
            wait_ms=5000, caps=["job:read"]))
        self.assertTrue(terminal["resource_timed_out"])
        self.assertTrue(terminal["resource_violation"])
        self.assertTrue(terminal["resource_empty"])
        self.assertFalse(marker.exists())


if __name__ == "__main__":
    result = unittest.main(verbosity=2, exit=False).result
    sys.exit(1 if not result.wasSuccessful() else 77 if result.skipped else 0)
