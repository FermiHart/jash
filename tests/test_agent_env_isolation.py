#!/usr/bin/env python3
"""Real-runtime env isolation; argv: jash-agentd [--artifacts NEW_DIRECTORY]."""

import argparse
import base64
import json
import os
from pathlib import Path
import select
import shutil
import subprocess
import tempfile
import time
import unittest


class RuntimeCase(unittest.TestCase):
    extra_environment = {}

    def setUp(self):
        self.home = ROOT / self._testMethodName
        self.home.mkdir()
        self.environment = {"HOME": str(self.home), "PATH": "/usr/bin:/bin",
                            "TMPDIR": str(self.home), "LC_ALL": "C",
                             "R" * 63: "ORIGINAL"}
        self.environment.update(self.extra_environment)
        self.serial = 0
        self.proc = None
        self.addCleanup(self.stop)
        self.start()

    def start(self):
        self.proc = subprocess.Popen(
            [str(BINARY)], cwd=self.home, env=self.environment,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.pending = b""
        self.handshake = json.loads(self.readline())
        self.assertEqual(self.handshake["runtime"], "jash-agentd")
        (self.home / "handshake.json").write_text(json.dumps(self.handshake))

    def stop(self):
        proc, self.proc = self.proc, None
        if proc is None:
            return
        if proc.poll() is None:
            proc.stdin.close()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
                self.fail("runtime did not exit after EOF")
        stderr = proc.stderr.read()
        proc.stdout.close()
        proc.stderr.close()
        self.assertEqual(proc.returncode, 0, stderr)

    def readline(self):
        deadline = time.monotonic() + 10
        while b"\n" not in self.pending:
            remaining = deadline - time.monotonic()
            self.assertGreater(remaining, 0, "runtime response timed out")
            ready, _, _ = select.select([self.proc.stdout], [], [], remaining)
            self.assertTrue(ready, "runtime response timed out")
            chunk = os.read(self.proc.stdout.fileno(), 65536)
            self.assertTrue(chunk, "runtime closed response stream")
            self.pending += chunk
        line, self.pending = self.pending.split(b"\n", 1)
        return line + b"\n"

    def request(self, **body):
        self.serial += 1
        body = {"id": f"env-{self.serial}", "caps": ["effect:exec"], **body}
        wire = json.dumps(body, separators=(",", ":")) + "\n"
        request_path = self.home / f"{self.serial}-request.json"
        response_path = self.home / f"{self.serial}-response.json"
        request_path.write_text(wire)
        self.proc.stdin.write(wire.encode())
        self.proc.stdin.flush()
        frames = []
        while True:
            raw = self.readline()
            row = json.loads(raw)
            frames.append(raw)
            if row.get("event") not in ("start", "chunk"):
                break
        (self.home / f"{self.serial}-frames.jsonl").write_bytes(b"".join(frames))
        response_path.write_bytes(raw)
        self.assertEqual(row["id"], body["id"])
        if "receipt_ref" in row:
            key = self.handshake["receipts"]["pubkey"]
            self.assertEqual(row["receipt_pubkey"], key)
            command = [str(VERIFIER), "runtime-receipt", key]
            if "op" in body:
                command = [str(VERIFIER), "job-receipt", key, row["receipt_phase"]]
            command += [str(request_path), str(response_path),
                        str(self.home / ".jash/agent_receipts" / row["receipt_ref"])]
            verified = subprocess.run(command, env=self.environment, timeout=10,
                                      capture_output=True, text=True)
            (self.home / f"{self.serial}-verify.txt").write_text(
                verified.stdout + verified.stderr)
            self.assertEqual(verified.returncode, 0, verified.stdout + verified.stderr)
        elif "error" not in row and body.get("op") != "job.read":
            self.fail(f"completed execution without receipt: {row}")
        return row

    def absent(self, key):
        row = self.request(argv=[PRINTENV, key])
        self.assertEqual((row.get("rc"), row.get("stdout")), (1, ""), row)

    def rejected(self, row, marker):
        self.assertFalse(marker.exists(), f"rejected request executed: {row}")
        self.assertIn("env", row.get("error", ""), row)
        self.assertNotIn("receipt_ref", row, row)
        self.assertNotIn("job_id", row, row)

class EnvIsolation(RuntimeCase):
    def test_sync_overlong_rejected_without_leak(self):
        for version in (1, 2):
            for length in (64, 65, 255, 256):
                with self.subTest(version=version, length=length):
                    key = f"K{version}_{length}_".ljust(length, "K")
                    marker = self.home / f"marker-{version}-{length}"
                    row = self.request(
                        request_version=version, argv=[TOUCH, str(marker)],
                        env={"BEFORE_BAD_KEY": "must-not-apply", key: "PREVIOUS_SECRET"},
                    )
                    # Probe before asserting rejection so the red run records
                    # the leaked secret with an independently verified receipt.
                    leaked = self.request(argv=[PRINTENV, key[:63]])
                    self.assertEqual((leaked.get("rc"), leaked.get("stdout")),
                                     (1, ""), leaked)
                    self.absent("BEFORE_BAD_KEY")
                    self.rejected(row, marker)

    def test_sync_63_byte_roundtrip_and_restore(self):
        key = "V" * 63
        for version in (1, 2):
            with self.subTest(version=version):
                row = self.request(request_version=version, argv=[PRINTENV, key],
                                   env={key: "BOUNDARY_OK"})
                self.assertEqual(row.get("rc"), 0, row)
                stdout = (row["stdout"] if version == 1 else
                          (self.home / ".jash/overflow" / row["stdout_ref"]).read_text())
                self.assertEqual(stdout, "BOUNDARY_OK\n", row)
                self.absent(key)
        key = "R" * 63
        row = self.request(argv=[PRINTENV, key], env={key: "TEMPORARY"})
        self.assertEqual(row.get("stdout"), "TEMPORARY\n", row)
        row = self.request(argv=[PRINTENV, key])
        self.assertEqual(row.get("stdout"), "ORIGINAL\n", row)

    def test_loader_prefixes_rejected_before_effect(self):
        for mode in ("sync-v1", "sync-v2", "exec", "job.start"):
            for key in ("LD_PRELOAD", "LD_AUDIT", "LD_", "LD_FUTURE",
                        "DYLD_INSERT_LIBRARIES", "DYLD_", "DYLD_FUTURE"):
                with self.subTest(mode=mode, key=key):
                    marker = self.home / f"marker-{mode}-{key}"
                    fields = {"request_version": 1 if mode == "sync-v1" else 2}
                    if mode in ("exec", "job.start"):
                        fields.update(op=mode, caps=["effect:exec", "job:create"])
                    if mode == "job.start":
                        fields["background"] = True
                    row = self.request(argv=[TOUCH, str(marker)], env={key: ""}, **fields)
                    if "job_id" in row and mode == "job.start":
                        self.request(request_version=2, op="job.wait", job_id=row["job_id"],
                                     wait_ms=2000, caps=["job:read"])
                    self.absent(key)
                    self.rejected(row, marker)

    def test_job_long_keys_roundtrip_without_daemon_overlay(self):
        for length in (63, 64, 255):
            with self.subTest(length=length):
                key = "J" * length
                row = self.request(request_version=2, op="exec",
                                   caps=["effect:exec", "job:create"],
                                   argv=[PRINTENV, key], env={key: "JOB_VALUE"})
                self.assertEqual(row.get("rc"), 0, row)
                output = self.request(request_version=2, op="job.read", job_id=row["job_id"],
                                      stream="stdout", offset=0, limit=4096, caps=["job:read"])
                self.assertEqual(base64.b64decode(output["data_b64"], validate=True),
                                 b"JOB_VALUE\n")
                self.absent(key)
                self.absent(key[:63])
        marker = self.home / "job-key-256"
        row = self.request(request_version=2, op="exec", caps=["effect:exec", "job:create"],
                           argv=[TOUCH, str(marker)], env={"J" * 256: "BAD"})
        self.rejected(row, marker)


def run(test_case):
    global BINARY, VERIFIER, PRINTENV, TOUCH, ROOT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--artifacts", type=Path, help="retain evidence in a new directory")
    args = parser.parse_args()
    BINARY = args.binary.resolve()
    VERIFIER = BINARY.with_name("jef_verify")
    PRINTENV = shutil.which("printenv")
    TOUCH = shutil.which("touch")
    if not BINARY.is_file() or not VERIFIER.is_file() or not PRINTENV or not TOUCH:
        parser.error("requires jash-agentd, sibling jef_verify, printenv and touch")
    temporary = None
    if args.artifacts:
        ROOT = args.artifacts.resolve()
        ROOT.mkdir()
        print(f"Evidence: {ROOT}", flush=True)
    else:
        temporary = tempfile.TemporaryDirectory(prefix="jash-env-isolation-")
        ROOT = Path(temporary.name)
    try:
        result = unittest.TextTestRunner(verbosity=2).run(
            unittest.defaultTestLoader.loadTestsFromTestCase(test_case))
    finally:
        if temporary:
            temporary.cleanup()
    raise SystemExit(0 if result.wasSuccessful() else 1)


if __name__ == "__main__":
    run(EnvIsolation)
