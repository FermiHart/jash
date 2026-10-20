#!/usr/bin/env python3
"""Explicit-capability path from an o: sealed snapshot to a durable receipt."""
import array
import base64
import fcntl
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile

import test_agent_data_plane as data_plane


ROOT = Path(__file__).resolve().parents[1]


class ObservationPromotion(data_plane.Runtime):
    @classmethod
    def setUpClass(cls):
        tmp = tempfile.TemporaryDirectory(prefix="jash-observation-promotion-")
        cls.addClassCleanup(tmp.cleanup)
        cls.helper = Path(tmp.name) / "export-ref"
        sources = [
            "tests/ai_ref_fd_export.c", "c/core/ai_observation.c",
            "c/core/data_filter.c", "c/core/data_wire.c", "c/core/data_wire_decode.c",
            "c/osif/buffer.c", "c/osif/ipc.c", "c/builtins/ls_data.c",
            "c/builtins/ls_collector.c", "c/core/data_stream.c",
            "c/core/data_value.c", "c/core/json_writer.c",
            "c/module/blake3.c",
        ]
        command = ["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                   "-pedantic-errors", "-I", str(ROOT / "c")]
        command += [str(ROOT / path) for path in sources]
        command += ["-o", str(cls.helper)]
        built = subprocess.run(command, capture_output=True, timeout=120)
        if built.returncode:
            raise AssertionError(built.stderr.decode(errors="replace"))

        cls.verifier = Path(tmp.name) / "jef_verify"
        verifier_sources = [
            "libjef/jef_verify_main.c", "libjef/jef_verify.c",
            "libjef/ed25519.c", "c/module/blake3.c",
            "c/module/jash_crypto.c",
        ]
        command = ["cc", "-std=gnu11", "-O1", "-I", str(ROOT / "c"),
                   "-I", str(ROOT / "libjef")]
        command += [str(ROOT / path) for path in verifier_sources]
        command += ["-o", str(cls.verifier)]
        built = subprocess.run(command, capture_output=True, timeout=120)
        if built.returncode:
            raise AssertionError(built.stderr.decode(errors="replace"))

    def exported_snapshot(self, directory):
        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        parent.settimeout(15)
        proc = None
        try:
            proc = subprocess.Popen(
                [str(self.helper), str(directory), str(child.fileno())],
                cwd=self.work, env=self.env, pass_fds=(child.fileno(),),
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            child.close()
            marker, ancillary, flags, _ = parent.recvmsg(
                1, socket.CMSG_SPACE(4), socket.MSG_CMSG_CLOEXEC)
            stdout, stderr = proc.communicate(timeout=15)
            self.assertEqual(proc.returncode, 0, stderr)
            self.assertEqual(marker, b"O")
            self.assertFalse(flags & socket.MSG_CTRUNC)
            descriptors = array.array("i")
            self.assertEqual(len(ancillary), 1)
            descriptors.frombytes(ancillary[0][2])
            self.assertEqual(len(descriptors), 1)
            snapshot = descriptors[0]
            self.addCleanup(os.close, snapshot)
            self.assertFalse(os.get_inheritable(snapshot))
            self.assertEqual(fcntl.fcntl(snapshot, fcntl.F_GETFL) & os.O_ACCMODE,
                             os.O_RDONLY)
            self.assertEqual(os.pread(snapshot, 4, 0), b"JTV1")
            observation = json.loads(stdout)
            self.assertTrue(observation["complete"])
            self.assertEqual(os.fstat(snapshot).st_size,
                             observation["stored_bytes"])
            return snapshot, observation
        finally:
            if proc is not None and proc.poll() is None:
                proc.kill()
                proc.communicate(timeout=5)
            parent.close()
            child.close()

    @staticmethod
    def frame(identifier, size):
        raw = identifier.encode()
        assert 1 <= len(raw) <= 47
        return (b"JDF1" + bytes([len(raw)]) + b"\0" * 3 +
                raw.ljust(48, b"\0") + size.to_bytes(8, "big"))

    def test_explicit_caps_and_independent_artifact_receipt(self):
        observed = self.work / "observed"
        observed.mkdir()
        (observed / "ordinary").touch()
        raw_name = b"raw-\xff"
        fd = os.open(os.fsencode(observed) + b"/" + raw_name,
                     os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        os.close(fd)
        snapshot, observation = self.exported_snapshot(observed)
        self.assertEqual(observation["items_seen"], 2)
        connection = self.local()
        policy = self.send(connection, "data.policy.get", caps=["store:read"])
        if not policy["fd_enabled"]:
            enabled = self.send(connection, "data.policy.set",
                                id="allow-sealed-fd", revision=policy["revision"],
                                max_bytes=8 * 1024 * 1024, fd_enabled=True,
                                caps=["store:manage"])
            self.assertTrue(enabled["ok"], enabled)

        denied = self.send(connection, "data.import", id="import-denied",
                           transport="sealed-fd", caps=["data:read"])
        self.assertEqual(denied["error"], "capability denied", denied)

        identifier = "observation-import"
        payload_size = observation["stored_bytes"]
        sent = connection.lane.sendmsg(
            [self.frame(identifier, payload_size)],
            [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
              array.array("i", [snapshot]))])
        self.assertEqual(sent, 64)
        imported = self.send(connection, "data.import", id=identifier,
                             transport="sealed-fd", caps=["data:create"])
        self.assertTrue(imported.get("ok"), imported)
        self.assertEqual(imported["copied_bytes"], 0)
        self.assertEqual(imported["blake3"], observation["ref_blake3"])

        expected = (self.home / ".jash/agent_artifacts" /
                    f"blake3-{imported['blake3']}-{payload_size}.bin")
        refused = self.send(connection, "data.promote", id="promote-denied",
                            artifact_ref=imported["artifact_ref"],
                            caps=["data:read"])
        self.assertEqual(refused["error"], "capability denied", refused)
        self.assertFalse(expected.exists())

        promoted = self.send(connection, "data.promote", id="promote-once",
                             artifact_ref=imported["artifact_ref"],
                             caps=["data:manage"])
        request_wire = self.last_wire
        self.assertTrue(promoted.get("ok"), promoted)
        self.assertEqual(promoted["durability"], "durable")
        self.assertEqual(promoted["blake3"], observation["ref_blake3"])
        self.assertEqual(promoted["bytes"], payload_size)
        self.assertEqual(promoted["artifact_ref"],
                         "a:" + expected.name)
        self.assertEqual(self.send(connection, "data.promote", id="promote-once",
                                   artifact_ref=imported["artifact_ref"],
                                   caps=["data:manage"]), promoted)
        self.assertEqual(expected.read_bytes(), os.pread(snapshot,
                                                         payload_size, 0))

        request = self.work / "promotion-request.json"
        request.write_bytes(request_wire)
        receipt = (self.home / ".jash/agent_artifact_receipts" /
                   promoted["receipt_ref"])
        command = [str(self.verifier), "artifact-receipt",
                   promoted["receipt_pubkey"], str(request), str(expected),
                   str(receipt)]
        checked = subprocess.run(command, capture_output=True, timeout=15)
        self.assertEqual(checked.returncode, 0, checked.stderr)
        modified = self.work / "modified-copy.bin"
        modified.write_bytes(expected.read_bytes() + b"!")
        self.assertNotEqual(subprocess.run(command[:-2] + [str(modified),
                                                    str(receipt)],
                                          capture_output=True,
                                          timeout=15).returncode, 0)

        attached = self.send(connection, "data.attach", id="attach-snapshot",
                             artifact_ref=promoted["artifact_ref"],
                             caps=["data:read"])
        self.assertTrue(attached.get("ok"), attached)
        page = self.send(connection, "data.read", id="read-durable",
                         artifact_ref=attached["artifact_ref"], offset=0,
                         limit=payload_size, caps=["data:read"])
        self.assertTrue(page.get("ok"), page)
        self.assertEqual(base64.b64decode(page["data_b64"]),
                         expected.read_bytes())
        self.assertTrue(base64.b64decode(page["data_b64"]).startswith(b"JTV1"))


if __name__ == "__main__":
    import unittest
    unittest.main()
