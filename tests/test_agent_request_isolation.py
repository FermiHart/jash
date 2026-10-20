#!/usr/bin/env python3
"""Real-runtime request rejection and recovery, sharing RT02 receipt checks."""

import base64
import json

import test_agent_env_isolation as runtime


class RequestIsolation(runtime.RuntimeCase):
    extra_environment = {f"F{i}": "x" for i in range(2000)}

    def output(self, row):
        self.assertEqual(row.get("rc"), 0, row)
        if "stdout_ref" in row:
            return (self.home / ".jash/overflow" / row["stdout_ref"]).read_text()
        return row["stdout"]

    def state_environment(self, **body):
        row = self.request(argv=["env", "--json"], caps=["effect:read"], **body)
        return row, json.loads(self.output(row))

    def remove_fillers(self, observed):
        # Keep the same daemon and every variable under test. Removing only
        # fixture-owned fillers lets a real external observer run below its
        # own environment limit without erasing a leaked request overlay.
        fillers = sorted(key for key in self.environment
                         if key.startswith("F") and key[1:].isdigit())
        for offset in range(0, len(fillers), 62):  # argv limit: command + flag + keys
            self.output(self.request(argv=["unset", "--json", *fillers[offset:offset + 62]],
                                     caps=["effect:mutate"]))
        _, reduced = self.state_environment()
        expected = {key: value for key, value in observed.items() if key not in fillers}
        self.assertEqual(reduced, expected)
        self.assertLess(len(reduced), 256)  # independent job environment limit
        return fillers

    def os_environment(self):
        # The shell-free job path inherits libc environ, independently of the
        # builtin's state table. The normal shell exec path reconstructs envp
        # from that table and therefore cannot prove mirror restoration.
        row = self.request(request_version=2, op="exec", argv=[runtime.PRINTENV],
                           caps=["effect:exec", "job:create"])
        self.assertEqual(row.get("rc"), 0, row)
        output = self.request(request_version=2, op="job.read", job_id=row["job_id"],
                              stream="stdout", offset=0, limit=65536, caps=["job:read"])
        self.assertTrue(output["eof"], output)
        raw = base64.b64decode(output["data_b64"], validate=True)
        self.assertEqual(output["bytes"], len(raw), output)
        values = {}
        for entry in raw.decode().splitlines():
            key, separator, value = entry.partition("=")
            self.assertEqual(separator, "=", entry)
            self.assertNotIn(key, values)
            values[key] = value
        return values

    def test_invalid_sync_env_rejected_before_effect(self):
        for version in (1, 2):
            for key in ("", "=", "RT09_BAD=NAME"):
                with self.subTest(version=version, key=key):
                    # Reestablish pressure for every invalid-key case; the
                    # previous case removes its fillers only for observation.
                    self.stop()
                    self.start()
                    before_row, before = self.state_environment()
                    self.assertGreater(len(before), 2000)
                    marker = self.home / f"invalid-{version}-{len(key)}"
                    row = self.request(
                        request_version=version, argv=[runtime.TOUCH, str(marker)],
                        env={"RT09_BEFORE": "temporary", key: "bad"},
                    )
                    after_row, after = self.state_environment()
                    self.remove_fillers(after)
                    mirrored = self.os_environment()
                    self.rejected(row, marker)
                    self.assertEqual(after, before)
                    self.assertEqual(after_row["cwd"], before_row["cwd"])
                    self.assertNotIn("RT09_BEFORE", after)
                    self.assertNotIn("RT09_BEFORE", mirrored)
                    self.assertEqual(mirrored["R" * 63], "ORIGINAL")

    def test_sync_capacity_failure_is_atomic_and_retryable(self):
        original = "R" * 63
        destination = self.home / "destination"
        destination.mkdir()
        for capacity in (2047, 2048):  # JASH_MAX_ENV, including partial-fit failure.
            for version in (1, 2):
                with self.subTest(capacity=capacity, version=version):
                    _, before = self.state_environment()
                    needed = capacity - len(before)
                    self.assertGreaterEqual(needed, 0)
                    fillers = [f"F{i}" for i in range(capacity) if f"F{i}" not in before]
                    self.environment.update({key: "x" for key in fillers[:needed]})
                    self.stop()
                    self.start()
                    full_row, full = self.state_environment()
                    self.assertEqual(len(full), capacity)

                    marker = self.home / f"capacity-{capacity}-{version}"
                    body = dict(
                        id=f"capacity-{capacity}-{version}", request_version=version,
                        argv=[runtime.TOUCH, str(marker)], cwd=str(destination),
                        env={original: "temporary", "RT09_FIRST": "first",
                             "RT09_SECOND": "second"},
                    )
                    row = self.request(**body)
                    # Capture independently verified observations even on red.
                    after_row, after = self.state_environment()
                    self.rejected(row, marker)
                    self.assertEqual(after, full)
                    self.assertEqual(after[original], "ORIGINAL")
                    self.assertNotIn("RT09_FIRST", after)
                    self.assertNotIn("RT09_SECOND", after)
                    self.assertEqual(after_row["cwd"], full_row["cwd"])
                    self.assertEqual(after_row["cwd"], str(self.home))

                    # Check libc before any successful overlay could overwrite
                    # evidence of a failed request's mirror leak.
                    self.remove_fillers(after)
                    mirrored = self.os_environment()
                    self.assertEqual(mirrored[original], "ORIGINAL")
                    self.assertNotIn("RT09_FIRST", mirrored)
                    self.assertNotIn("RT09_SECOND", mirrored)

                    # Replacing an existing entry must still work at capacity.
                    self.stop()
                    self.start()
                    _, reloaded = self.state_environment()
                    self.assertEqual(reloaded, full)
                    _, replace = self.state_environment(request_version=version,
                                                        env={original: "replacement"})
                    self.assertEqual(replace, {**full, original: "replacement"})
                    _, restored = self.state_environment()
                    self.assertEqual(restored, full)

                    # Observe the OS mirror before a restart could hide a leak.
                    fillers = self.remove_fillers(restored)
                    mirrored = self.os_environment()
                    self.assertEqual(mirrored[original], "ORIGINAL")
                    self.assertNotIn("RT09_FIRST", mirrored)
                    self.assertNotIn("RT09_SECOND", mirrored)

                    # Restart with room, retaining the same durable ID namespace.
                    # The retry executes real touch, so it must also fit the
                    # external exec limit, not merely the 2048-entry state table.
                    for key in fillers:
                        del self.environment[key]
                    self.stop()
                    self.start()
                    retry = self.request(**body)
                    self.assertEqual(retry.get("rc"), 0, retry)
                    self.assertTrue(marker.is_file(), retry)
                    self.assertEqual(self.request(**body), retry)
                    restored = self.request(argv=[runtime.PRINTENV, original], cwd=str(self.home))
                    self.assertEqual(restored.get("stdout"), "ORIGINAL\n", restored)
                    self.absent("RT09_FIRST")
                    self.absent("RT09_SECOND")
                    mirrored = self.os_environment()
                    self.assertEqual(mirrored[original], "ORIGINAL")
                    self.assertNotIn("RT09_FIRST", mirrored)
                    self.assertNotIn("RT09_SECOND", mirrored)


if __name__ == "__main__":
    runtime.run(RequestIsolation)
