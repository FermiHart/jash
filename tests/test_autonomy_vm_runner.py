#!/usr/bin/env python3
"""Fail-closed VM transport and registry tests; never a substitute for the gate."""
import base64
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import time
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('autonomy_vm', ROOT / 'tools/run_autonomy_vm.py')
VM = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VM)


class TransportTests(unittest.TestCase):
    def setUp(self):
        self.expected = dict(revision='a' * 40, source_sha256='b' * 64)
        self.nonce = '0123456789abcdef' * 2
        self.log = b'JASH_VM_ORIGINAL_GATE_BEGIN\nRan 20 tests\nOK\nJASH_VM_ORIGINAL_GATE_END\n'
        self.result = dict(version=1, **self.expected, exit=0, log_bytes=len(self.log),
                           log_sha256=hashlib.sha256(self.log).hexdigest())

    def serial(self, *, result=None, log=None, nonce=None):
        nonce = nonce or self.nonce
        return (b'ordinary boot noise\n' + f'JASH_VM_LOG_{nonce} '.encode() +
                base64.b64encode(self.log if log is None else log) + b'\r\n' +
                f'JASH_VM_RESULT_{nonce} '.encode() +
                json.dumps(self.result if result is None else result).encode() + b'\r\n')

    def validate(self, serial=None, rc=0):
        return VM.validate_result(self.serial() if serial is None else serial,
                                  self.nonce, self.expected, rc)

    def test_complete_matching_zero_result(self):
        result, log = self.validate()
        self.assertEqual(log, self.log)
        VM.qualify(result, log)

    def test_normal_shutdown_alone_is_not_success(self):
        for serial in (b'', b'Power down\n', self.serial().splitlines()[1],
                       self.serial().splitlines()[-1]):
            with self.subTest(serial=serial), self.assertRaises(ValueError):
                self.validate(serial)

    def test_qemu_failure_signal_and_timeout_fail(self):
        for status in (1, -9, -15, 77, 124):
            with self.subTest(status=status), self.assertRaises(ValueError):
                self.validate(rc=status)

    def test_duplicate_wrong_revision_and_wrong_run_fail(self):
        for serial in (self.serial() * 2,
                       self.serial(result=dict(self.result, revision='c' * 40)),
                       self.serial(result=dict(self.result, source_sha256='d' * 64)),
                       self.serial(nonce='other-run')):
            with self.subTest(serial=serial), self.assertRaises(ValueError):
                self.validate(serial)

    def test_lost_changed_truncated_and_oversized_logs_fail(self):
        for serial in (self.serial(log=self.log[:-1]),
                       self.serial(log=self.log.replace(b'OK', b'NO')),
                       self.serial(result=dict(self.result, log_bytes=0)),
                       self.serial(result=dict(self.result, log_bytes=VM.MAX_LOG_BYTES + 1)),
                       self.serial(result=dict(self.result, log_sha256='0' * 64)),
                       self.serial().replace(b'JASH_VM_LOG_', b'lost_log_')):
            with self.subTest(serial=serial), self.assertRaises(ValueError):
                self.validate(serial)

    def test_corrupt_base64_fails(self):
        with self.assertRaises(ValueError):
            self.validate(self.serial(log=b'bad').replace(b'YmFk', b'!!!!'))

    def test_nonzero_guest_and_exit77_never_qualify(self):
        for status in (1, 77, 124, -9):
            with self.subTest(status=status), self.assertRaises(ValueError):
                VM.qualify(dict(self.result, exit=status), self.log)

    def test_missing_and_duplicate_original_gate_fail(self):
        for log in (b'OK\n', self.log * 2, self.log.replace(b'JASH_VM_ORIGINAL_GATE_END', b'lost')):
            with self.subTest(log=log), self.assertRaises(ValueError):
                VM.qualify(self.result, log)

    def test_any_skipped_qualification_fails(self):
        for text in (b'OK (skipped=1)', b'SKIP: backend unavailable', b'8 skipped'):
            with self.subTest(text=text), self.assertRaises(ValueError):
                VM.qualify(self.result, self.log + text)

    def test_dirty_revision_rejected_before_archiving(self):
        with mock.patch.object(VM, 'git', side_effect=['a' * 40, ' M test.sh']), \
                mock.patch.dict(os.environ, {}, clear=True), mock.patch.object(VM, 'run') as command:
            with self.assertRaisesRegex(ValueError, 'clean tracked checkout'):
                VM.source_archive(Path('/unused'))
            command.assert_not_called()

    def test_failed_download_removes_only_its_own_partial(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / 'package.deb'
            partial = target.with_suffix('.deb.partial')
            with mock.patch.object(VM.urllib.request, 'urlopen', side_effect=OSError('offline')):
                with self.assertRaisesRegex(OSError, 'offline'):
                    VM.fetch('https://deb.debian.org/example', target, 'a' * 64)
            self.assertFalse(partial.exists())
            partial.write_bytes(b'previous attempt')
            with self.assertRaises(FileExistsError):
                VM.fetch('https://deb.debian.org/example', target, 'a' * 64)
            self.assertEqual(partial.read_bytes(), b'previous attempt')

    def test_wrong_github_revision_rejected_before_archiving(self):
        with mock.patch.object(VM, 'git', return_value='a' * 40), \
                mock.patch.dict(os.environ, {'GITHUB_SHA': 'b' * 40}), \
                mock.patch.object(VM, 'run') as command:
            with self.assertRaisesRegex(ValueError, 'GITHUB_SHA'):
                VM.source_archive(Path('/unused'))
            command.assert_not_called()


class RegistryTests(unittest.TestCase):
    def test_emulated_build_budget_remains_explicit_and_bounded(self):
        self.assertEqual(VM.VM_SECONDS, 3600)
        workflow = (ROOT / '.github/workflows/ci.yml').read_text()
        self.assertIn('run: bash tests/test_suite.sh --timeout 4200 ', workflow)
        self.assertIn('timeout-minutes: 120', workflow)

    def listing(self, backend):
        env = dict(os.environ)
        env.pop('BASH_ENV', None)
        env.pop('ENV', None)
        if backend is None:
            env.pop('JASH_AUTONOMY_BACKEND', None)
        else:
            env['JASH_AUTONOMY_BACKEND'] = backend
        return subprocess.run(['bash', str(ROOT / 'tests/test_suite.sh'), '--list'],
                              env=env, capture_output=True, timeout=10)

    def test_native_default_exact_original_denominator_and_argv(self):
        native = self.listing('native')
        default = self.listing(None)
        self.assertEqual(native.returncode, 0, native.stderr)
        self.assertEqual(default.stdout, native.stdout)
        gates = [json.loads(line) for line in native.stdout.splitlines()]
        self.assertEqual(len(gates), 351)
        self.assertEqual(hashlib.sha256(json.dumps(gates, sort_keys=True,
                         separators=(',', ':'), ensure_ascii=True).encode()).hexdigest(),
                         'e1cc98c242990826c161516014abca19ca0331503e3816fd506b15e31be6bf7f')

    def test_qemu_changes_only_one_argv_not_names_order_or_denominator(self):
        native = [json.loads(line) for line in self.listing('native').stdout.splitlines()]
        vm = self.listing('qemu-tcg')
        self.assertEqual(vm.returncode, 0, vm.stderr)
        routed = [json.loads(line) for line in vm.stdout.splitlines()]
        self.assertEqual(len(routed), 351)
        changed = [(a, b) for a, b in zip(native, routed) if a != b]
        self.assertEqual(len(changed), 1)
        a, b = changed[0]
        self.assertEqual(a['name'], 'experimental verifiable autonomy (Linux integration)')
        self.assertEqual(a['argv'], ['bash', 'tests/test_autonomy.sh'])
        self.assertEqual(b, dict(a, argv=['python3', '-B', 'tools/run_autonomy_vm.py']))

    def test_unknown_backend_rejected_without_exporting_any_gates(self):
        result = self.listing('silently-skip-isolation')
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, b'')


