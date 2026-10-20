#!/usr/bin/env python3
"""ADR-0056 W15a: credit-driven binary delivery from bounded job spools."""
import base64
import os
import socket
import struct
import subprocess
import time
import unittest

import test_agent_data_plane as data_plane


class BinaryStreams(data_plane.Runtime):
    FRAME_HEADER = 96
    DATA = 1
    EOF = 2
    INCOMPLETE = 4
    CANCELLED = 8

    def start_job(self, connection, script, *, stdin_pipe=False, identifier="stream-job"):
        fields = {
            "id": identifier,
            "argv": ["/usr/bin/python3", "-c", script],
            "background": True,
            "caps": ["effect:exec", "job:create"],
        }
        if stdin_pipe:
            fields["stdin_mode"] = "pipe"
        response = self.send(connection, "job.start", **fields)
        self.assertTrue(response.get("accepted"), response)
        return response["job_id"]

    def wait_job(self, connection, job_id, identifier="stream-wait"):
        response = self.send(connection, "job.wait", id=identifier, job_id=job_id,
                             wait_ms=10000, caps=["job:read"])
        self.assertTrue(response.get("terminal"), response)
        return response

    def open_stream(self, connection, job_id, stream="stdout", identifier="stream-open"):
        response = self.send(connection, "data.stream.open", id=identifier,
                             job_id=job_id, stream=stream, offset=0,
                             caps=["job:read"])
        self.assertTrue(response.get("ok"), response)
        return response["stream_ref"]

    def receive_frame(self, lane):
        packet, ancillary, flags, _ = lane.recvmsg(96 + 16384, socket.CMSG_SPACE(4))
        self.assertFalse(flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC))
        self.assertEqual(ancillary, [])
        self.assertGreaterEqual(len(packet), self.FRAME_HEADER)
        self.assertEqual(packet[:4], b"JDS1")
        frame_flags, ref_length = packet[4], packet[5]
        self.assertEqual(packet[6:8], b"\0\0")
        sequence, offset, length = struct.unpack(">QQQ", packet[8:32])
        self.assertEqual(packet[32 + ref_length:96], b"\0" * (64 - ref_length))
        self.assertEqual(length, len(packet) - self.FRAME_HEADER)
        return {
            "flags": frame_flags,
            "ref": packet[32:32 + ref_length].decode(),
            "sequence": sequence,
            "offset": offset,
            "payload": packet[self.FRAME_HEADER:],
        }

    def credit(self, connection, ref, credit, ack, identifier):
        return self.send(connection, "data.stream.credit", id=identifier,
                         stream_ref=ref, credit_bytes=credit, ack_offset=ack,
                         caps=["job:read"])

    def test_binary_chunks_are_ordered_credit_bounded_and_hash_bound_at_eof(self):
        connection = self.local()
        for field in ("stream_ref", "credit_bytes", "ack_offset"):
            self.assertIn(field, self.handshake["fields"])
        self.assertIn("close_applied", self.handshake["response_fields"])
        payload = bytes(range(256)) * 97
        script = "import os; os.write(1, bytes(range(256))*97)"
        job_id = self.start_job(connection, script)
        terminal = self.wait_job(connection, job_id)
        ref = self.open_stream(connection, job_id)

        rebuilt = bytearray()
        sequence = 0
        ack = 0
        for request in range(16):
            response = self.credit(connection, ref, 4096, ack, f"credit-{request}")
            self.assertTrue(response.get("ok"), response)
            frame = self.receive_frame(connection.lane)
            self.assertEqual(frame["ref"], ref)
            self.assertEqual(frame["sequence"], sequence)
            self.assertEqual(frame["offset"], len(rebuilt))
            if frame["flags"] & self.DATA:
                self.assertLessEqual(len(frame["payload"]), 4096)
                rebuilt.extend(frame["payload"])
                ack = len(rebuilt)
                sequence += 1
                continue
            self.assertEqual(frame["flags"], self.EOF)
            self.assertEqual(frame["payload"], b"")
            self.assertTrue(response["eof"])
            self.assertTrue(response["complete"])
            self.assertEqual(response["final_blake3"], terminal["stdout_blake3"])
            self.assertEqual(response["final_ref"], terminal["stdout_ref"])
            break
        else:
            self.fail("stream did not produce EOF")
        self.assertEqual(bytes(rebuilt), payload)

    def test_fanout_has_independent_cursors_and_consumer_cancel_is_local(self):
        connection = self.local()
        job_id = self.start_job(connection, "import os; os.write(1, b'fanout')")
        self.wait_job(connection, job_id)
        first = self.open_stream(connection, job_id, identifier="open-first")
        second = self.open_stream(connection, job_id, identifier="open-second")
        cancelled = self.send(connection, "data.stream.cancel", id="cancel-first",
                              stream_ref=first, caps=["job:read"])
        self.assertTrue(cancelled["cancel_applied"])
        repeated = self.send(connection, "data.stream.cancel", id="cancel-first-again",
                             stream_ref=first, caps=["job:read"])
        self.assertFalse(repeated["cancel_applied"])
        self.assertEqual(self.credit(connection, first, 16, 0, "credit-cancelled")["error"],
                         "stream unavailable")
        response = self.credit(connection, second, 16, 0, "credit-second")
        self.assertTrue(response["packet_sent"])
        frame = self.receive_frame(connection.lane)
        self.assertEqual(frame["payload"], b"fanout")

    def test_full_lane_returns_without_advancing_and_control_remains_responsive(self):
        connection = self.local()
        job_id = self.start_job(
            connection,
            "import os; chunk=b'x'*16384; [os.write(1, chunk) for _ in range(128)]",
            identifier="blocked-job",
        )
        self.wait_job(connection, job_id, identifier="blocked-wait")
        ref = self.open_stream(connection, job_id, identifier="blocked-open")
        ack = 0
        blocked = None
        for request in range(256):
            response = self.credit(connection, ref, 16384, ack, f"blocked-credit-{request}")
            self.assertTrue(response.get("ok"), response)
            if response["blocked"]:
                blocked = response
                break
            ack = response["next_offset"]
        self.assertIsNotNone(blocked, "seqpacket lane did not reach bounded EAGAIN")
        self.assertFalse(blocked["packet_sent"])
        self.assertFalse(blocked["credit_applied"])
        budget = self.send(connection, "budget.get", id="blocked-budget", caps=["store:read"])
        self.assertTrue(budget["ok"])
        status = self.send(connection, "job.get", id="blocked-status", job_id=job_id,
                           caps=["job:read"])
        self.assertTrue(status["terminal"])
        drained = []
        while True:
            try:
                drained.append(self.receive_frame(connection.lane))
            except BlockingIOError:
                break
        self.assertTrue(drained)
        retried = self.credit(connection, ref, 16384, ack, "blocked-retry")
        self.assertTrue(retried["packet_sent"], retried)
        retry_frame = self.receive_frame(connection.lane)
        self.assertEqual(retry_frame["sequence"], blocked["next_sequence"])
        self.assertEqual(retry_frame["offset"], blocked["next_offset"])
        self.assertLess(drained[0]["offset"], retry_frame["offset"])

    def test_stdin_and_lane_half_close_keep_opposite_direction_usable(self):
        connection = self.local()
        job_id = self.start_job(
            connection,
            "import sys; data=sys.stdin.buffer.read(); print(len(data))",
            stdin_pipe=True,
            identifier="stdin-job",
        )
        closed = self.send(connection, "job.close-stdin", id="close-stdin",
                           job_id=job_id, caps=["job:stdin:write"])
        self.assertTrue(closed["close_applied"], closed)
        repeated = self.send(connection, "job.close-stdin", id="close-stdin-again",
                             job_id=job_id, caps=["job:stdin:write"])
        self.assertFalse(repeated["close_applied"], repeated)
        rejected = self.send(connection, "job.write", id="write-after-close", job_id=job_id,
                             data_b64=base64.b64encode(b"late").decode(),
                             caps=["job:stdin:write"])
        self.assertIn("closed", rejected["error"])
        terminal = self.wait_job(connection, job_id, identifier="stdin-wait")
        self.assertEqual(terminal["rc"], 0)

        ref = self.open_stream(connection, job_id, identifier="half-open")
        connection.lane.shutdown(socket.SHUT_WR)
        missing = self.send(connection, "data.import", id="closed-import",
                            transport="sealed-fd", caps=["data:create"])
        self.assertEqual(missing["error"], "attachment_lane_closed")
        response = self.credit(connection, ref, 64, 0, "half-credit")
        self.assertTrue(response["packet_sent"], response)
        self.assertEqual(self.receive_frame(connection.lane)["payload"], b"0\n")

        cancelled_job = self.start_job(
            connection, "import time; time.sleep(30)", stdin_pipe=True,
            identifier="cancelled-stdin-job")
        self.send(connection, "job.cancel", id="cancel-stdin-job",
                  job_id=cancelled_job, grace_ms=10, caps=["job:signal"])
        already_closed = self.send(connection, "job.close-stdin", id="close-after-cancel",
                                   job_id=cancelled_job, caps=["job:stdin:write"])
        self.assertFalse(already_closed["close_applied"], already_closed)

    def test_invalid_ack_is_rejected_and_producer_cancel_marks_incomplete_eof(self):
        connection = self.local()
        job_id = self.start_job(
            connection,
            "import os,time; os.write(1,b'before-cancel'); time.sleep(30)",
            identifier="cancelled-producer",
        )
        deadline = time.monotonic() + 5
        while True:
            observed = self.send(connection, "job.read", id="observe-before-cancel",
                                 job_id=job_id, stream="stdout", offset=0, limit=64,
                                 caps=["job:read"])
            if base64.b64decode(observed["data_b64"]) == b"before-cancel":
                break
            if time.monotonic() >= deadline:
                self.fail(("producer output was not observable", observed))
            time.sleep(.01)
        terminal = self.send(connection, "job.cancel", id="cancel-producer",
                             job_id=job_id, grace_ms=10, caps=["job:signal"])
        self.assertTrue(terminal["terminal"], terminal)
        ref = self.open_stream(connection, job_id, identifier="cancelled-open")
        invalid = self.credit(connection, ref, 16, 1, "invalid-ack")
        self.assertEqual(invalid["error"], "ack_offset outside sent range")
        ack = 0
        saw_data = False
        for request in range(4):
            response = self.credit(connection, ref, 16, ack, f"cancelled-credit-{request}")
            frame = self.receive_frame(connection.lane)
            if frame["flags"] == self.DATA:
                saw_data = True
                self.assertEqual(frame["offset"], 0)
                ack = len(frame["payload"])
                continue
            self.assertEqual(frame["flags"], self.EOF | self.INCOMPLETE | self.CANCELLED)
            self.assertTrue(response["eof"])
            self.assertFalse(response["complete"])
            break
        else:
            self.fail("cancelled producer did not produce terminal frame")
        self.assertTrue(saw_data)


if __name__ == "__main__":
    unittest.main()
