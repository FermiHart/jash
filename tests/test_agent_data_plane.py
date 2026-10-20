#!/usr/bin/env python3
"""ADR-0056 W09–W14: bounded Linux control/data-plane integration."""
import json
import subprocess
import unittest
import tempfile
import os
import socket
import array
import time
import types
import base64
import fcntl
import test_agent_local_cancel as hooks
from pathlib import Path
import test_agent_mutations_json as fixture

AGENT = Path(os.environ.get("JASH_AGENTD_BIN", fixture.ROOT / "build/jash-agentd")).resolve()
VERIFIER = Path(os.environ.get("JASH_VERIFIER_BIN", fixture.ROOT / "build/jef_verify")).resolve()

def make_memfd(name):
    if hasattr(os, "memfd_create"):
        return os.memfd_create(name, os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING)
    # Some Linux Python builds omit this wrapper despite libc/kernel support.
    # Exercise the identical syscall rather than skipping the descriptor gate.
    import ctypes
    libc = ctypes.CDLL(None, use_errno=True)
    create = libc.memfd_create; create.argtypes = [ctypes.c_char_p, ctypes.c_uint]; create.restype = ctypes.c_int
    fd = create(name.encode(), 1 | 2)  # Linux MFD_CLOEXEC | MFD_ALLOW_SEALING
    if fd < 0: raise OSError(ctypes.get_errno(), "memfd_create")
    return fd

def seal_memfd(fd):
    # Stable Linux UAPI constants; Python's fcntl exposure is build-dependent.
    fcntl.fcntl(fd, getattr(fcntl, "F_ADD_SEALS", 1033), 1 | 2 | 4 | 8)

