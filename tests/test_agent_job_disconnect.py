#!/usr/bin/env python3
"""Session-owned jobs stop on disconnect and retain verifiable terminal evidence."""
import json
import os
from pathlib import Path
import subprocess
import time
import unittest
import signal
import tempfile
import struct
import socket
import test_agent_local_cancel as local
import test_agent_local_sessions as fixture
from proc_observation import direct_children


class Disconnect(unittest.TestCase):
    setUp = local.LocalCancel.setUp
    setUpClass = classmethod(local.LocalCancel.setUpClass.__func__)
    stop = local.LocalCancel.stop
    open_connection = local.LocalCancel.open_connection
    connect_control = local.LocalCancel.connect_control

    def configure_env(self, env):
        if "lost_start_reply" in self._testMethodName: env["JASH_TEST_JOB_REPLY_PAUSE"] = str(self.root / "reply-paused")
        if "archive_failure" in self._testMethodName: env["JASH_TEST_JOB_ARCHIVE_FAIL"] = "1"

    def query(self, stream, request):
        stream.write((json.dumps(request) + "\n").encode())
        return json.loads(stream.readline())

    def result(self, stream, job, identifier="result"):
        return self.query(stream, {"id": identifier, "protocol_version": 2, "op": "job.result", "job_id": job, "caps": ["job:read"]})

    def eventually(self, predicate, message):
        deadline = time.monotonic() + 5
        while not predicate() and time.monotonic() < deadline: time.sleep(.01)
        self.assertTrue(predicate(), message)

    def own_pid(self, pid):
        path = Path(f"/proc/{pid}/stat")
        try: start = path.read_text().rsplit(")", 1)[1].split()[19]
        except FileNotFoundError: return
        def cleanup():
            try:
                if path.read_text().rsplit(")", 1)[1].split()[19] == start: os.kill(pid, signal.SIGKILL)
            except ProcessLookupError: pass
            except FileNotFoundError: pass
        self.addCleanup(cleanup)

    def start(self, stream, identifier="worker"):
        marker = self.root / (identifier + ".pid")
        temporary = str(marker) + ".tmp"
        code = (f"import os,time; print('before disconnect',flush=True); f=open({temporary!r},'w'); "
                f"f.write(str(os.getpid())); f.close(); os.rename({temporary!r},{str(marker)!r}); time.sleep(60)")
        result = self.query(stream, {"id": identifier, "protocol_version": 2, "op": "job.start",
            "argv": ["/usr/bin/python3", "-c", code], "background": True, "caps": ["effect:exec", "job:create"]})
        self.assertIn("job_id", result, result)
        deadline = time.monotonic() + 5
        while not marker.exists() and time.monotonic() < deadline: time.sleep(.01)
        self.assertTrue(marker.exists())
        pid = int(marker.read_text()); self.own_pid(pid)
        return result, pid

    def test_disconnect_archives_terminal_and_preserves_output(self):
        client, stream, control, _ = self.connect_control()
        started, pid = self.start(stream)
        worker = struct.unpack("3i", control.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))[0]
        launchers = direct_children(worker)
        self.assertEqual(len(launchers), 1)
        sockets = {os.readlink(fd) for fd in Path(f"/proc/{launchers[0]}/fd").iterdir() if os.readlink(fd).startswith("socket:")}
        self.assertEqual(len(sockets), 1, "launcher should retain only its supervisor channel")
        stream.close(); client.close()
        self.assertEqual(control.recv(1), b"")
        with self.assertRaises(ProcessLookupError): os.kill(pid, 0)
        _, new, _, _ = self.connect_control()
        archived = self.query(new, {"id": "result", "protocol_version": 2, "op": "job.result",
                                   "job_id": started["job_id"], "caps": ["job:read"]})
        self.assertTrue(archived.get("available"), archived)
        response = json.loads(archived["response_json"])
        self.assertTrue(response["terminal"]); self.assertTrue(response["quiescent"])
        self.assertTrue(response["cancel_requested"])
        self.assertEqual(response["completion_trigger"], "connection_eof")
        output = self.root / ".jash/overflow" / response["stdout_ref"]
        self.assertIn(b"before disconnect", output.read_bytes())
        request_path = self.root / "observation.request"; request_path.write_text(archived["request_json"])
        response_path = self.root / "observation.response"; response_path.write_text(archived["response_json"])
        receipt = self.root / ".jash/agent_receipts" / response["receipt_ref"]
        self.assertEqual(response["receipt_pubkey"], started["receipt_pubkey"])
        verified = subprocess.run([str(fixture.ROOT / "build/jef_verify"), "job-receipt", started["receipt_pubkey"], "terminal",
                                   str(request_path), str(response_path), str(receipt)], capture_output=True, timeout=10)
        self.assertEqual(verified.returncode, 0, verified.stderr)

    def test_lost_start_reply_still_archives_after_disconnect(self):
        client, stream, control, _ = self.connect_control()
        request = {"id": "lost", "protocol_version": 2, "op": "job.start", "argv": ["sleep", "60"],
                   "background": True, "caps": ["effect:exec", "job:create"]}
        stream.write((json.dumps(request) + "\n").encode())
        self.eventually(lambda: (self.root / "reply-paused").exists(), "start response did not reach barrier")
        # Start receipt is already persisted even though the response was not delivered.
        import base64
        receipt, = (self.root / ".jash/agent_receipts").glob("*.job-start.dsse")
        job_id = json.loads(base64.b64decode(json.loads(receipt.read_text())["payload"]))["job_id"]
        stream.close(); client.close(); (self.root / "reply-paused.release").touch()
        self.assertEqual(control.recv(1), b"")
        _, reader, _, _ = self.connect_control()
        archived = self.result(reader, job_id)
        self.assertTrue(archived["available"], archived)

    def test_completed_job_is_not_reported_as_cancelled(self):
        client, stream, control, _ = self.connect_control()
        started = self.query(stream, {"id": "short", "protocol_version": 2, "op": "job.start",
            "argv": ["/usr/bin/printf", "finished"], "background": True, "caps": ["effect:exec", "job:create"]})
        done = self.query(stream, {"id": "wait-short", "protocol_version": 2, "op": "job.wait",
            "job_id": started["job_id"], "wait_ms": 5000, "caps": ["job:read"]})
        self.assertTrue(done["terminal"], done)
        stream.close(); client.close(); self.assertEqual(control.recv(1), b"")
        _, reader, _, _ = self.connect_control()
        final = json.loads(self.result(reader, started["job_id"])["response_json"])
        self.assertEqual(final["rc"], 0); self.assertFalse(final["cancel_requested"])

    def test_archive_failure_retains_spools_and_reports_unavailable(self):
        client, stream, control, _ = self.connect_control()
        started, pid = self.start(stream)
        stream.close(); client.close(); self.assertEqual(control.recv(1), b"")
        with self.assertRaises(ProcessLookupError): os.kill(pid, 0)
        _, reader, _, _ = self.connect_control()
        self.assertFalse(self.result(reader, started["job_id"])["available"])
        files = list((self.root / ".jash/agent_jobs").glob("**/*.stdout"))
        self.assertEqual(len(files), 1)
        self.assertIn(b"before disconnect", files[0].read_bytes())

    def test_other_client_job_remains_live_and_is_not_reattached(self):
        a, sa, ca, _ = self.connect_control(); b, sb, cb, _ = self.connect_control()
        ja, pa = self.start(sa, "a"); jb, pb = self.start(sb, "b")
        sa.close(); a.close(); self.assertEqual(ca.recv(1), b"")
        with self.assertRaises(ProcessLookupError): os.kill(pa, 0)
        os.kill(pb, 0)
        self.assertFalse(self.result(sb, jb["job_id"])["available"])
        self.assertTrue(self.result(sb, ja["job_id"], "other-result")["available"])
        missing = self.query(sb, {"id": "not-reattach", "protocol_version": 2, "op": "job.get", "job_id": ja["job_id"], "caps": ["job:read"]})
        self.assertEqual(missing["error"], "job not found")
        sb.close(); b.close(); self.assertEqual(cb.recv(1), b"")

    def test_corrupt_and_nonregular_archives_are_not_results(self):
        client, stream, control, _ = self.connect_control(); started, _ = self.start(stream)
        stream.close(); client.close(); self.assertEqual(control.recv(1), b"")
        path = self.root / ".jash/agent_job_results" / (started["job_id"] + ".result")
        original = path.read_bytes(); path.write_bytes(original[:-1])
        _, reader, _, _ = self.connect_control()
        self.assertFalse(self.result(reader, started["job_id"])["available"])
        path.unlink(); os.mkfifo(path)
        self.assertFalse(self.result(reader, started["job_id"], "fifo")["available"])
        self.assertFalse(self.result(reader, "../escape", "traversal")["available"])

    def test_disconnect_during_long_wait_does_not_wait_for_request_deadline(self):
        client, stream, control, _ = self.connect_control(); started, pid = self.start(stream)
        stream.write((json.dumps({"id": "waiting", "protocol_version": 2, "op": "job.wait",
            "job_id": started["job_id"], "wait_ms": 30000, "caps": ["job:read"]}) + "\n").encode())
        stream.close(); client.close()
        self.assertEqual(control.recv(1), b"")
        with self.assertRaises(ProcessLookupError): os.kill(pid, 0)
        _, reader, _, _ = self.connect_control()
        self.assertTrue(self.result(reader, started["job_id"])["available"])

    def test_detached_term_ignoring_descendant_is_reaped_before_archive(self):
        client, stream, control, _ = self.connect_control()
        marker = self.root / "descendants.json"
        code = ("import os,time,signal,json; child=os.fork(); "
                "\nif child == 0:\n os.setsid(); signal.signal(signal.SIGTERM,signal.SIG_IGN); time.sleep(60); os._exit(0)"
                f"\nf=open({str(marker) + '.tmp'!r},'w'); f.write(json.dumps([os.getpid(),child])); f.close(); "
                f"os.rename({str(marker) + '.tmp'!r},{str(marker)!r}); time.sleep(60)")
        started = self.query(stream, {"id": "descendants", "protocol_version": 2, "op": "job.start",
            "argv": ["/usr/bin/python3", "-c", code], "background": True, "caps": ["effect:exec", "job:create"]})
        self.eventually(marker.exists, "descendant did not start")
        pids = json.loads(marker.read_text())
        for pid in pids: self.own_pid(pid)
        # setsid has completed, rather than merely forking a same-group child.
        self.eventually(lambda: os.getsid(pids[1]) == pids[1], "descendant did not detach")
        stream.close(); client.close(); self.assertEqual(control.recv(1), b"")
        for pid in pids:
            with self.assertRaises(ProcessLookupError): os.kill(pid, 0)
        _, reader, _, _ = self.connect_control()
        response = json.loads(self.result(reader, started["job_id"])["response_json"])
        self.assertTrue(response["quiescent"]); self.assertTrue(response["terminal"])

    def test_result_requires_read_capability_and_rejects_execution_fields(self):
        _, stream, _, _ = self.connect_control()
        for extra in ({"caps": ["job:signal"]}, {"caps": ["job:read"], "argv": ["touch", "not-created"]}):
            result = self.query(stream, {"id": "denied", "protocol_version": 2, "op": "job.result",
                                        "job_id": "j-" + "0" * 32, **extra})
            self.assertIn("error", result); self.assertNotIn("response_json", result)
        self.assertFalse((self.root / "not-created").exists())

    def test_stdio_archive_failure_exits_nonzero_after_reaping(self):
        env = dict(self.env, JASH_TEST_JOB_ARCHIVE_FAIL="1")
        proc = subprocess.Popen([str(fixture.AGENT)], cwd=self.root, env=env, stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(lambda: proc.poll() is None and proc.kill())
        json.loads(proc.stdout.readline())
        request = {"id": "stdio", "protocol_version": 2, "op": "job.start", "argv": ["sleep", "60"],
                   "background": True, "caps": ["effect:exec", "job:create"]}
        proc.stdin.write((json.dumps(request) + "\n").encode()); proc.stdin.flush()
        started = json.loads(proc.stdout.readline()); self.assertIn("job_id", started)
        proc.stdin.close(); proc.stdin = None
        _, diagnostic = proc.communicate(timeout=10)
        self.assertEqual(proc.returncode, 125); self.assertIn(b"retaining", diagnostic)


class ArchiveCore(unittest.TestCase):
    def test_archive_bounds_integrity_and_no_clobber(self):
        with tempfile.TemporaryDirectory(prefix="jash-job-archive-") as temp:
            binary = str(Path(temp) / "test")
            flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if os.environ.get("SANITIZE") else []
            compiled = subprocess.run(["cc", "-std=gnu11", "-g", "-O1", "-ffunction-sections", "-fdata-sections", *flags,
                "-I", str(fixture.ROOT / "c"), "-I", str(fixture.ROOT / "c/sys"), "-I", str(fixture.ROOT / "libjef"),
                str(fixture.ROOT / "tests/test_job_archive.c"), str(fixture.ROOT / "c/core/json_writer.c"),
                str(fixture.ROOT / "c/module/blake3.c"), str(fixture.ROOT / "c/module/jash_crypto.c"), str(fixture.ROOT / "c/osif/posix_common.c"),
                "-Wl,--gc-sections", "-o", binary], capture_output=True, timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            subprocess.run([binary, temp], check=True, capture_output=True, timeout=10)


if __name__ == "__main__": unittest.main()