class ObserverTests(unittest.TestCase):
    def test_only_allowlisted_stage_hints_are_printed_and_status_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            serial = Path(directory) / 'serial.log'
            output = io.StringIO()
            script = "print('JASH_VM_STAGE=modules'); print('JASH_VM_STAGE=forged-pass'); raise SystemExit(7)"
            with contextlib.redirect_stdout(output):
                status = VM.run_qemu([sys.executable, '-u', '-c', script], serial, 5)
            self.assertEqual(status, 7)
            self.assertIn('building native guest modules', output.getvalue())
            self.assertNotIn('forged-pass', output.getvalue())
            self.assertIn(b'forged-pass', serial.read_bytes())

    def test_deadline_kills_and_reaps_owned_emulator(self):
        with tempfile.TemporaryDirectory() as directory:
            serial = Path(directory) / 'serial.log'
            script = "import os,time; print(os.getpid(),flush=True); time.sleep(30)"
            started = time.monotonic()
            with self.assertRaises(subprocess.TimeoutExpired):
                VM.run_qemu([sys.executable, '-u', '-c', script], serial, 0.2)
            self.assertLess(time.monotonic() - started, 5)
            pid = int(serial.read_text().strip())
            with self.assertRaises(ProcessLookupError):
                os.kill(pid, 0)
            with self.assertRaises(ChildProcessError):
                os.waitpid(pid, os.WNOHANG)


if __name__ == '__main__':
    unittest.main()
