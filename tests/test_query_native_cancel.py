#!/usr/bin/env python3
"""Accepted native cancellation survives the final observation IO boundary."""

import base64
import fcntl
import json
import os
from pathlib import Path
import select
import socket
import subprocess
import tempfile
import time
import unittest

import native_data_fixture as fixture


class QueryCancellation(fixture.NativeQueryCase):
    @classmethod
    def setUpClass(cls):
        temporary = tempfile.TemporaryDirectory(prefix="jash-query-cancel-hook-")
        cls.addClassCleanup(temporary.cleanup)
        cls.hook = str(Path(temporary.name) / "hook.so")
        subprocess.run(["cc", "-std=gnu11", "-shared", "-fPIC", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                        str(fixture.ROOT / "tests/query_cancel_faults.c"), "-ldl", "-o", cls.hook],
                       check=True, capture_output=True, timeout=30)

    def test_cancel_thirteen_native_producers_and_reuse_session(self):
        (self.work / "cancel-root").mkdir()
        (self.work / "cancel-root/entry").write_bytes(b"data")
        (self.work / "cancel-file").write_bytes(b"count these bytes\n")
        (self.work / "done-file").write_bytes(b"alive")
        checkpoints = self.root / "home/.jash"
        checkpoints.mkdir(exist_ok=True)
        (checkpoints / "checkpoints.tsv").write_text("cancelbase\t0\t0\t/work\n")
        cases = ((["glob", "--data-v2", "cancel-root/*"], "directory"),
                 (["find", "--data-v2", "cancel-root"], "directory"),
                 (["cat", "--data-v2", "cancel-file"], "page"),
                 (["stat", "--data-v2", "cancel-file"], "metadata"),
                 (["wc", "--data-v2", "cancel-file"], "count"),
                 (["df", "--data-v2", "."], "mounts"),
                 (["du", "--data-v2", "cancel-root"], "directory"),
                 (["grep", "--data-v2", "count", "cancel-file"], "count"),
                 (["diff", "--data-v2", "cancel-file", "done-file"], "count"),
                 (["tread", "--data-v2", "cancel-file"], "page"),
                 (["tdiff", "--data-v2", "cancel-file", "@cancelbase"], "count"),
                 (["awk", "--data-v2", "{print}", "cancel-file"], "count"),
                 (["verify", "--data-v2", "cancel-file"], "count"))
        for case, (argv, point) in enumerate(cases):
            with self.subTest(command=argv[0]):
                self.cancel_case(case, argv, point)

    def test_mesh_query_cancels_while_another_writer_holds_store_lock(self):
        made=self.run_command(["mesh","--json","register","--id","worker"])
        self.assertEqual(made.returncode,0,made.stderr)
        (self.work / "done-file").write_bytes(b"alive")
        path=Path(self.env["HOME"]) / ".jash/mesh/.lock"
        with path.open("rb") as lock:
            fcntl.flock(lock,fcntl.LOCK_EX)
            self.cancel_case(100,["mesh","--data-v2","workers"],"mesh-lock")

    def cancel_case(self, case, argv, point):
        control, child = socket.socketpair()
        ready, notify = os.pipe()
        release, send_release = os.pipe()
        env = dict(self.env, LD_PRELOAD=self.hook, JASH_AGENT_CANCEL_FD=str(child.fileno()),
                   JASH_TEST_QUERY_READY=str(notify), JASH_TEST_QUERY_RELEASE=str(release),
                   JASH_TEST_QUERY_POINT=point)
        proc = subprocess.Popen([str(fixture.AGENT)], cwd=self.work, env=env,
                                pass_fds=(child.fileno(), notify, release),
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, bufsize=0)
        child.close(); os.close(notify); os.close(release)
        pending = {}

        def line(stream):
            fd = stream.fileno()
            data = pending.pop(fd, b"")
            deadline = time.monotonic() + 10
            while b"\n" not in data:
                self.assertTrue(select.select([fd], [], [], max(0, deadline - time.monotonic()))[0],
                                ("response timeout", argv, data))
                chunk = os.read(fd, 65536)
                self.assertTrue(chunk, ("unexpected EOF", argv, data))
                data += chunk
            row, _, rest = data.partition(b"\n")
            pending[fd] = rest
            return json.loads(row)

        try:
            hello = line(proc.stdout)
            self.assertTrue(hello["cancel"]["enabled"])
            ident = f"query-cancel-{case}"
            request = {"id": ident, "protocol_version": 2, "argv": argv,
                       "caps": ["effect:read"], "cancelable": True}
            proc.stdin.write((json.dumps(request) + "\n").encode())
            start = line(proc.stdout)
            self.assertEqual(start.get("event"), "start", start)
            self.assertTrue(select.select([ready], [], [], 10)[0], ("IO barrier not reached", argv))
            self.assertEqual(os.read(ready, 1), b"1")
            control.sendall((json.dumps({"cancel": ident}, separators=(",", ":")) + "\n").encode())
            os.write(send_release, b"1")
            self.assertEqual(line(control), {"cancel": ident, "accepted": True})
            chunks = []
            while True:
                response = line(proc.stdout)
                if response.get("event") != "chunk": break
                if response["stream"] == "stdout": chunks.append(base64.b64decode(response["data_b64"]))
            self.assertEqual(response["rc"], 130, response)
            self.assertTrue(response["cancelled"], response)
            # verify uses discovery's declared zero logical credits; filesystem
            # queries declare canonical JDV1 value bytes. Assert either policy,
            # including the corresponding terminal sum, never ignore credits.
            stream = fixture.stream(b"".join(chunks), credit_kind="none" if argv[0] in ("verify","mesh") else "canonical")
            self.assertEqual((stream.state, stream.producer, stream.reason), (1, 130, "request_cancelled"))
            self.assertFalse(stream.truncated)
            again = {"id": f"query-after-{case}", "protocol_version": 2,
                     "argv": ["stat", "--data-v2", "done-file"], "caps": ["effect:read"]}
            proc.stdin.write((json.dumps(again) + "\n").encode())
            self.assertEqual(line(proc.stdout).get("event"), "start")
            chunks = []
            while True:
                response = line(proc.stdout)
                if response.get("event") != "chunk": break
                if response["stream"] == "stdout": chunks.append(base64.b64decode(response["data_b64"]))
            self.assertEqual(response["rc"], 0, response)
            self.assertEqual(fixture.stream(b"".join(chunks)).state, 0)
        finally:
            control.close(); os.close(ready); os.close(send_release)
            if proc.poll() is None: proc.kill()
            proc.communicate(timeout=10)


if __name__ == "__main__":
    unittest.main()
