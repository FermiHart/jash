#!/usr/bin/env python3
"""Runtime cancellation reaches native mutation boundaries without killing the session."""
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import tempfile
import time
import unittest
import base64
import test_agent_mutations_json as fixture

AGENT = Path(os.environ.get("JASH_AGENTD_BIN", fixture.ROOT / "build/jash-agentd")).resolve()


class BuiltinCancel(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temporary = tempfile.TemporaryDirectory(prefix="jash-cancel-hook-")
        cls.addClassCleanup(temporary.cleanup)
        cls.hook = str(Path(temporary.name) / "hook.so")
        subprocess.run(["cc", "-shared", "-fPIC", str(fixture.ROOT / "tests/runtime_cancel_faults.c"), "-ldl", "-o", cls.hook],
                       check=True, capture_output=True, timeout=30)

    def setUp(self):
        fixture.Mutations.setUp(self)
        self.control, child = socket.socketpair()
        self.addCleanup(self.control.close)
        self.control.settimeout(10)
        self.ready, notify = os.pipe()
        release, self.release = os.pipe()
        self.addCleanup(os.close, self.ready); self.addCleanup(os.close, self.release)
        self.env.update(JASH_AGENT_CANCEL_FD=str(child.fileno()), LD_PRELOAD=self.hook,
                        JASH_TEST_CANCEL_READY_FD=str(notify), JASH_TEST_CANCEL_RELEASE_FD=str(release))
        if "before_first_effect" in self._testMethodName: self.env["JASH_TEST_CANCEL_POINT"] = "before"
        if "copy" in self._testMethodName: self.env["JASH_TEST_CANCEL_POINT"] = "copy"
        if "commit_wins" in self._testMethodName:
            self.env.update(JASH_TEST_CANCEL_POINT="commit", JASH_TEST_CANCEL_COMMIT_PARENT=str(self.work))
        self.proc = subprocess.Popen([str(AGENT)], cwd=self.work, env=self.env, pass_fds=(child.fileno(), notify, release),
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        self.addCleanup(self.stop)
        child.close(); os.close(notify); os.close(release)
        self.pending = {}
        self.hello = self.line(self.proc.stdout)

    def stop(self):
        if self.proc.poll() is None: self.proc.kill()
        self.proc.communicate(timeout=10)

    def line(self, stream):
        fd = stream.fileno(); data = self.pending.pop(fd, b""); deadline = time.monotonic() + 10
        while b"\n" not in data:
            self.assertTrue(select.select([fd], [], [], max(0, deadline - time.monotonic()))[0], "response timed out")
            chunk = os.read(fd, 65536); self.assertTrue(chunk, ("unexpected EOF", data))
            data += chunk
        row, _, rest = data.partition(b"\n"); self.pending[fd] = rest
        self.last_wire = row + b"\n"
        return json.loads(row)

    def send(self, request):
        self.proc.stdin.write((json.dumps(request) + "\n").encode())

    def start(self, argv, ident="native-cancel"):
        request = {"id": ident, "argv": argv, "caps": ["effect:mutate"], "cancelable": True}
        self.send(request)
        active = self.line(self.proc.stdout)
        self.assertEqual(active, {"id": ident, "active": True, "cancelable": True})
        return request

    def pause(self):
        self.assertTrue(select.select([self.ready], [], [], 10)[0], "first effect did not reach barrier")
        self.assertEqual(os.read(self.ready, 1), b"1")

    def test_cancel_rm_preserves_remaining_operand_and_session(self):
        (self.work / "first").write_text("first"); (self.work / "second").write_text("keep")
        request = self.start(["rm", "--json", "first", "second"])
        self.pause()
        self.control.sendall(b'{"cancel":"native-cancel"}\n')
        os.write(self.release, b"1")
        self.assertEqual(self.line(self.control), {"cancel": "native-cancel", "accepted": True})
        terminal = self.line(self.proc.stdout)
        (self.work / "request.json").write_text(json.dumps(request) + "\n")
        (self.work / "response.json").write_bytes(self.last_wire)
        self.assertEqual(terminal["rc"], 130, terminal)
        self.assertTrue(terminal["cancelled"])
        report = json.loads(terminal["stdout"])
        self.assertEqual(report["status"], "cancelled")
        self.assertEqual(report["counts"]["completed"], 1)
        self.assertFalse((self.work / "first").exists())
        self.assertEqual((self.work / "second").read_text(), "keep")
        self.send({"id": "verify", "argv": ["mirror", "--json", "--receipt", terminal["receipt_ref"],
                   "--blake3", terminal["receipt_blake3"], "--pubkey", terminal["receipt_pubkey"],
                   "--request", "request.json", "--response", "response.json"], "caps": ["effect:read"]})
        verified = self.line(self.proc.stdout)
        self.assertEqual(verified["rc"], 0, verified)
        receipt = json.loads(json.loads(verified["stdout"])["receipt"])
        self.assertTrue(json.loads(base64.b64decode(receipt["payload"]))["cancelled"])
        self.send({"id": "after", "argv": ["echo", "alive"], "caps": ["effect:read"]})
        self.assertEqual(self.line(self.proc.stdout)["stdout"], "alive\n")
        self.send(request)
        self.assertEqual(self.line(self.proc.stdout), terminal)
        self.send({"id": "next-mutator", "argv": ["mkdir", "--json", "next"], "caps": ["effect:mutate"]})
        self.assertEqual(self.line(self.proc.stdout)["rc"], 0)
        self.assertTrue((self.work / "next").is_dir())

    def test_v2_streaming_cancel_preserves_complete_manifest(self):
        (self.work / "first").write_text("one"); (self.work / "second").write_text("keep")
        self.send({"id": "streaming", "protocol_version": 2, "argv": ["rm", "--json", "first", "second"],
                   "caps": ["effect:mutate"], "cancelable": True})
        start = self.line(self.proc.stdout); self.assertEqual(start["event"], "start")
        self.pause(); self.control.sendall(b'{"cancel":"streaming"}\n'); os.write(self.release, b"1")
        self.assertTrue(self.line(self.control)["accepted"])
        chunks = []
        while True:
            row = self.line(self.proc.stdout)
            if row.get("event") != "chunk": break
            if row["stream"] == "stdout": chunks.append(base64.b64decode(row["data_b64"]))
        self.assertEqual(row["rc"], 130, row); self.assertTrue(row["cancelled"])
        self.assertEqual(json.loads(b"".join(chunks))["status"], "cancelled")
        self.assertEqual((self.work / "second").read_text(), "keep")

    def test_atomic_cancel_rolls_back_before_reporting_terminal(self):
        (self.work / "first").write_text("new"); (self.work / "second").write_text("second")
        (self.work / "dest").mkdir(); (self.work / "dest/first").write_text("old")
        self.start(["mv", "--json", "--atomic", "first", "second", "dest"])
        self.pause(); self.control.sendall(b'{"cancel":"native-cancel"}\n'); os.write(self.release, b"1")
        self.assertTrue(self.line(self.control)["accepted"])
        terminal = self.line(self.proc.stdout); report = json.loads(terminal["stdout"])
        self.assertEqual(terminal["rc"], 130, terminal)
        self.assertEqual(report["transaction"], "rolled_back")
        self.assertEqual(report["counts"]["completed"], 0)
        self.assertEqual((self.work / "first").read_text(), "new")
        self.assertEqual((self.work / "dest/first").read_text(), "old")
        self.assertEqual((self.work / "second").read_text(), "second")

    def test_cancel_during_copy_reports_partial_bytes(self):
        payload = b"x" * (1024 * 1024); (self.work / "source").write_bytes(payload)
        self.start(["cp", "--json", "source", "first"])
        self.pause(); self.control.sendall(b'{"cancel":"native-cancel"}\n'); os.write(self.release, b"1")
        self.assertTrue(self.line(self.control)["accepted"])
        terminal = self.line(self.proc.stdout); report = json.loads(terminal["stdout"])
        self.assertEqual(terminal["rc"], 130, terminal)
        item, = report["items"]
        self.assertEqual(item["status"], "partial")
        self.assertGreater(item["bytes_written"], 0)
        self.assertLess(item["bytes_written"], len(payload))
        self.assertEqual((self.work / "source").read_bytes(), payload)

    def test_cancel_before_first_effect_for_all_advertised_mutators(self):
        (self.work / "source").write_text("source"); (self.work / "victim").write_text("old")
        (self.work / "empty").mkdir()
        mode = (self.work / "victim").stat().st_mode
        cases = [
            ["cp", "--json", "source", "target"], ["mv", "--json", "source", "target"],
            ["rm", "--json", "victim"], ["touch", "--json", "target"], ["mkdir", "--json", "newdir"],
            ["rmdir", "--json", "empty"], ["ln", "--json", "source", "target"],
            ["chmod", "--json", "000", "victim"], ["chown", "--json", str(os.getuid()), "victim"],
            ["sed", "--json", "-i", "s/old/new/", "victim"]]
        for argv in cases:
            with self.subTest(operation=argv[0]):
                ident = "cancel-" + argv[0]; self.start(argv, ident)
                self.pause(); self.control.sendall((f'{{"cancel":"{ident}"}}\n').encode()); os.write(self.release, b"1")
                self.assertTrue(self.line(self.control)["accepted"])
                terminal = self.line(self.proc.stdout)
                self.assertEqual(terminal["rc"], 130, terminal)
                report = json.loads(terminal["stdout"])
                self.assertEqual(report["counts"]["completed"], 0)
                self.assertEqual((self.work / "source").read_text(), "source")
                self.assertEqual((self.work / "victim").read_text(), "old")
                self.assertEqual((self.work / "victim").stat().st_mode, mode)
                self.assertFalse((self.work / "target").exists()); self.assertFalse((self.work / "newdir").exists())
                self.assertTrue((self.work / "empty").is_dir())

    def test_wrong_and_malformed_frames_do_not_cancel(self):
        (self.work / "first").write_text("one"); (self.work / "second").write_text("two")
        self.start(["rm", "--json", "first", "second"])
        self.pause()
        self.control.sendall(b'{"cancel":"wrong-id"}\n{"cancel":"native-cancel","extra":true}\n')
        os.write(self.release, b"1")
        self.assertFalse(self.line(self.control)["accepted"])
        self.assertFalse(self.line(self.control)["accepted"])
        terminal = self.line(self.proc.stdout)
        self.assertEqual(terminal["rc"], 0, terminal); self.assertFalse(terminal["cancelled"])
        self.assertFalse((self.work / "second").exists())

    def test_closed_control_peer_does_not_kill_session(self):
        (self.work / "first").write_text("one"); (self.work / "second").write_text("keep")
        self.start(["rm", "--json", "first", "second"])
        self.pause(); self.control.sendall(b'{"cancel":"native-cancel"}\n'); self.control.close()
        os.write(self.release, b"1")
        terminal = self.line(self.proc.stdout)
        self.assertEqual(terminal["rc"], 130, terminal); self.assertTrue(terminal["cancelled"])
        self.assertTrue((self.work / "second").exists())
        self.send({"id": "alive", "argv": ["true"], "caps": ["effect:read"]})
        self.assertEqual(self.line(self.proc.stdout)["rc"], 0)

    def test_stale_frames_do_not_cancel_future_request(self):
        self.control.sendall(b'{"cancel":"future"}\n')
        self.start(["mkdir", "--json", "created"], "future")
        terminal = self.line(self.proc.stdout)
        self.assertEqual(terminal["rc"], 0); self.assertFalse(terminal["cancelled"])
        self.assertTrue((self.work / "created").is_dir())

    def test_unsupported_forms_and_missing_capability_fail_before_effect(self):
        cases = [(["mkdir", "--json", "created"], ["effect:read"], {}),
                 (["mkdir", "created"], ["effect:mutate"], {}),
                 (["mkdir", "--", "--json"], ["effect:mutate"], {}),
                 (["mkdir", "--json", "created"], ["effect:mutate"], {"timeout_ms": 1000})]
        for index, (argv, caps, extra) in enumerate(cases):
            self.send({"id": f"denied-{index}", "argv": argv, "caps": caps, "cancelable": True, **extra})
            response = self.line(self.proc.stdout)
            self.assertEqual(response["rc"], 125, response)
            self.assertFalse((self.work / "created").exists()); self.assertFalse((self.work / "--json").exists())

    def test_cancellation_capabilities_match_executable_scope(self):
        native = self.hello["cancel"]["native_mutators"]
        self.assertEqual(native["mode"], "cooperative"); self.assertIsNone(native["kill_after_ms"])
        self.assertFalse(native["timeout_ms"])
        inventory = json.loads(subprocess.check_output([str(AGENT), "--contract-json"], timeout=10))
        advertised = {op["name"] for op in inventory["operations"] if op.get("native_cancellation")}
        self.assertEqual(advertised, {"cp", "mv", "rm", "touch", "mkdir", "rmdir", "ln", "chmod", "chown", "sed"})

    def test_duplicate_control_does_not_poison_next_cancelable_request(self):
        (self.work / "first").write_text("one"); (self.work / "second").write_text("keep")
        self.start(["rm", "--json", "first", "second"]); self.pause()
        self.control.sendall(b'{"cancel":"native-cancel"}\n' * 2); os.write(self.release, b"1")
        self.assertTrue(self.line(self.control)["accepted"])
        self.assertEqual(self.line(self.proc.stdout)["rc"], 130)
        self.start(["mkdir", "--json", "next"], "next-cancelable")
        terminal = self.line(self.proc.stdout)
        self.assertEqual(terminal["rc"], 0); self.assertFalse(terminal["cancelled"])
        self.assertTrue((self.work / "second").exists()); self.assertTrue((self.work / "next").is_dir())

    def test_atomic_commit_wins_over_cancel_arriving_after_last_boundary(self):
        (self.work / "first").write_text("committed")
        self.start(["mv", "--json", "--atomic", "-n", "first", "target"])
        self.pause(); self.control.sendall(b'{"cancel":"native-cancel"}\n'); os.write(self.release, b"1")
        terminal = self.line(self.proc.stdout)
        self.assertEqual(terminal["rc"], 0); self.assertFalse(terminal["cancelled"])
        self.assertEqual(json.loads(terminal["stdout"])["transaction"], "committed")
        self.assertEqual((self.work / "target").read_text(), "committed")
        self.start(["mkdir", "--json", "next"], "after-commit")
        self.assertEqual(self.line(self.proc.stdout)["rc"], 0)

    def test_missing_control_descriptor_rejects_before_effect(self):
        env = {k: v for k, v in self.env.items() if k != "JASH_AGENT_CANCEL_FD" and not k.startswith("JASH_TEST_") and k != "LD_PRELOAD"}
        request = {"id": "no-control", "argv": ["mkdir", "--json", "created"], "caps": ["effect:mutate"], "cancelable": True}
        result = subprocess.run([str(AGENT)], cwd=self.work, env=env, input=(json.dumps(request) + "\n").encode(),
                                capture_output=True, timeout=10)
        terminal = json.loads(result.stdout.splitlines()[-1])
        self.assertEqual(terminal["rc"], 125); self.assertIn("JASH_AGENT_CANCEL_FD", terminal["error"])
        self.assertFalse((self.work / "created").exists())

    def test_listener_refuses_one_control_socket_shared_between_workers(self):
        left, right = socket.socketpair()
        try:
            key = self.work / "scope"; key.write_text("1" * 64); key.chmod(0o600)
            path = self.work / "listener"
            result = subprocess.run([str(AGENT), "--listen", str(path), "--scope-file", str(key)],
                env=dict(self.env, JASH_AGENT_CANCEL_FD=str(right.fileno())), pass_fds=(right.fileno(),),
                capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertFalse(path.exists())
        finally: left.close(); right.close()


class ControlCore(unittest.TestCase):
    def test_fragmentation_invalid_bytes_oversize_and_closed_peer(self):
        with tempfile.TemporaryDirectory(prefix="jash-cancel-core-") as temp:
            binary = str(Path(temp) / "test")
            flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if os.environ.get("SANITIZE") else []
            subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", *flags, "-I", str(fixture.ROOT / "c"),
                str(fixture.ROOT / "tests/test_agent_cancel_core.c"), str(fixture.ROOT / "c/core/agent_cancel.c"), "-o", binary],
                check=True, capture_output=True, timeout=30)
            subprocess.run([binary], check=True, capture_output=True, timeout=10)


if __name__ == "__main__": unittest.main()
