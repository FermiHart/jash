#!/usr/bin/env python3
"""ADR-0056 W16: native immutable-input adapters for external jobs."""
import base64
import socket
import struct
import time

import test_agent_data_plane as data_plane


class ArtifactPipeline(data_plane.Runtime):
    EXEC_CAPS = ["effect:exec", "job:create", "data:read"]

    def import_bytes(self, proc, payload, identifier):
        result = self.send(proc, "data.import", id=identifier, transport="inline",
                           data_b64=base64.b64encode(payload).decode())
        self.assertTrue(result.get("ok"), result)
        return result

    def read_stdout(self, proc, job, identifier):
        result = self.send(proc, "job.read", id=identifier,
                           job_id=job["job_id"], stream="stdout", offset=0,
                           limit=65536, caps=["job:read"])
        self.assertNotIn("error", result, result)
        return base64.b64decode(result["data_b64"])

    def start_source(self, proc, script, identifier):
        result = self.send(proc, "job.start", id=identifier,
                           argv=["/usr/bin/python3", "-c", script],
                           background=True,
                           caps=["effect:exec", "job:create"])
        self.assertTrue(result.get("accepted"), result)
        return result

    def wait_job(self, proc, job, identifier):
        result = self.send(proc, "job.wait", id=identifier,
                           job_id=job["job_id"], wait_ms=5000,
                           caps=["job:read"])
        self.assertTrue(result.get("terminal"), result)
        return result

    def open_stdout(self, proc, job, identifier, offset=0):
        result = self.send(proc, "data.stream.open", id=identifier,
                           job_id=job["job_id"], stream="stdout", offset=offset,
                           caps=["job:read"])
        self.assertTrue(result.get("ok"), result)
        return result

    def test_volatile_binary_input_uses_an_independent_native_reader(self):
        proc = self.start()
        self.assertEqual(self.handshake["schema_version"], 18)
        self.assertTrue(self.handshake["jobs"]["features"]["artifact_stdin"])
        for field in ("stdin_artifact_ref", "stdin_blake3", "stdin_bytes",
                      "stdin_durability", "stdin_delivery",
                      "stdin_serialized_bytes", "stdin_materialized_bytes",
                       "stdin_verified_bytes"):
            self.assertIn(field, self.handshake["response_fields"])
        payload = b"native\x00artifact\xffstdin"
        source = self.import_bytes(proc, payload, "volatile-source")
        command = ["/usr/bin/python3", "-c",
                   "import sys; d=sys.stdin.buffer.read(); sys.stdout.buffer.write(d[::-1])"]

        first = self.send(proc, "exec", id="consume-first", argv=command,
                          artifact_ref=source["artifact_ref"], caps=self.EXEC_CAPS)
        self.assertEqual(first.get("rc"), 0, first)
        self.assertEqual(self.read_stdout(proc, first, "read-first"), payload[::-1])
        self.assertEqual(first["stdin_artifact_ref"], source["artifact_ref"])
        self.assertEqual(first["stdin_blake3"], source["blake3"])
        self.assertEqual(first["stdin_bytes"], len(payload))
        self.assertEqual(first["stdin_durability"], "volatile")
        self.assertEqual(first["stdin_delivery"], "fd")
        self.assertEqual(first["stdin_serialized_bytes"], 0)
        self.assertEqual(first["stdin_materialized_bytes"], 0)

        pipeline = {"version": 1, "pipeline": [
            {"argv": command}, {"argv": ["/bin/cat"]},
        ]}
        second = self.send(proc, "exec", id="consume-second", ir=pipeline,
                           artifact_ref=source["artifact_ref"], caps=self.EXEC_CAPS)
        self.assertEqual(second.get("rc"), 0, second)
        self.assertEqual(self.read_stdout(proc, second, "read-second"), payload[::-1])

    def test_durable_input_survives_runtime_restart_without_attach(self):
        proc = self.start()
        payload = b"durable-pipeline-input"
        source = self.import_bytes(proc, payload, "durable-source")
        durable = self.send(proc, "data.promote", id="durable-publish",
                            artifact_ref=source["artifact_ref"])
        self.assertTrue(durable.get("ok"), durable)
        self.stop(proc)

        proc = self.start()
        result = self.send(proc, "exec", id="durable-consume",
                           argv=["/usr/bin/wc", "-c"],
                           artifact_ref=durable["artifact_ref"], caps=self.EXEC_CAPS)
        self.assertEqual(result.get("rc"), 0, result)
        self.assertEqual(self.read_stdout(proc, result, "read-durable").strip(),
                         str(len(payload)).encode())
        self.assertEqual(result["stdin_durability"], "durable")
        self.assertEqual(result["stdin_verified_bytes"], len(payload))
        self.assertEqual(result["stdin_materialized_bytes"], len(payload))

        path = self.home / ".jash/agent_artifacts" / durable["artifact_ref"][2:]
        delayed = self.send(proc, "job.start", id="durable-delayed",
                            argv=["/usr/bin/python3", "-c",
                                  "import sys,time; time.sleep(.5); "
                                  "sys.stdout.buffer.write(sys.stdin.buffer.read())"],
                            background=True, artifact_ref=durable["artifact_ref"],
                            caps=self.EXEC_CAPS)
        self.assertTrue(delayed.get("accepted"), delayed)
        self.assertEqual(delayed["stdin_materialized_bytes"], len(payload))
        path.write_bytes(b"x" * len(payload))
        terminal = self.send(proc, "job.wait", id="wait-durable-delayed",
                             job_id=delayed["job_id"], wait_ms=5000,
                             caps=["job:read"])
        self.assertTrue(terminal.get("terminal"), terminal)
        self.assertEqual(self.read_stdout(proc, terminal, "read-delayed"), payload)

        marker = self.work / "corrupt-must-not-run"
        corrupt = self.send(proc, "exec", id="corrupt-durable",
                            argv=["/usr/bin/touch", str(marker)],
                            artifact_ref=durable["artifact_ref"], caps=self.EXEC_CAPS)
        self.assertEqual(corrupt.get("error"), "artifact_verification_failed", corrupt)
        self.assertFalse(marker.exists())

    def test_input_authority_conflicts_and_lifetime_fail_closed(self):
        proc = self.start()
        payload = b"held-input"
        source = self.import_bytes(proc, payload, "held-source")
        marker = self.work / "must-not-run"
        command = ["/usr/bin/python3", "-c",
                   f"open({str(marker)!r},'wb').write(b'x')"]

        denied = self.send(proc, "exec", id="missing-read-cap", argv=command,
                           artifact_ref=source["artifact_ref"],
                           caps=["effect:exec", "job:create"])
        self.assertIn("data:read", denied.get("error", ""), denied)
        conflict = self.send(proc, "exec", id="conflicting-input", argv=command,
                             artifact_ref=source["artifact_ref"], stdin_b64="eA==",
                             caps=self.EXEC_CAPS)
        self.assertIn("exclusive", conflict.get("error", ""), conflict)
        redirected = self.send(proc, "exec", id="redirected-input",
                               ir={"version": 1, "pipeline": [{
                                   "argv": command,
                                   "redirs": [{"fd": 0, "kind": "read",
                                               "path": "/dev/null"}],
                               }]}, artifact_ref=source["artifact_ref"],
                               caps=self.EXEC_CAPS)
        self.assertIn("redirection", redirected.get("error", ""), redirected)
        stale = self.send(proc, "exec", id="stale-input", argv=command,
                          artifact_ref="v-" + "0" * 32 + "-" + "0" * 16,
                          caps=self.EXEC_CAPS)
        self.assertEqual(stale.get("error"), "volatile_ref_unavailable", stale)
        self.assertFalse(marker.exists())

        running = self.send(proc, "job.start", id="held-job",
                            argv=["/usr/bin/python3", "-c",
                                  "import sys,time; time.sleep(.5); sys.stdin.buffer.read()"],
                            background=True, artifact_ref=source["artifact_ref"],
                            caps=self.EXEC_CAPS)
        self.assertTrue(running.get("accepted"), running)
        held = self.send(proc, "data.release", id="release-held",
                         artifact_ref=source["artifact_ref"])
        self.assertEqual(held.get("error"), "volatile_ref_in_use", held)
        deadline = time.monotonic() + 10
        terminal = None
        while time.monotonic() < deadline:
            terminal = self.send(proc, "job.wait", id="wait-held",
                                 job_id=running["job_id"], wait_ms=1000,
                                 caps=["job:read"])
            if terminal.get("terminal"):
                break
        self.assertTrue(terminal and terminal.get("terminal"), terminal)
        released = self.send(proc, "data.release", id="release-after-done",
                             artifact_ref=source["artifact_ref"])
        self.assertTrue(released.get("release_applied"), released)

    def test_terminal_stream_snapshot_is_reusable_pipeline_input(self):
        proc = self.local()
        self.assertTrue(self.handshake["jobs"]["features"]["terminal_stream_stdin"])
        for field in ("stdin_stream_ref", "stdin_stream_offset",
                      "stdin_stream_final_ref", "stdin_stream_final_blake3"):
            self.assertIn(field, self.handshake["response_fields"])
        payload = b"stream\x00snapshot\xffpipeline"
        source = self.start_source(
            proc, f"import os; os.write(1, {payload!r})", "stream-source")
        terminal = self.wait_job(proc, source, "wait-stream-source")
        opened = self.open_stdout(proc, source, "open-stream-source")
        caps = ["effect:exec", "job:create", "job:read"]
        command = ["/usr/bin/python3", "-c",
                   "import sys; d=sys.stdin.buffer.read(); sys.stdout.buffer.write(d[::-1])"]

        first = self.send(proc, "exec", id="consume-stream-first", argv=command,
                          stream_ref=opened["stream_ref"], caps=caps)
        self.assertEqual(first.get("rc"), 0, first)
        self.assertEqual(self.read_stdout(proc, first, "read-stream-first"),
                         payload[::-1])
        self.assertEqual(first["stdin_stream_ref"], opened["stream_ref"])
        self.assertEqual(first["stdin_stream_offset"], 0)
        self.assertEqual(first["stdin_stream_final_ref"], terminal["stdout_ref"])
        self.assertEqual(first["stdin_stream_final_blake3"],
                         terminal["stdout_blake3"])
        self.assertEqual(first["stdin_blake3"], terminal["stdout_blake3"])
        self.assertEqual(first["stdin_bytes"], len(payload))
        self.assertEqual(first["stdin_durability"], "sealed-snapshot")
        self.assertEqual(first["stdin_materialized_bytes"], len(payload))
        self.assertEqual(first["stdin_verified_bytes"], len(payload))

        second = self.send(proc, "exec", id="consume-stream-second",
                           argv=["/bin/cat"], stream_ref=opened["stream_ref"],
                           caps=caps)
        self.assertEqual(second.get("rc"), 0, second)
        self.assertEqual(self.read_stdout(proc, second, "read-stream-second"),
                         payload)

        offset = 7
        suffix = self.open_stdout(proc, source, "open-stream-suffix", offset)
        seekable = [
            "/usr/bin/python3", "-c",
            "import os; direct=os.pread(0,1024,0); os.lseek(0,0,0); "
            "os.write(1,direct+b'|'+os.read(0,1024))",
        ]
        third = self.send(proc, "exec", id="consume-stream-suffix",
                           argv=seekable, stream_ref=suffix["stream_ref"],
                           caps=caps)
        self.assertEqual(third.get("rc"), 0, third)
        self.assertEqual(self.read_stdout(proc, third, "read-stream-suffix"),
                          payload[offset:] + b"|" + payload[offset:])
        self.assertEqual(third["stdin_stream_offset"], offset)
        self.assertEqual(third["stdin_bytes"], len(payload) - offset)
        self.assertEqual(third["stdin_materialized_bytes"], len(payload) - offset)
        self.assertEqual(third["stdin_verified_bytes"], len(payload))

    def test_live_stream_and_missing_authority_fail_before_target_effect(self):
        proc = self.local()
        source = self.start_source(
            proc, "import time,os; time.sleep(.5); os.write(1,b'late')",
            "live-stream-source")
        opened = self.open_stdout(proc, source, "open-live-stream")
        marker = self.work / "live-stream-must-not-run"
        command = ["/usr/bin/touch", str(marker)]

        denied = self.send(proc, "exec", id="stream-missing-read", argv=command,
                           stream_ref=opened["stream_ref"],
                           caps=["effect:exec", "job:create"])
        self.assertIn("job:read", denied.get("error", ""), denied)
        live = self.send(proc, "exec", id="stream-live", argv=command,
                         stream_ref=opened["stream_ref"],
                         caps=["effect:exec", "job:create", "job:read"])
        self.assertEqual(live.get("error"), "stream_not_terminal", live)
        self.assertFalse(marker.exists())
        self.wait_job(proc, source, "wait-live-stream")

        terminal = self.start_source(proc, "import os; os.write(1,b'pending')",
                                     "in-flight-source")
        self.wait_job(proc, terminal, "wait-in-flight-source")
        pending = self.open_stdout(proc, terminal, "open-in-flight-source")
        credit = self.send(proc, "data.stream.credit", id="credit-in-flight",
                           stream_ref=pending["stream_ref"], credit_bytes=7,
                           ack_offset=0, caps=["job:read"])
        self.assertTrue(credit.get("packet_sent"), credit)
        packet, ancillary, flags, _ = proc.lane.recvmsg(96 + 7,
                                                        socket.CMSG_SPACE(4))
        self.assertFalse(flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC))
        self.assertEqual(ancillary, [])
        self.assertEqual(packet[:4], b"JDS1")
        self.assertEqual(struct.unpack(">Q", packet[24:32])[0], 7)
        blocked = self.send(proc, "exec", id="stream-in-flight", argv=command,
                            stream_ref=pending["stream_ref"],
                            caps=["effect:exec", "job:create", "job:read"])
        self.assertEqual(blocked.get("error"), "stream_has_in_flight_bytes", blocked)
        self.assertFalse(marker.exists())

    def test_incomplete_conflicting_and_oversized_streams_fail_closed(self):
        proc = self.local()
        marker = self.work / "invalid-stream-must-not-run"
        command = ["/usr/bin/touch", str(marker)]
        caps = ["effect:exec", "job:create", "job:read"]

        cancelled = self.start_source(
            proc, "import os,time; os.write(1,b'partial'); time.sleep(30)",
            "cancelled-stream-source")
        deadline = time.monotonic() + 5
        while True:
            observed = self.send(proc, "job.read", id="observe-partial",
                                 job_id=cancelled["job_id"], stream="stdout",
                                 offset=0, limit=64, caps=["job:read"])
            if base64.b64decode(observed["data_b64"]) == b"partial":
                break
            if time.monotonic() >= deadline:
                self.fail(("source output was not observable", observed))
            time.sleep(.01)
        stopped = self.send(proc, "job.cancel", id="cancel-stream-source",
                            job_id=cancelled["job_id"], grace_ms=10,
                            caps=["job:signal"])
        self.assertTrue(stopped.get("terminal"), stopped)
        incomplete = self.open_stdout(proc, cancelled, "open-cancelled-stream")
        rejected = self.send(proc, "exec", id="consume-incomplete", argv=command,
                             stream_ref=incomplete["stream_ref"], caps=caps)
        self.assertEqual(rejected.get("error"), "stream_incomplete", rejected)

        forced = self.start_source(
            proc,
            "import os,time; p=os.fork(); "
            "p or (os.setsid(),time.sleep(30),os._exit(0)); "
            "os.write(1,b'forced')",
            "forced-descendant-source")
        self.wait_job(proc, forced, "wait-forced-descendant")
        forced_stream = self.open_stdout(proc, forced, "open-forced-descendant")
        forced_rejected = self.send(
            proc, "exec", id="consume-forced-descendant", argv=command,
            stream_ref=forced_stream["stream_ref"], caps=caps)
        self.assertEqual(forced_rejected.get("error"), "stream_incomplete",
                         forced_rejected)

        complete = self.start_source(proc, "import os; os.write(1,b'12345')",
                                     "bounded-stream-source")
        self.wait_job(proc, complete, "wait-bounded-stream")
        opened = self.open_stdout(proc, complete, "open-bounded-stream")
        artifact = self.import_bytes(proc, b"x", "conflicting-artifact")
        conflict = self.send(proc, "exec", id="conflicting-stream-input",
                             argv=command, stream_ref=opened["stream_ref"],
                             artifact_ref=artifact["artifact_ref"], caps=caps)
        self.assertIn("exclusive", conflict.get("error", ""), conflict)

        policy = self.send(proc, "data.policy.get", id="stream-policy")
        changed = self.send(proc, "data.policy.set", id="bound-stream-policy",
                            revision=policy["revision"], max_bytes=8,
                            fd_enabled=True)
        self.assertTrue(changed.get("ok"), changed)
        held = self.send(
            proc, "job.start", id="hold-stream-snapshot",
            argv=["/usr/bin/python3", "-c",
                  "import sys,time; sys.stdin.buffer.read(); time.sleep(2)"],
            background=True, stream_ref=opened["stream_ref"], caps=caps)
        self.assertTrue(held.get("accepted"), held)
        budget = self.send(proc, "budget.get", id="held-snapshot-budget")
        self.assertEqual(budget["stdin_snapshot_bytes"], 5, budget)
        held_policy = self.send(proc, "data.policy.get", id="held-policy")
        lower_held = self.send(
            proc, "data.policy.set", id="lower-held-policy",
            revision=held_policy["revision"], max_bytes=4, fd_enabled=True)
        self.assertEqual(lower_held.get("error"), "policy_below_usage", lower_held)
        quota = self.send(proc, "exec", id="stream-snapshot-quota", argv=command,
                          stream_ref=opened["stream_ref"], caps=caps)
        self.assertEqual(quota.get("error"), "stream_input_too_large", quota)
        self.wait_job(proc, held, "wait-held-stream-snapshot")

        policy = self.send(proc, "data.policy.get", id="stream-policy-after-hold")
        changed = self.send(proc, "data.policy.set", id="limit-stream-policy",
                            revision=policy["revision"], max_bytes=4,
                            fd_enabled=True)
        self.assertTrue(changed.get("ok"), changed)
        oversized = self.send(proc, "exec", id="consume-oversized", argv=command,
                              stream_ref=opened["stream_ref"], caps=caps)
        self.assertEqual(oversized.get("error"), "stream_input_too_large", oversized)
        self.assertFalse(marker.exists())


if __name__ == "__main__":
    data_plane.unittest.main()
