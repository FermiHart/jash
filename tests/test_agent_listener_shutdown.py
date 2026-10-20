#!/usr/bin/env python3
"""A listener shutdown drains session-owned jobs before worker/process teardown."""
import json
import os
import signal
import socket
import struct
import subprocess
import unittest
import time
import tempfile
from pathlib import Path
import test_agent_job_disconnect as jobs
import test_agent_local_cancel as local
import test_agent_local_sessions as fixture
from proc_observation import direct_children


class Shutdown(unittest.TestCase):
    setUp = local.LocalCancel.setUp
    setUpClass = classmethod(local.LocalCancel.setUpClass.__func__)
    stop = local.LocalCancel.stop
    open_connection = local.LocalCancel.open_connection
    connect_control = local.LocalCancel.connect_control
    query = jobs.Disconnect.query
    start = jobs.Disconnect.start
    own_pid = jobs.Disconnect.own_pid
    eventually = jobs.Disconnect.eventually
    workspace = local.LocalCancel.workspace
    pause = local.LocalCancel.pause

    def configure_env(self, env):
        if "archive_failure" in self._testMethodName: env["JASH_TEST_JOB_ARCHIVE_FAIL"] = "1"
        if "repeated_signals" in self._testMethodName: env["JASH_TEST_SHUTDOWN_ARCHIVE_GATE"] = str(self.root / "archiving")
        if "queued_work" in self._testMethodName: env["JASH_TEST_SHUTDOWN_WAIT_GATE"] = str(self.root / "waiting")
        if "native_atomic" in self._testMethodName: env["JASH_TEST_WAIT_SHUTDOWN_NATIVE"] = "1"

    def result(self, job_id):
        request = {"id": "after-shutdown", "protocol_version": 2, "op": "job.result", "job_id": job_id, "caps": ["job:read"]}
        p = subprocess.run([str(fixture.AGENT)], input=(json.dumps(request) + "\n").encode(), cwd=self.root,
                           env=self.env, capture_output=True, timeout=10)
        self.assertEqual(p.returncode, 0, p.stderr)
        return json.loads(p.stdout.splitlines()[-1])

    def watch_launcher(self, control):
        worker = struct.unpack("3i", control.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))[0]
        for child in direct_children(worker): self.own_pid(child)
        return worker

    def test_sigterm_drains_job_and_retains_terminal_observation(self):
        _, _, control, _ = self.connect_control()
        # A separate data stream is kept open throughout listener shutdown.
        client, stream, control, hello = self.connect_control()
        self.assertTrue(hello["managed_shutdown"])
        started, pid = self.start(stream); self.watch_launcher(control)
        self.server.terminate(); self.assertEqual(self.server.wait(timeout=10), 0)
        archived = self.result(started["job_id"])
        self.assertTrue(archived["available"], archived)
        response = json.loads(archived["response_json"])
        self.assertEqual(response["completion_trigger"], "listener_shutdown")
        self.assertTrue(response["quiescent"])
        with self.assertRaises(ProcessLookupError): os.kill(pid, 0)
        self.assertEqual(control.recv(1), b"")
        self.assertFalse(self.path.exists())
        descriptor = json.loads(subprocess.check_output([str(fixture.AGENT), "--contract-json", "invocation:listen"], timeout=5))
        self.assertEqual(descriptor["traits"]["shutdown"]["policy"], "drain-workers")
        self.assertIsNone(descriptor["traits"]["shutdown"]["deadline_ms"])

    def test_sigint_wakes_idle_and_partially_authenticated_workers(self):
        _, _, control, _ = self.connect_control()
        unauth, stream = self.open_connection()
        stream.write(b'{"scope":')
        self.server.send_signal(signal.SIGINT)
        self.assertEqual(self.server.wait(timeout=3), 0)
        self.assertEqual(control.recv(1), b"")
        try: closed = unauth.recv(1)
        except ConnectionResetError: closed = b""
        self.assertEqual(closed, b"")

    def test_repeated_signals_wait_for_archive_instead_of_killing_workers(self):
        _, stream, control, _ = self.connect_control()
        started, pid = self.start(stream); self.watch_launcher(control)
        self.server.terminate()
        self.eventually(lambda: (self.root / "archiving").exists(), "archive barrier not reached")
        self.server.send_signal(signal.SIGINT); self.server.terminate()
        with self.assertRaises(subprocess.TimeoutExpired): self.server.wait(timeout=.35)
        (self.root / "archiving.release").touch()
        self.assertEqual(self.server.wait(timeout=10), 0)
        self.assertTrue(self.result(started["job_id"])["available"])
        with self.assertRaises(ProcessLookupError): os.kill(pid, 0)

    def test_archive_failure_propagates_to_listener_exit(self):
        _, stream, control, _ = self.connect_control()
        started, pid = self.start(stream); self.watch_launcher(control)
        self.server.terminate(); self.assertEqual(self.server.wait(timeout=10), 1)
        self.assertFalse(self.result(started["job_id"])["available"])
        self.assertEqual(len(list((self.root / ".jash/agent_jobs").glob("**/*.stdout"))), 1)
        with self.assertRaises(ProcessLookupError): os.kill(pid, 0)

    def test_shutdown_drops_queued_work_after_active_wait(self):
        _, stream, control, _ = self.connect_control()
        started, _ = self.start(stream); worker = self.watch_launcher(control)
        (self.root / "waiting.arm").touch()
        waiting = {"id": "waiting", "protocol_version": 2, "op": "job.wait", "job_id": started["job_id"],
                   "wait_ms": 30000, "caps": ["job:read"]}
        queued = {"id": "queued", "argv": ["mkdir", "must-not-run"], "cwd": str(self.root), "caps": ["effect:mutate"]}
        stream.write((json.dumps(waiting) + "\n" + json.dumps(queued) + "\n").encode())
        self.eventually(lambda: (self.root / "waiting").exists() and (self.root / "waiting").stat().st_size > 0,
                        "job.wait did not enter its wait")
        observed_worker, wait_kind = (self.root / "waiting").read_text().split()
        self.assertEqual(int(observed_worker), worker, "barrier must observe the session worker's wait")
        self.assertIn(wait_kind, ("completion-pidfd", "polling"))
        self.server.terminate(); self.assertEqual(self.server.wait(timeout=10), 0)
        self.assertFalse((self.root / "must-not-run").exists())
        self.assertTrue(self.result(started["job_id"])["available"])

    def test_listener_loss_reaches_multiple_workers_without_sibling_fd_holds(self):
        records = []
        for name in ("a", "b"):
            _, stream, control, _ = self.connect_control()
            started, pid = self.start(stream, name); self.watch_launcher(control)
            records.append((started, pid, control))
        self.server.kill(); self.assertEqual(self.server.wait(timeout=5), -signal.SIGKILL)
        for started, pid, control in records:
            self.assertEqual(control.recv(1), b"")
            with self.assertRaises(ProcessLookupError): os.kill(pid, 0)
            archived = self.result(started["job_id"])
            self.assertTrue(archived["available"], (archived, self.server.stderr.read() if not archived["available"] else b""))
            self.assertEqual(json.loads(archived["response_json"])["completion_trigger"], "listener_lost")

    def test_fragmented_large_and_prefetched_requests_preserve_framing(self):
        _, stream, _, _ = self.connect_control()
        text = "x" * 9000
        first = (json.dumps({"id": "large", "argv": ["echo", text], "caps": ["effect:read"]}) + "\n").encode()
        second = (json.dumps({"id": "next", "argv": ["echo", "next"], "caps": ["effect:read"]}) + "\n").encode()
        stream.write(first[:3000]); stream.write(first[3000:] + second)
        self.assertEqual(json.loads(stream.readline())["stdout"], text + "\n")
        self.assertEqual(json.loads(stream.readline())["stdout"], "next\n")

    def test_auth_rejection_is_not_a_runtime_shutdown_failure(self):
        _, stream = self.open_connection()
        stream.write(b'{"scope":"' + b"0" * 64 + b'"}\n')
        self.assertEqual(json.loads(stream.readline())["error"], "scope_denied")
        self.server.terminate(); self.assertEqual(self.server.wait(timeout=5), 0)

    def test_native_atomic_without_client_cancel_flag_rolls_back_on_shutdown(self):
        _, stream, _, hello = self.connect_control()
        work, notify, release = self.workspace("native")
        (work / "dst").mkdir(); (work / "dst/first").write_text("old")
        request = {"id": "native", "argv": ["mv", "--json", "--atomic", "first", "second", "dst"],
                   "cwd": str(work), "caps": ["effect:mutate"]}
        wire = (json.dumps(request) + "\n").encode(); stream.write(wire)
        self.pause(notify); self.server.terminate(); os.write(release, b"1")
        raw = stream.readline(); response = json.loads(raw)
        self.assertEqual(response["rc"], 130, response); self.assertTrue(response["cancelled"])
        self.assertEqual(json.loads(response["stdout"])["transaction"], "rolled_back")
        self.assertEqual((work / "first").read_text(), "first")
        self.assertEqual((work / "dst/first").read_text(), "old")
        self.assertEqual(self.server.wait(timeout=10), 0)
        req = self.root / "request.json"; req.write_bytes(wire)
        res = self.root / "response.json"; res.write_bytes(raw)
        receipt = self.root / ".jash/agent_receipts" / response["receipt_ref"]
        verified = subprocess.run([str(fixture.ROOT / "build/jef_verify"), "runtime-receipt", hello["receipts"]["pubkey"],
            str(req), str(res), str(receipt)], capture_output=True, timeout=10)
        self.assertEqual(verified.returncode, 0, (verified.stdout, verified.stderr))

    def test_shutdown_reaps_detached_descendant_before_reporting_success(self):
        _, stream, control, _ = self.connect_control()
        marker = self.root / "detached.json"; ready = self.root / "detached-ready"
        code = ("import os,time,signal,json; child=os.fork();"
                f"\nif child == 0:\n os.setsid(); signal.signal(signal.SIGTERM,signal.SIG_IGN); open({str(ready)!r},'w').close(); time.sleep(60); os._exit(0)"
                f"\nf=open({str(marker) + '.tmp'!r},'w'); f.write(json.dumps([os.getpid(),child])); f.close(); os.rename({str(marker) + '.tmp'!r},{str(marker)!r}); time.sleep(60)")
        started = self.query(stream, {"id": "tree", "protocol_version": 2, "op": "job.start", "argv": ["/usr/bin/python3", "-c", code],
            "background": True, "caps": ["effect:exec", "job:create"]})
        self.eventually(lambda: marker.exists() and ready.exists(), "detached descendant not ready")
        pids = json.loads(marker.read_text())
        for pid in pids: self.own_pid(pid)
        self.watch_launcher(control)
        self.server.terminate(); self.assertEqual(self.server.wait(timeout=10), 0)
        for pid in pids:
            with self.assertRaises(ProcessLookupError): os.kill(pid, 0)
        self.assertTrue(json.loads(self.result(started["job_id"])["response_json"])["quiescent"])


class Publication(unittest.TestCase):
    def test_concurrent_publishers_never_observe_partial_cas(self):
        with tempfile.TemporaryDirectory(prefix="jash-cas-publish-") as temp:
            binary = str(Path(temp) / "test")
            flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if os.environ.get("SANITIZE") else []
            sources = ["tests/test_cas_publication.c", "c/core/json_writer.c", "c/module/blake3.c",
                       "c/module/jash_crypto.c", "c/osif/posix_common.c"]
            command = ["cc", "-std=gnu11", "-g", "-O1", "-ffunction-sections", "-fdata-sections", *flags]
            for include in ("c", "c/sys", "libjef"): command += ["-I", str(fixture.ROOT / include)]
            command += [str(fixture.ROOT / source) for source in sources] + ["-Wl,--gc-sections", "-o", binary]
            compiled = subprocess.run(command, capture_output=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([binary, temp], capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__": unittest.main()
