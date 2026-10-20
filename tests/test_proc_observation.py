#!/usr/bin/env python3
"""Independent controls for the runtime fixtures' Linux child observer."""

import errno
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

import proc_observation as proc


def stat(pid, parent, start=123, comm=b"worker"):
    fields = [b"S", str(parent).encode()] + [b"0"] * 17 + [str(start).encode()] + [b"0"] * 30
    return str(pid).encode() + b" (" + comm + b") " + b" ".join(fields) + b"\n"


class ProcObservation(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="jash-proc-observation-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / "self").mkdir()
        (self.root / "self/stat").write_bytes(stat(os.getpid(), os.getppid()))
        self.put(42, stat(42, 1))
        self.put(101, stat(101, 42, comm=b"name ) with\nspaces"))
        self.put(102, stat(102, 101))
        self.put(103, stat(103, 1))

    def put(self, pid, raw):
        directory = self.root / str(pid)
        directory.mkdir(exist_ok=True)
        (directory / "stat").write_bytes(raw)

    def observe(self, **kwargs):
        return proc.direct_children(42, proc_root=self.root, **kwargs)

    def children_file(self, raw):
        directory = self.root / "42/task/42"
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "children").write_bytes(raw)

    def test_absent_children_falls_back_to_exact_direct_parent(self):
        (self.root / "104").mkdir()  # A process that vanished during enumeration.
        self.assertEqual(self.observe(), [101])

    def test_available_children_checks_parent_and_identity(self):
        self.children_file(b"101 ")
        self.assertEqual(self.observe(), [101])
        self.children_file(b"102 ")
        with self.assertRaises(proc.ProcObservationError):
            self.observe()

    def test_all_threads_uses_ppid_even_when_leader_child_list_is_empty(self):
        self.children_file(b"")
        self.assertEqual(self.observe(), [])
        self.assertEqual(self.observe(all_threads=True), [101])
        observed = proc.read_process_stat(101, proc_root=self.root)
        self.assertEqual((observed.pid, observed.parent, observed.start, observed.state),
                         (101, 42, 123, b"S"))

    def test_incomplete_or_malformed_stat_never_becomes_empty_success(self):
        invalid = [b"", b"101 (x) S 42\n", stat(999, 42), stat(101, -1),
                   stat(101, 1 << 31), stat(101, 42, start=-1),
                   stat(101, 42, start=1 << 64), stat(101, 42)[:-1],
                   stat(101, 42).replace(b" S ", b" ? "), stat(101, 42) + b"\0"]
        for raw in invalid:
            with self.subTest(raw=raw):
                self.put(101, raw)
                with self.assertRaises(proc.ProcObservationError):
                    self.observe()

    def test_namespace_mismatch_and_missing_target_are_failures(self):
        (self.root / "self/stat").write_bytes(stat(os.getpid(), os.getppid() + 1))
        with self.assertRaises(proc.ProcObservationError):
            self.observe()
        (self.root / "self/stat").write_bytes(stat(os.getpid() + 1, os.getppid()))
        with self.assertRaises(proc.ProcObservationError):
            self.observe()
        (self.root / "self/stat").write_bytes(stat(os.getpid(), os.getppid()))
        (self.root / "42/stat").unlink()
        with self.assertRaises(FileNotFoundError):
            self.observe()

    def test_permission_failure_is_not_unavailable_or_vanished(self):
        original = proc._read_bounded
        for denied in ("42/task/42/children", "103/stat"):
            with self.subTest(denied=denied):
                def read(directory, path, limit):
                    if path == denied:
                        raise PermissionError(errno.EACCES, "injected denial", path)
                    return original(directory, path, limit)
                with mock.patch.object(proc, "_read_bounded", side_effect=read):
                    with self.assertRaises(PermissionError):
                        self.observe()

    def test_resource_limits_and_duplicate_children_fail(self):
        for options in ({"max_entries": 1}, {"max_children": 0}):
            with self.subTest(options=options):
                with self.assertRaises(proc.ProcObservationError):
                    self.observe(**options)
        self.children_file(b"101 101 ")
        with self.assertRaises(proc.ProcObservationError):
            self.observe()
        for raw in (b"0 ", b"-1 ", b"101x", b"101\0", b"2147483648 "):
            self.children_file(raw)
            with self.subTest(raw=raw), self.assertRaises(proc.ProcObservationError):
                self.observe()
        self.children_file(b"101 " * 20000)
        with self.assertRaises(proc.ProcObservationError):
            self.observe()

    def test_target_or_child_reuse_is_reported(self):
        original = proc._stat
        for altered in (42, 101):
            calls = 0
            def read(directory, path, expected):
                nonlocal calls
                observed = original(directory, path, expected)
                if expected == altered:
                    calls += 1
                    if calls > 1:
                        return proc.ProcessStat(observed.pid, observed.parent, observed.start + 1)
                return observed
            with self.subTest(altered=altered), mock.patch.object(proc, "_stat", side_effect=read):
                with self.assertRaises(proc.ProcObservationError):
                    self.observe()

    def test_real_child_observation_keeps_process_alive(self):
        child = subprocess.Popen(["/usr/bin/sleep", "30"])
        try:
            self.assertIn(child.pid, proc.direct_children(os.getpid()))
            self.assertIsNone(child.poll())
        finally:
            child.terminate()
            child.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
