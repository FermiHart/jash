#!/usr/bin/env python3
"""Authenticated local clients receive disjoint cancellation endpoints."""
import array
import json
import os
import socket
import unittest
import subprocess
import tempfile
import time
import select
import base64
import secrets
import struct
from pathlib import Path
import test_agent_local_sessions as fixture
from proc_observation import direct_children


class LocalCancel(unittest.TestCase):
    stop = fixture.Sessions.stop
    request = fixture.Sessions.request

    @classmethod
    def setUpClass(cls):
        temp = tempfile.TemporaryDirectory(prefix="jash-local-cancel-hooks-"); cls.addClassCleanup(temp.cleanup)
        cls.hook = str(Path(temp.name) / "hook.so")
        subprocess.run(["cc", "-shared", "-fPIC", str(fixture.ROOT / "tests/runtime_cancel_faults.c"), "-ldl", "-o", cls.hook],
                       check=True, capture_output=True, timeout=30)

    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="jash-local-cancel-"); self.addCleanup(temp.cleanup)
        self.root = Path(temp.name); self.path = self.root / "socket"
        self.token = secrets.token_hex(32)
        self.key = self.root / "scope"; self.key.write_text(self.token); self.key.chmod(0o600)
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("JASH_")}
        self.env.update(HOME=str(self.root), JASH_SIGIL="0", JASH_TIMELINE="0")
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_CANCEL_FIFO="1")
        if "transfer_failure" in self._testMethodName: self.env["JASH_TEST_LOCAL_SEND_FAIL"] = "1"
        if "pair_failure" in self._testMethodName: self.env["JASH_TEST_LOCAL_PAIR_FAIL"] = "1"
        configure = getattr(self, "configure_env", None)
        if configure: configure(self.env)
        self.server = subprocess.Popen([str(fixture.AGENT), "--listen", str(self.path), "--scope-file", str(self.key)],
                                       cwd=self.root, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(self.stop)
        deadline = time.monotonic() + 5
        while not self.path.exists() and self.server.poll() is None and time.monotonic() < deadline: time.sleep(.01)
        self.assertTrue(self.path.exists())

    def open_connection(self):
        deadline = time.monotonic() + 5
        while True:
            client = socket.socket(socket.AF_UNIX); client.settimeout(5)
            try: client.connect(str(self.path)); break
            except (FileNotFoundError, ConnectionRefusedError):
                client.close()
                if self.server.poll() is not None or time.monotonic() >= deadline: raise
                time.sleep(.01)
        self.addCleanup(client.close)
        stream = client.makefile("rwb", buffering=0); self.addCleanup(stream.close)
        self.assertEqual(json.loads(stream.readline())["jash_local"], 1)
        return client, stream

    def connect_control(self):
        client, stream = self.open_connection()
        stream.write((json.dumps({"scope": self.token, "control": "fd-v1"}, separators=(",", ":")) + "\n").encode())
        attachment = json.loads(stream.readline())
        self.assertEqual(attachment.get("jash_control"), 1, attachment)
        data, ancillary, flags, _ = client.recvmsg(1, socket.CMSG_SPACE(array.array("i").itemsize), socket.MSG_CMSG_CLOEXEC)
        received = []
        for level, kind, payload in ancillary:
            if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                numbers = array.array("i"); numbers.frombytes(payload); received.extend(numbers)
        if data != b"C" or flags & socket.MSG_CTRUNC or len(received) != 1:
            for fd in received: os.close(fd)
            self.fail((data, ancillary, flags))
        control = socket.socket(fileno=received[0]); control.settimeout(5); self.addCleanup(control.close)
        hello = json.loads(stream.readline())
        return client, stream, control, hello

    def workspace(self, name):
        work = self.root / name; work.mkdir()
        (work / "first").write_text("first"); (work / "second").write_text("second")
        descriptors = []
        for path in (work / ".cancel-ready", work / ".cancel-release"):
            os.mkfifo(path); fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
            self.addCleanup(os.close, fd); descriptors.append(fd)
        return work, *descriptors

    def start(self, stream, ident, work, argv=None):
        request = {"id": ident, "argv": argv or ["rm", "--json", "first", "second"],
                   "cwd": str(work), "caps": ["effect:read", "effect:mutate"], "cancelable": True}
        wire = (json.dumps(request) + "\n").encode(); stream.write(wire)
        self.assertEqual(json.loads(stream.readline()), {"id": ident, "active": True, "cancelable": True})
        return wire

    def pause(self, ready):
        self.assertTrue(select.select([ready], [], [], 5)[0], "effect did not reach barrier")
        self.assertEqual(os.read(ready, 1), b"1")

    def control_reply(self, control):
        data = b""
        while not data.endswith(b"\n"):
            byte = control.recv(1); self.assertTrue(byte, data); data += byte
        return json.loads(data)

    def test_opt_in_negotiates_private_control_and_preserves_jsonl(self):
        _, stream, control, hello = self.connect_control()
        self.assertTrue(hello["cancel"]["enabled"])
        self.assertFalse(os.get_inheritable(control.fileno()))
        self.assertEqual(self.request(stream, "alive", ["echo", "connected"])["stdout"], "connected\n")

    def test_negotiation_is_discoverable(self):
        descriptor = json.loads(subprocess.check_output([str(fixture.AGENT), "--contract-json", "invocation:listen"], timeout=5))
        self.assertEqual(descriptor["traits"]["control"], {"negotiation": "control=fd-v1", "attachment": "scm-rights-one-byte-v1",
                                                        "lifetime": "connection-worker", "shared": False})

    def test_two_clients_cannot_consume_or_apply_each_others_cancel(self):
        _, a, ca, _ = self.connect_control(); _, b, cb, _ = self.connect_control()
        wa, na, ra = self.workspace("a"); wb, nb, rb = self.workspace("b")
        self.start(a, "request-a", wa); self.pause(na)
        self.start(b, "request-b", wb); self.pause(nb)
        ca.sendall(b'{"cancel":"request-b"}\n{"cancel":"request-a"}\n'); os.write(ra, b"1")
        self.assertFalse(self.control_reply(ca)["accepted"])
        self.assertTrue(self.control_reply(ca)["accepted"])
        cancelled = json.loads(a.readline())
        self.assertEqual(cancelled["rc"], 130); self.assertTrue(cancelled["cancelled"])
        self.assertEqual((wa / "second").read_text(), "second")
        # B's controller has received no A acknowledgements and B is still paused.
        self.assertFalse(select.select([cb], [], [], 0)[0])
        self.assertTrue((wb / "second").exists())
        os.write(rb, b"1")
        completed = json.loads(b.readline())
        self.assertEqual(completed["rc"], 0); self.assertFalse(completed["cancelled"])
        self.assertFalse((wb / "second").exists())

    def test_atomic_rollback_over_private_control(self):
        _, stream, control, _ = self.connect_control()
        work, notify, release = self.workspace("atomic")
        (work / "dst").mkdir(); (work / "dst/first").write_text("old")
        self.start(stream, "atomic", work, ["mv", "--json", "--atomic", "first", "second", "dst"])
        self.pause(notify); control.sendall(b'{"cancel":"atomic"}\n'); os.write(release, b"1")
        self.assertTrue(self.control_reply(control)["accepted"])
        terminal = json.loads(stream.readline())
        self.assertEqual(terminal["rc"], 130)
        self.assertEqual(json.loads(terminal["stdout"])["transaction"], "rolled_back")
        self.assertEqual((work / "first").read_text(), "first")
        self.assertEqual((work / "dst/first").read_text(), "old")

    def test_disconnect_data_then_cancel_and_reconnect_replays_terminal(self):
        client, stream, control, _ = self.connect_control()
        work, notify, release = self.workspace("disconnected")
        wire = self.start(stream, "disconnected", work); self.pause(notify)
        stream.close(); client.close()
        control.sendall(b'{"cancel":"disconnected"}\n'); os.write(release, b"1")
        self.assertTrue(self.control_reply(control)["accepted"])
        self.assertEqual(control.recv(1), b"")
        _, retry, _, _ = self.connect_control()
        retry.write(wire); terminal = json.loads(retry.readline())
        self.assertEqual(terminal["rc"], 130, terminal); self.assertTrue(terminal["cancelled"])
        self.assertEqual((work / "second").read_text(), "second")
        retry.write(wire); self.assertEqual(json.loads(retry.readline()), terminal)

    def test_worker_exit_closes_its_control_even_while_other_client_lives(self):
        client, stream, control, _ = self.connect_control()
        _, other, _, _ = self.connect_control()
        stream.close(); client.close()
        self.assertEqual(control.recv(1), b"")
        self.assertEqual(self.request(other, "other-alive", ["echo", "alive"])["stdout"], "alive\n")

    def test_legacy_client_remains_json_only(self):
        _, stream = self.open_connection()
        stream.write((json.dumps({"scope": self.token}, separators=(",", ":")) + "\n").encode())
        hello = json.loads(stream.readline())
        self.assertFalse(hello["cancel"]["enabled"])
        self.assertEqual(self.request(stream, "legacy", ["echo", "legacy"])["stdout"], "legacy\n")

    def test_invalid_authentication_never_transfers_a_descriptor(self):
        frames = [{"scope": "0" * 64, "control": "fd-v1"}, {"scope": self.token, "control": "unknown"},
                  {"scope": self.token, "control": "fd-v1", "extra": True}]
        for frame in frames:
            client, stream = self.open_connection()
            stream.write((json.dumps(frame, separators=(",", ":")) + "\n").encode())
            data = b""
            while b"\n" not in data:
                chunk, ancillary, _, _ = client.recvmsg(256, socket.CMSG_SPACE(4))
                self.assertFalse(ancillary); self.assertTrue(chunk); data += chunk
            self.assertEqual(json.loads(data)["error"], "scope_denied")
            self.assertEqual(client.recv(1), b"")

    def test_transfer_failure_does_not_start_runtime_or_execute_queued_request(self):
        client, stream = self.open_connection()
        auth = (json.dumps({"scope": self.token, "control": "fd-v1"}, separators=(",", ":")) + "\n").encode()
        request = {"id": "not-run", "argv": ["mkdir", "created"], "caps": ["effect:mutate"]}
        stream.write(auth + (json.dumps(request) + "\n").encode())
        self.assertEqual(json.loads(stream.readline())["jash_control"], 1)
        try: data, ancillary, _, _ = client.recvmsg(256, socket.CMSG_SPACE(4))
        except ConnectionResetError: data, ancillary = b"", []
        self.assertEqual(data, b""); self.assertFalse(ancillary)
        self.assertFalse((self.root / "created").exists())

    def test_pair_failure_is_explicit_before_runtime(self):
        _, stream = self.open_connection()
        stream.write((json.dumps({"scope": self.token, "control": "fd-v1"}, separators=(",", ":")) + "\n").encode())
        self.assertEqual(json.loads(stream.readline()), {"error": "control_unavailable"})
        self.assertEqual(stream.readline(), b"")

    def test_control_only_disconnect_does_not_cancel_or_break_data_session(self):
        _, stream, control, _ = self.connect_control()
        work, notify, release = self.workspace("control-closed")
        self.start(stream, "control-closed", work); self.pause(notify)
        control.close(); os.write(release, b"1")
        terminal = json.loads(stream.readline())
        self.assertEqual(terminal["rc"], 0); self.assertFalse(terminal["cancelled"])
        self.assertFalse((work / "second").exists())
        self.assertEqual(self.request(stream, "still-alive", ["echo", "alive"])["stdout"], "alive\n")
        denied = self.request(stream, "no-control", ["mkdir", "--json", "not-created"], cwd=str(work), cancelable=True)
        self.assertEqual(denied["rc"], 125); self.assertFalse((work / "not-created").exists())

    def test_external_exec_cancellation_uses_the_same_private_channel(self):
        _, stream, control, _ = self.connect_control()
        request = {"id": "external", "argv": ["sleep", "30"], "caps": ["effect:exec"],
                   "timeout_ms": 10000, "cancelable": True}
        stream.write((json.dumps(request) + "\n").encode())
        self.assertTrue(json.loads(stream.readline())["active"])
        control.sendall(b'{"cancel":"external"}\n')
        self.assertTrue(self.control_reply(control)["accepted"])
        terminal = json.loads(stream.readline())
        self.assertEqual(terminal["rc"], 130); self.assertTrue(terminal["cancelled"])
        self.assertFalse(terminal["timed_out"])

    def test_v2_local_cancellation_streams_complete_manifest(self):
        _, stream, control, _ = self.connect_control()
        work, notify, release = self.workspace("v2")
        request = {"id": "v2", "protocol_version": 2, "argv": ["rm", "--json", "first", "second"],
                   "cwd": str(work), "caps": ["effect:mutate"], "cancelable": True}
        stream.write((json.dumps(request) + "\n").encode())
        self.assertEqual(json.loads(stream.readline())["event"], "start")
        self.pause(notify)
        # The first chunk proves the forked pump has run its descriptor cleanup;
        # the parent can reach its mutation barrier before the child is scheduled.
        first_chunk = json.loads(stream.readline())
        self.assertEqual(first_chunk["event"], "chunk")
        chunks = [base64.b64decode(first_chunk["data_b64"])] if first_chunk["stream"] == "stdout" else []
        worker = struct.unpack("3i", control.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))[0]
        children = direct_children(worker)
        self.assertEqual(len(children), 1, "expected one v2 IO pump")
        sockets = {os.readlink(fd) for fd in Path(f"/proc/{children[0]}/fd").iterdir() if os.readlink(fd).startswith("socket:")}
        self.assertEqual(len(sockets), 1, "IO pump must retain only data socket, not cancellation authority")
        control.sendall(b'{"cancel":"v2"}\n'); os.write(release, b"1")
        self.assertTrue(self.control_reply(control)["accepted"])
        while True:
            row = json.loads(stream.readline())
            if row.get("event") != "chunk": break
            if row["stream"] == "stdout": chunks.append(base64.b64decode(row["data_b64"]))
        self.assertEqual(row["rc"], 130); self.assertTrue(row["cancelled"])
        self.assertEqual(json.loads(b"".join(chunks))["status"], "cancelled")
        self.assertTrue((work / "second").exists())

    def test_repeated_connections_do_not_leak_endpoints_into_listener(self):
        directory = Path(f"/proc/{self.server.pid}/fd")
        before = len(list(directory.iterdir()))
        for _ in range(12):
            client, stream, control, _ = self.connect_control()
            stream.close(); client.close()
            self.assertEqual(control.recv(1), b""); control.close()
        deadline = time.monotonic() + 3
        while len(list(directory.iterdir())) != before and time.monotonic() < deadline: time.sleep(.01)
        self.assertEqual(len(list(directory.iterdir())), before)


class ControlOSIF(unittest.TestCase):
    def test_descriptor_ownership_transfer_closed_peer_and_no_leaks(self):
        with tempfile.TemporaryDirectory(prefix="jash-control-osif-") as temporary:
            binary = str(Path(temporary) / "test")
            flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if os.environ.get("SANITIZE") else []
            subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", *flags, "-I", str(fixture.ROOT / "c"),
                str(fixture.ROOT / "tests/test_ipc_control.c"), str(fixture.ROOT / "c/osif/ipc.c"), "-o", binary],
                check=True, capture_output=True, timeout=30)
            subprocess.run([binary], check=True, capture_output=True, timeout=10)


if __name__ == "__main__": unittest.main()