class Runtime(unittest.TestCase):
    setUp = fixture.Mutations.setUp

    def start(self):
        proc = subprocess.Popen([str(AGENT)], cwd=self.work, env=self.env,
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(self.stop, proc)
        self.handshake = json.loads(proc.stdout.readline())
        return proc

    def stop(self, proc):
        if proc.poll() is None:
            proc.stdin.close(); proc.stdin = None
            try: proc.communicate(timeout=10)
            except subprocess.TimeoutExpired: proc.kill(); proc.communicate(timeout=10)

    def send(self, proc, op, **fields):
        caps = fields.pop("caps", ["store:read", "store:manage", "data:create", "data:read", "data:manage"])
        identifier = fields.pop("id", "data-request")
        request = dict(id=identifier, protocol_version=2, op=op, caps=caps, **fields)
        self.last_wire = (json.dumps(request) + "\n").encode()
        proc.stdin.write(self.last_wire); proc.stdin.flush()
        return json.loads(proc.stdout.readline())

    def once(self, op, **fields):
        proc = self.start()
        try: return self.send(proc, op, **fields)
        finally: self.stop(proc)

    def local(self, control=False):
        self.work.chmod(0o700)
        scope = self.work / "scope"; scope.write_text("a" * 64); scope.chmod(0o600)
        path = self.work / "socket"
        server = subprocess.Popen([str(AGENT), "--listen", str(path), "--scope-file", str(scope)], cwd=self.work, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        def finish_server():
            server.terminate()
            try: server.communicate(timeout=10)
            except subprocess.TimeoutExpired: server.kill(); server.communicate(timeout=10)
        self.addCleanup(finish_server)
        deadline = time.monotonic() + 10
        client = socket.socket(socket.AF_UNIX); client.settimeout(10)
        while True:
            try: client.connect(str(path)); break
            except (FileNotFoundError, ConnectionRefusedError):
                if server.poll() is not None: self.fail(server.communicate(timeout=5))
                if time.monotonic() > deadline: raise
                time.sleep(.01)
        stream = client.makefile("rwb", buffering=0)
        json.loads(stream.readline())
        auth = dict(scope="a" * 64)
        if control: auth["control"] = "fd-v1"
        auth["data"] = "fd-v1"
        stream.write((json.dumps(auth, separators=(",", ":")) + "\n").encode())
        lane = None
        for marker in ([b"C", b"D"] if control else [b"D"]):
            notice = json.loads(stream.readline()); self.assertIn("attachment", notice, notice)
            byte, ancillary, flags, _ = client.recvmsg(1, socket.CMSG_SPACE(4), socket.MSG_CMSG_CLOEXEC)
            self.assertEqual(byte, marker); self.assertFalse(flags & socket.MSG_CTRUNC)
            descriptors = array.array("i"); descriptors.frombytes(ancillary[0][2]); self.assertEqual(len(descriptors), 1)
            if marker == b"C": os.close(descriptors[0])
            else: lane = socket.socket(fileno=descriptors[0])
        handshake = json.loads(stream.readline()); self.handshake = handshake
        self.addCleanup(client.close); self.addCleanup(stream.close); self.addCleanup(lane.close)
        return types.SimpleNamespace(stdin=stream, stdout=stream, lane=lane)

class Budget(Runtime):
    def test_observation_distinguishes_unknown_metrics_and_counts_managed_bytes(self):
        response = self.once("budget.get")
        self.assertEqual(response.get("schema"), "jash.agent-budget.v1", response)
        self.assertEqual(response["journal"]["used_bytes"], 0)
        self.assertIsNone(response["rate_bytes_per_second"]); self.assertIsNone(response["eta_seconds"])
        self.assertIsNone(response["memory_available_bytes"])
        directory = self.home / ".jash/agent_maintenance"; directory.mkdir(mode=0o700)
        path = directory / ".retained"; path.write_bytes(b"unique"); path.chmod(0o600)
        self.assertEqual(self.once("budget.get")["journal"]["used_bytes"], 6)
        path.chmod(0o644)
        self.assertIsNone(self.once("budget.get")["journal"]["used_bytes"])
        self.assertEqual(path.read_bytes(), b"unique")

    def test_sparse_and_hardlinked_files_are_not_evicted_or_miscounted(self):
        human = self.work / "human-untracked"; human.write_bytes(b"keep me")
        self.once("budget.get")
        journal = self.home / ".jash/agent_maintenance"; journal.mkdir(mode=0o700)
        sparse = journal / ".sparse"
        with sparse.open("wb") as stream: stream.truncate(1024 * 1024)
        sparse.chmod(0o600)
        result = self.once("budget.get")
        self.assertEqual(result["journal"]["used_bytes"], 1024 * 1024)
        os.link(sparse, journal / ".second-link")
        result = self.once("budget.get"); self.assertFalse(result["measurement_complete"])
        self.assertIsNone(result["journal"]["used_bytes"]); self.assertEqual(human.read_bytes(), b"keep me")
        self.assertEqual(sparse.stat().st_nlink, 2)

    def test_budget_requires_read_capability(self):
        self.assertEqual(self.once("budget.get", caps=["data:read"])["error"], "capability denied")

class Transport(Runtime):
    def test_probe_and_required_property_do_not_silently_fallback(self):
        probe = self.once("data.probe")
        self.assertEqual(probe.get("schema"), "jash.agent-data-probe.v1", probe)
        self.assertTrue(probe["sealed_fd"]["compiled"]); self.assertTrue(probe["sealed_fd"]["available"])
        self.assertEqual(self.once("data.probe", transport="sealed-fd")["error"], "data_lane_unavailable")
        policy = self.once("data.policy.get")
        self.assertTrue(self.once("data.policy.set", revision=policy["revision"], max_bytes=4096, fd_enabled=False)["ok"])
        self.assertEqual(self.once("data.probe", transport="sealed-fd")["error"], "transport_denied")
        self.assertTrue(self.once("data.probe", transport="inline")["ok"])
        self.assertEqual(self.once("data.probe", transport="dma-buf")["error"], "transport_unsupported")

    def test_osif_and_recorded_baseline(self):
        with tempfile.TemporaryDirectory(prefix="jash-data-osif-") as temp:
            binary = str(Path(temp) / "test")
            flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if os.environ.get("SANITIZE") else []
            command = ["cc", "-std=gnu11", "-O1", "-g", *flags, "-I", str(fixture.ROOT / "c"),
                       str(fixture.ROOT / "tests/test_data_plane_osif.c"), str(fixture.ROOT / "c/osif/ipc.c"),
                       str(fixture.ROOT / "c/osif/buffer.c"), str(fixture.ROOT / "c/module/blake3.c"), "-o", binary]
            built = subprocess.run(command, capture_output=True, timeout=120); self.assertEqual(built.returncode, 0, built.stderr)
            run = subprocess.run([binary, "benchmark"], capture_output=True, timeout=30); self.assertEqual(run.returncode, 0, run.stderr)
            baseline = json.loads(run.stdout); self.assertEqual(len(baseline["samples"]), 3)
            for sample in baseline["samples"]:
                self.assertGreater(sample["create_seal_ns"], 0); self.assertGreater(sample["transfer_map_hash_ns"], 0)
            if os.environ.get("JASH_DATA_BASELINE"):
                with open(os.environ["JASH_DATA_BASELINE"], "xb") as output: output.write(run.stdout)

    def test_compiled_out_backend_refuses_required_properties(self):
        with tempfile.TemporaryDirectory(prefix="jash-data-reduced-") as temp:
            binary = str(Path(temp) / "test")
            command = ["cc", "-std=gnu11", "-U__linux__", "-I", str(fixture.ROOT / "c"),
                       str(fixture.ROOT / "tests/test_data_plane_reduced.c"), str(fixture.ROOT / "c/osif/ipc.c"),
                       str(fixture.ROOT / "c/osif/buffer.c"), "-o", binary]
            built = subprocess.run(command, capture_output=True, timeout=120); self.assertEqual(built.returncode, 0, built.stderr)
            self.assertEqual(subprocess.run([binary], capture_output=True, timeout=10).returncode, 0)

class FD(Runtime):
    def test_opt_in_private_lane_and_control_coexist(self):
        connection = self.local(control=True)
        self.assertEqual(connection.lane.getsockopt(socket.SOL_SOCKET, socket.SO_TYPE), socket.SOCK_SEQPACKET)
        self.assertFalse(os.get_inheritable(connection.lane.fileno()))
        self.assertTrue(self.send(connection, "data.probe", transport="sealed-fd")["ok"])

    def frame(self, id, size):
        raw = id.encode(); return b"JDF1" + bytes([len(raw)]) + b"\0" * 3 + raw.ljust(48, b"\0") + size.to_bytes(8, "big")

    def test_import_associates_frame_and_rejects_unsealed_or_extra_descriptors(self):
        connection = self.local()
        fd = make_memfd("producer")
        self.addCleanup(os.close, fd); os.write(fd, b"shared")
        def offer(id="data-request", count=1):
            connection.lane.sendmsg([self.frame(id, 6)], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", [fd] * count))])
        offer()
        self.assertEqual(self.send(connection, "data.import", transport="sealed-fd")["error"], "buffer_not_immutable")
        seal_memfd(fd)
        offer("wrong")
        self.assertEqual(self.send(connection, "data.import", transport="sealed-fd")["error"], "attachment_invalid")
        offer(count=2)
        self.assertEqual(self.send(connection, "data.import", transport="sealed-fd")["error"], "attachment_invalid")
        offer()
        imported = self.send(connection, "data.import", transport="sealed-fd")
        self.assertTrue(imported["ok"], imported); self.assertEqual(imported["copied_bytes"], 0)
        read = self.send(connection, "data.read", artifact_ref=imported["artifact_ref"], offset=1, limit=3)
        self.assertEqual(base64.b64decode(read["data_b64"]), b"har")
        self.assertEqual(os.lseek(fd, 0, os.SEEK_CUR), 6)
        program = "import os; print([os.readlink('/proc/self/fd/'+f) for f in os.listdir('/proc/self/fd') if os.path.exists('/proc/self/fd/'+f) and os.readlink('/proc/self/fd/'+f).startswith(('socket:', '/memfd:'))])"
        job = self.send(connection, "exec", id="local-fd-isolation", argv=["/usr/bin/python3", "-c", program], caps=["effect:exec", "job:create"])
        self.assertEqual(job["rc"], 0, job)
        output = self.send(connection, "job.read", job_id=job["job_id"], stream="stdout", offset=0, limit=4096, caps=["job:read"])
        self.assertEqual(base64.b64decode(output["data_b64"]).strip(), b"[]")

    def test_repeated_bad_frames_close_received_descriptors(self):
        connection = self.local(); worker = self.handshake["pid"]
        fd = make_memfd("bad-frames"); self.addCleanup(os.close, fd)
        before = len(list(Path(f"/proc/{worker}/fd").iterdir()))
        for _ in range(30):
            connection.lane.sendmsg([self.frame("wrong", 0)], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", [fd]))])
            self.assertEqual(self.send(connection, "data.import", transport="sealed-fd")["error"], "attachment_invalid")
        connection.lane.sendmsg([self.frame("data-request", 0)], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", [fd]))])
        self.assertEqual(self.send(connection, "data.import", transport="sealed-fd", caps=["data:read"])["error"], "capability denied")
        self.assertEqual(self.send(connection, "data.import", transport="sealed-fd")["error"], "attachment_missing")
        self.assertEqual(len(list(Path(f"/proc/{worker}/fd").iterdir())), before)

    def test_stdio_cannot_claim_an_attachment_lane(self):
        self.assertEqual(self.once("data.import", transport="sealed-fd")["error"], "data_lane_unavailable")

class Buffers(Runtime):
    def test_inline_binary_ranges_release_and_session_lifetime(self):
        proc = self.start(); payload = b"a\x00\xffbinary"
        value = self.send(proc, "data.import", transport="inline", data_b64=base64.b64encode(payload).decode())
        self.assertTrue(value.get("ok"), value); ref = value["artifact_ref"]
        self.assertEqual(value["durability"], "volatile")
        for offset in (3, 0, len(payload)):
            got = self.send(proc, "data.read", artifact_ref=ref, offset=offset, limit=65536)
            self.assertEqual(base64.b64decode(got["data_b64"]), payload[offset:])
        self.assertEqual(self.send(proc, "budget.get")["volatile_buffer_bytes"], len(payload))
        self.assertTrue(self.send(proc, "data.release", artifact_ref=ref)["ok"])
        repeated = self.send(proc, "data.release", artifact_ref=ref)
        self.assertTrue(repeated["ok"]); self.assertFalse(repeated["release_applied"])
        self.assertEqual(self.send(proc, "data.read", artifact_ref=ref, offset=0, limit=1)["error"], "volatile_ref_unavailable")
        fresh = self.send(proc, "data.import", transport="inline", data_b64="")
        self.assertNotEqual(fresh["artifact_ref"], ref)
        self.assertFalse(self.send(proc, "data.release", artifact_ref=ref)["release_applied"])
        self.assertTrue(self.send(proc, "data.read", artifact_ref=fresh["artifact_ref"], offset=0, limit=1)["ok"])
        self.stop(proc)
        self.assertEqual(self.once("data.read", artifact_ref=fresh["artifact_ref"], offset=0, limit=1)["error"], "volatile_ref_unavailable")

    def test_policy_bytes_and_slot_limits(self):
        policy = self.once("data.policy.get")
        self.assertTrue(self.once("data.policy.set", revision=policy["revision"], max_bytes=4, fd_enabled=True)["ok"])
        proc = self.start()
        first = self.send(proc, "data.import", transport="inline", data_b64="YWJjZA==")
        self.assertTrue(first["ok"])
        self.assertEqual(self.send(proc, "data.import", transport="inline", data_b64="eA==")["error"], "buffer_quota_exceeded")
        for _ in range(3): self.assertTrue(self.send(proc, "data.import", transport="inline", data_b64="")["ok"])
        self.assertEqual(self.send(proc, "data.import", transport="inline", data_b64="")["error"], "buffer_slots_exhausted")

    def test_external_workload_does_not_inherit_buffer_fds(self):
        proc = self.start()
        self.assertTrue(self.send(proc, "data.import", transport="inline", data_b64="YWJj")["ok"])
        program = "import os; print([os.readlink('/proc/self/fd/'+f) for f in os.listdir('/proc/self/fd') if os.path.exists('/proc/self/fd/'+f) and 'memfd:' in os.readlink('/proc/self/fd/'+f)])"
        reply = self.send(proc, "exec", id="no-buffer-inherit", argv=["/usr/bin/python3", "-c", program], caps=["effect:exec", "job:create"])
        self.assertEqual(reply["rc"], 0, reply)
        output = self.send(proc, "job.read", job_id=reply["job_id"], stream="stdout", offset=0, limit=4096, caps=["job:read"])
        self.assertEqual(base64.b64decode(output["data_b64"]).strip(), b"[]")

    def test_release_survives_corrupt_policy_and_rejects_remote_fd_numbers(self):
        proc = self.start(); value = self.send(proc, "data.import", transport="inline", data_b64="eA==")
        path = self.home / ".jash/agent_store/data_policy"; path.write_bytes(b"broken"); path.chmod(0o600)
        self.assertTrue(self.send(proc, "data.release", artifact_ref=value["artifact_ref"])["ok"])
        self.assertEqual(self.send(proc, "data.read", artifact_ref="3", offset=0, limit=1)["error"], "volatile_ref_unavailable")
        self.assertEqual(self.send(proc, "data.import", transport="inline", data_b64="eA==")["error"], "policy_invalid")

class Artifacts(Runtime):
    setUpClass = classmethod(hooks.LocalCancel.setUpClass.__func__)
    def test_restart_replay_handoff_and_independent_receipt(self):
        proc = self.start(); payload = b"durable\x00\xffcontents"
        self.assertEqual(self.handshake["data_plane"]["promotion_ids"], "persistent-exact-request")
        self.assertEqual(self.handshake["data_plane"]["ids"], "correlation-except-promotion")
        buffer = self.send(proc, "data.import", transport="inline", data_b64=base64.b64encode(payload).decode())
        result = self.send(proc, "data.promote", artifact_ref=buffer["artifact_ref"], id="durable-once")
        wire = self.last_wire
        self.assertEqual(result.get("schema"), "jash.agent-artifact.v1", result)
        self.assertTrue(result["published"]); self.assertEqual(result["durability"], "durable")
        self.stop(proc)
        self.assertEqual(self.once("data.promote", artifact_ref=buffer["artifact_ref"], id="durable-once"), result)
        proc = self.start(); attached = self.send(proc, "data.attach", artifact_ref=result["artifact_ref"])
        self.assertTrue(attached["ok"], attached)
        got = self.send(proc, "data.read", artifact_ref=attached["artifact_ref"], offset=0, limit=100)
        self.assertEqual(base64.b64decode(got["data_b64"]), payload)
        self.stop(proc)
        gc = self.once("store.gc", id="gc-durable", older_than_seconds=0)
        self.assertTrue(gc["ok"], gc)
        path = self.home / ".jash/agent_artifacts" / result["artifact_ref"][2:]
        self.assertEqual(path.read_bytes(), payload)
        req = self.work / "request.json"; req.write_bytes(wire)
        proof = self.home / ".jash/agent_artifact_receipts" / result["receipt_ref"]
        command = [str(VERIFIER), "artifact-receipt", result["receipt_pubkey"], str(req), str(path), str(proof)]
        checked = subprocess.run(command, capture_output=True, timeout=10); self.assertEqual(checked.returncode, 0, checked.stderr)
        path.write_bytes(b"changed")
        self.assertNotEqual(subprocess.run(command, capture_output=True, timeout=10).returncode, 0)
        self.assertFalse(self.once("data.attach", artifact_ref=result["artifact_ref"])["ok"])

    def test_dedup_quota_and_attached_snapshot_survive_source_change(self):
        policy = self.once("data.policy.get")
        self.assertTrue(self.once("data.policy.set", revision=policy["revision"], max_bytes=4, fd_enabled=True)["ok"])
        proc = self.start()
        buffer = self.send(proc, "data.import", transport="inline", data_b64="YWJjZA==")
        first = self.send(proc, "data.promote", artifact_ref=buffer["artifact_ref"], id="first")
        second = self.send(proc, "data.promote", artifact_ref=buffer["artifact_ref"], id="dedup")
        self.assertFalse(second["published"]); self.assertEqual(first["artifact_ref"], second["artifact_ref"])
        self.send(proc, "data.release", artifact_ref=buffer["artifact_ref"])
        attached = self.send(proc, "data.attach", artifact_ref=first["artifact_ref"])
        path = self.home / ".jash/agent_artifacts" / first["artifact_ref"][2:]; path.write_bytes(b"zzzz")
        got = self.send(proc, "data.read", artifact_ref=attached["artifact_ref"], offset=0, limit=4)
        self.assertEqual(base64.b64decode(got["data_b64"]), b"abcd")
        self.send(proc, "data.release", artifact_ref=attached["artifact_ref"])
        other = self.send(proc, "data.import", transport="inline", data_b64="eHl6")
        self.assertEqual(self.send(proc, "data.promote", artifact_ref=other["artifact_ref"], id="overflow")["error"], "artifact_quota_exceeded")
        self.assertEqual(path.read_bytes(), b"zzzz")

    def test_failed_publication_preserves_staging_and_refuses_ambiguous_replay(self):
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_DATA_PUBLISH_FAIL="1")
        proc = self.start(); value = self.send(proc, "data.import", transport="inline", data_b64="YWJj")
        failed = self.send(proc, "data.promote", artifact_ref=value["artifact_ref"], id="publish-failure")
        self.assertEqual(failed["outcome"], "indeterminate")
        self.stop(proc); self.env.pop("LD_PRELOAD"); self.env.pop("JASH_TEST_DATA_PUBLISH_FAIL")
        stages = list((self.home / ".jash/agent_artifacts").glob(".publish-*.tmp"))
        self.assertEqual(len(stages), 1); self.assertEqual(stages[0].read_bytes(), b"abc")
        replay = self.once("data.promote", artifact_ref=value["artifact_ref"], id="publish-failure")
        self.assertEqual(replay["outcome"], "indeterminate"); self.assertTrue(stages[0].exists())

    def test_quota_refusal_before_staging_allows_same_id_after_policy_increase(self):
        policy = self.once("data.policy.get")
        self.once("data.policy.set", revision=policy["revision"], max_bytes=4, fd_enabled=True)
        proc = self.start(); first = self.send(proc, "data.import", transport="inline", data_b64="YWJj")
        self.assertTrue(self.send(proc, "data.promote", artifact_ref=first["artifact_ref"], id="stored-first")["ok"])
        self.send(proc, "data.release", artifact_ref=first["artifact_ref"])
        second = self.send(proc, "data.import", transport="inline", data_b64="eHk=")
        denied = self.send(proc, "data.promote", artifact_ref=second["artifact_ref"], id="quota-retry")
        self.assertEqual(denied["error"], "artifact_quota_exceeded"); self.assertNotIn("outcome", denied)
        policy = self.send(proc, "data.policy.get")
        self.assertTrue(self.send(proc, "data.policy.set", revision=policy["revision"], max_bytes=8, fd_enabled=True)["ok"])
        self.assertTrue(self.send(proc, "data.promote", artifact_ref=second["artifact_ref"], id="quota-retry")["ok"])

    def test_sigkill_after_publication_keeps_content_without_forging_terminal(self):
        marker = self.work / "published"
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_DATA_PUBLISH_GATE=str(marker))
        proc = self.start(); value = self.send(proc, "data.import", transport="inline", data_b64="YWJj")
        request = dict(id="killed-promotion", protocol_version=2, op="data.promote", caps=["data:manage"], artifact_ref=value["artifact_ref"])
        proc.stdin.write((json.dumps(request) + "\n").encode()); proc.stdin.flush()
        deadline = time.monotonic() + 10
        while not marker.exists() and proc.poll() is None and time.monotonic() < deadline: time.sleep(.01)
        self.assertTrue(marker.exists()); proc.kill(); proc.communicate(timeout=10)
        self.env.pop("LD_PRELOAD"); self.env.pop("JASH_TEST_DATA_PUBLISH_GATE")
        durable = "a:blake3-" + value["blake3"] + "-3.bin"
        self.assertTrue(self.once("data.attach", artifact_ref=durable)["ok"])
        replay = self.once("data.promote", artifact_ref=value["artifact_ref"], id="killed-promotion", caps=["data:manage"])
        self.assertEqual(replay["outcome"], "indeterminate")

    def test_independent_verifier_input_bounds(self):
        proc = self.start(); value = self.send(proc, "data.import", transport="inline", data_b64="AAH/")
        result = self.send(proc, "data.promote", artifact_ref=value["artifact_ref"], id="verify-inputs")
        wire = self.last_wire; self.stop(proc)
        req = self.work / "request"; req.write_bytes(wire)
        artifact = self.home / ".jash/agent_artifacts" / result["artifact_ref"][2:]
        receipt = self.home / ".jash/agent_artifact_receipts" / result["receipt_ref"]
        with tempfile.TemporaryDirectory(prefix="jash-artifact-verifier-") as temp:
            binary = str(Path(temp) / "verify")
            flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if os.environ.get("SANITIZE") else []
            sources = ["tests/test_artifact_receipt_verify.c", "libjef/jef_verify.c", "libjef/ed25519.c", "c/module/blake3.c", "c/module/jash_crypto.c"]
            command = ["cc", "-std=gnu11", "-O1", "-g", *flags, "-I", str(fixture.ROOT / "libjef"), "-I", str(fixture.ROOT / "c")]
            command += [str(fixture.ROOT / source) for source in sources] + ["-o", binary]
            built = subprocess.run(command, capture_output=True, timeout=180); self.assertEqual(built.returncode, 0, built.stderr)
            args = [binary, str(receipt), str(req), str(artifact), result["receipt_pubkey"]]
            self.assertEqual(subprocess.run(args, capture_output=True, timeout=10).returncode, 0)
            self.assertEqual(subprocess.run(args[:-1] + ["0" * 64], capture_output=True, timeout=10).returncode, 1)
            self.assertEqual(subprocess.run(args[:-1] + ["bad"], capture_output=True, timeout=10).returncode, 2)
            req.write_bytes(wire.replace(b'{"id":', b'{"id":"duplicate","id":', 1))
            self.assertEqual(subprocess.run(args, capture_output=True, timeout=10).returncode, 1)
            with req.open("wb") as stream: stream.truncate(32769)
            self.assertEqual(subprocess.run(args, capture_output=True, timeout=10).returncode, 2)
            req.unlink(); os.mkfifo(req)
            self.assertEqual(subprocess.run(args, capture_output=True, timeout=10).returncode, 2)

class Config(Runtime):
    setUpClass = classmethod(hooks.LocalCancel.setUpClass.__func__)
    def test_revision_guard_dry_run_and_persistence(self):
        first = self.once("data.policy.get")
        self.assertEqual(first.get("schema"), "jash.agent-data-policy.v1", first)
        preview = self.once("data.policy.set", revision=first["revision"], max_bytes=4096, fd_enabled=False, dry_run=True)
        self.assertTrue(preview["ok"]); self.assertFalse(preview["applied"])
        self.assertEqual(self.once("data.policy.get")["revision"], first["revision"])
        changed = self.once("data.policy.set", revision=first["revision"], max_bytes=4096, fd_enabled=False)
        self.assertTrue(changed["applied"]); self.assertIsNotNone(changed["intent_receipt_ref"])
        live = self.once("data.policy.get")
        self.assertEqual(live["max_bytes"], 4096); self.assertFalse(live["fd_enabled"])
        conflict = self.once("data.policy.set", revision=first["revision"], max_bytes=1000, fd_enabled=True)
        self.assertEqual(conflict["error"], "revision_conflict")

    def test_invalid_fields_types_and_hard_ceiling_precede_publication(self):
        policy = self.once("data.policy.get"); revision = policy["revision"]
        for fields in (dict(max_bytes=0, fd_enabled=True), dict(max_bytes=16777217, fd_enabled=True), dict(max_bytes=4096, fd_enabled=1)):
            response = self.once("data.policy.set", revision=revision, **fields)
            self.assertIn("error", response)
        self.assertEqual(self.once("data.policy.get")["revision"], revision)
        self.assertFalse((self.home / ".jash/agent_store/data_policy").exists())

    def test_peer_excludes_policy_writer_and_corruption_is_explicit(self):
        policy = self.once("data.policy.get"); peer = self.start()
        denied = self.once("data.policy.set", revision=policy["revision"], max_bytes=4096, fd_enabled=True)
        self.assertEqual(denied["error"], "store_busy"); self.stop(peer)
        path = self.home / ".jash/agent_store/data_policy"; path.write_bytes(b"JDP1 0 1\n"); path.chmod(0o600)
        invalid = self.once("data.policy.get")
        self.assertFalse(invalid["ok"]); self.assertIsNone(invalid["max_bytes"])

    def test_failed_atomic_publication_preserves_previous_policy(self):
        before = self.once("data.policy.get")
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_DATA_POLICY_FAIL="1")
        failed = self.once("data.policy.set", revision=before["revision"], max_bytes=4096, fd_enabled=False)
        self.assertEqual(failed["error"], "policy_publish_failed"); self.assertFalse(failed["applied"])
        self.env.pop("LD_PRELOAD"); self.env.pop("JASH_TEST_DATA_POLICY_FAIL")
        self.assertEqual(self.once("data.policy.get")["revision"], before["revision"])

if __name__ == "__main__": unittest.main()
