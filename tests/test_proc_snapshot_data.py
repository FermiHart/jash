#!/usr/bin/env python3
"""Compare native process snapshots with an independently controlled process.

Requires /proc and getpid() to name the same PID namespace.
"""
import base64
import json
import os
import select
import subprocess
import sys
import unittest
from pathlib import Path
import native_data_fixture as f
from contract_schema_fixture import validate_contract


class ProcessSnapshots(f.NativeQueryCase):
    def test_snapshot_fifo_is_rejected_without_opening_it_as_a_stream(self):
        fifo = self.work / 'snapshot.fifo'
        os.mkfifo(fifo)
        self.report(['proc', '--json', 'diff', str(fifo), str(fifo)], 1)

    def child(self):
        # An exec-error pipe closing does not prove that /proc/cmdline is ready.
        # The child acknowledges userspace startup and stays alive until this
        # fixture terminates it; a slow machine cannot exhaust a sleep lease.
        program = 'import os, signal; os.write(1, b"ready\\n"); signal.pause()'
        child = subprocess.Popen([sys.executable, '-c', program, 'two words', 'quote"\\'],
            env={'PATH': os.defpath, 'SNAPSHOT_VALUE': 'exact space'}, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)

        def cleanup():
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=5)
            child.stdout.close()
            child.stderr.close()

        self.addCleanup(cleanup)
        ready, _, _ = select.select([child.stdout], [], [], 10)
        self.assertTrue(ready, f'child did not acknowledge startup; rc={child.poll()}')
        self.assertEqual(os.read(child.stdout.fileno(), 6), b'ready\n',
                         f'child exited before startup; rc={child.poll()}')
        self.assertIsNone(child.poll(), 'fixture child must be alive before observing /proc')
        expected = [os.fsencode(sys.executable), b'-c', program.encode(), b'two words', b'quote"\\']
        actual = Path(f'/proc/{child.pid}/cmdline').read_bytes().split(b'\0')[:-1]
        self.assertEqual(actual, expected, f'fixture child rc={child.poll()}')
        return child, expected

    def report(self, args, rc=0, strict=False):
        result = self.run_command(args, strict=strict, caps=['effect:read', 'effect:mutate', 'effect:exec'])
        self.assertEqual(result.returncode, rc, result.stderr)
        value = json.loads(result.stdout)
        validate_contract('proc', value)
        return value

    def test_tree_and_tread_observe_exact_argv_and_descriptor_zero(self):
        child, expected = self.child()
        for strict in (False, True):
            report = self.report(['proc', '--json', 'tree', str(child.pid)], strict=strict)
            self.assertEqual([x.encode() for x in report['tree']['argv']], expected)
            self.assertEqual(report['tree']['env'], ['PATH=' + os.defpath, 'SNAPSHOT_VALUE=exact space'])
            self.assertIn(0, [x['fd'] for x in report['tree']['fds']])
            self.assertIsNone(report['tree']['children'])
            self.assertFalse(report['tree']['children_available'])
            for command in [['proc', '--data', 'tree', str(child.pid)], ['tread', '--data', '@proc:' + str(child.pid)]]:
                with self.subTest(command=command, strict=strict):
                    stream = self.native(command, strict=strict)
                    self.assertEqual(stream.state, 0)
                    tree = stream.items[0].value['tree']
                    self.assertEqual([v.value.encode() for v in self.scalar(tree, 'argv', 'list')], expected)
                    self.scalar(tree, 'start_ticks', 'u64', report['tree']['start_ticks'])
                    fd0 = next(v for v in self.scalar(tree, 'fds', 'list') if v.value['fd'].value == 0)
                    self.scalar(fd0, 'target', 'path', b'/dev/null')

    def test_snapshot_persists_actual_versioned_private_document(self):
        child, expected = self.child()
        for strict in (False, True):
            destination = self.work / ('snapshot ' + str(strict) + '.json')
            report = self.report(['proc', '--json', 'snapshot', str(child.pid), '--output', str(destination)], strict=strict)
            self.assertTrue(report['applied'])
            self.assertTrue(report['durable'])
            self.assertEqual(destination.stat().st_mode & 0o777, 0o600)
            document = json.loads(destination.read_bytes())
            self.assertEqual(document['version'], 3)
            self.assertEqual(document['root_pid'], child.pid)
            self.assertEqual([x.encode() for x in document['tree']['argv']], expected)
        # Default directory publication is also an actual effect, in isolated HOME.
        report = self.report(['proc', '--json', 'snapshot', str(child.pid)])
        self.assertTrue(Path(report['snapshot']).is_file())

    def test_diff_compares_bytes_and_preserves_size_units(self):
        left, right = self.work / 'a', self.work / 'b'
        left.write_bytes(b'A\0B\xff'); right.write_bytes(left.read_bytes())
        report = self.report(['proc', '--json', 'diff', str(left), str(right)])
        self.assertTrue(report['identical'])
        right.write_bytes(b'A\0C\xff')
        result = self.native(['proc', '--data', 'diff', str(left), str(right)])
        self.scalar(result.items[0], 'identical', 'bool', False)
        self.scalar(result.items[0], 'size_a', 'filesize', 4)

    def test_restore_executes_exact_argv_and_refuses_unsupported_profile(self):
        path, marker = self.work / 'restore.json', self.work / 'marker'
        argv = ['/bin/sh', '-c', 'printf %s "$1" > "$2"', 'fixture', 'two words " \\', str(marker)]
        path.write_text(json.dumps({'version': 3, 'tree': {'argv': argv}})); path.chmod(0o600)
        report = self.report(['proc', '--json', 'restore', str(path)])
        self.assertTrue(report['applied'])
        self.assertGreater(report['restored_pid'], 0)
        self.assertEqual(marker.read_text(), argv[4])
        marker.unlink()
        report = self.report(['proc', '--json', 'restore', str(path)], 2, strict=True)
        self.assertFalse(marker.exists())
        self.assertIn('background_not_supported', str(report))
        path.write_text(json.dumps({'version': 999, 'tree': {'argv': argv}}))
        report = self.report(['proc', '--json', 'restore', str(path)], 1)
        self.assertFalse(report['applied'])
        self.assertFalse(marker.exists())

    def test_failed_admission_never_publishes_or_spawns(self):
        child, _ = self.child()
        destination, marker = self.work / 'snapshot.json', self.work / 'spawned'
        source = self.work / 'restore.json'
        source.write_text(json.dumps({'version': 3, 'tree': {'argv': ['/usr/bin/touch', str(marker)]}})); source.chmod(0o600)
        for flag in ('--json', '--data', '--data-v2'):
            for args in [['proc', flag, 'snapshot', str(child.pid), '-o', str(destination)], ['proc', flag, 'restore', str(source)]]:
                with open('/dev/full', 'wb') as sink:
                    result = subprocess.run([str(f.JASH), '--exec-json', json.dumps({'request_version': 2, 'argv': args})],
                        env=self.env, cwd=self.work, stdout=sink, stderr=subprocess.PIPE, timeout=10)
                self.assertEqual(result.returncode, 125, result.stderr)
                self.assertFalse(destination.exists())
                self.assertFalse(marker.exists())

    def test_invalid_pid_and_missing_input_are_structured(self):
        for pid in ('0', '-1', '1junk', '999999999999999999999'):
            self.report(['proc', '--json', 'tree', pid], 2)
        result = self.run_command(['proc', '--data', 'diff', 'missing', 'missing'])
        self.assertEqual(result.returncode, 1)
        self.assertNotEqual(f.stream(result.stdout).state, 0)


if __name__ == '__main__':
    unittest.main()
