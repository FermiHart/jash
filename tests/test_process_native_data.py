#!/usr/bin/env python3
"""Compare native process observations with an independent controlled /proc PID."""
import json
import os
import subprocess
import unittest
from pathlib import Path
import native_data_fixture as f
from contract_schema_fixture import projection, validate_contract


class ProcessData(f.NativeQueryCase):
    def setUp(self):
        super().setUp()
        self.child = subprocess.Popen(['/bin/sleep', '90'])
        self.addCleanup(self.stop_child)

    def stop_child(self):
        self.child.terminate()
        self.child.wait(timeout=5)

    def observed(self):
        text = Path(f'/proc/{self.child.pid}/stat').read_text()
        fields = text[text.rfind(')') + 2:].split()
        return {'pid': self.child.pid, 'ppid': os.getpid(), 'pgrp': int(fields[2]),
                'comm': 'sleep', 'state': fields[0], 'uid': os.getuid(),
                'start_ticks': int(fields[19]), 'threads': int(fields[17]),
                'rss_kb': int(fields[21]) * os.sysconf('SC_PAGESIZE') // 1024,
                'vsz_kb': int(fields[20]) // 1024}

    def test_values_match_independent_proc_in_both_profiles(self):
        for strict in (False, True):
            for selector in ('--json', '--data', '--data-v2'):
                with self.subTest(strict=strict, selector=selector):
                    result = self.run_command(['ps', selector, '-p', str(self.child.pid)], strict=strict)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    expected = self.observed()
                    if selector == '--json':
                        rows = [json.loads(x) for x in result.stdout.splitlines()]
                        self.assertEqual(len(rows), 1)
                        validate_contract('ps-row', rows[0])
                        for key, value in expected.items():
                            self.assertEqual(rows[0][key], value, key)
                        self.assertTrue(rows[0]['complete'])
                    else:
                        output = f.stream(result.stdout)
                        self.assertEqual((output.count, output.state, output.producer), (1, 0, 0))
                        row = output.items[0]
                        validate_contract('ps-row', projection(row))
                        for key, value in expected.items():
                            kind = 'string' if isinstance(value, str) else 'i64' if key == 'pgrp' else 'u64'
                            self.scalar(row, key, kind, value)
                        self.scalar(row, 'rss_bytes', 'filesize', expected['rss_kb'] * 1024)
                        self.scalar(row, 'vsz_rounded_bytes', 'filesize', expected['vsz_kb'] * 1024)
                        self.scalar(row, 'ticks_per_second', 'u64', os.sysconf('SC_CLK_TCK'))

    def test_paging_top_zero_empty_and_order(self):
        output = self.native(['ps', '--data', '-e', '--sort', 'pid'])
        ids = [r.value['pid'].value for r in output.items]
        self.assertEqual(ids, sorted(ids))
        for args in (['--top', '0'], ['--limit', '0']):
            output = self.native(['ps', '--data', '-p', str(self.child.pid)] + args)
            self.assertEqual((output.count, output.state), (0, 3))
            self.assertEqual(output.reason, 'page_limit')
        output = self.native(['ps', '--data', '-p', str(self.child.pid), '--offset', '1'])
        self.assertEqual((output.count, output.state), (0, 0))
        output = self.native(['ps', '--data', '-p', str(self.child.pid), '--pattern', 'NO SUCH COMMAND'])
        self.assertEqual((output.count, output.state), (0, 0))

    def test_failed_write_and_budget(self):
        with open('/dev/full', 'wb') as sink:
            request = {'request_version': 2, 'argv': ['ps', '--data', '-p', str(self.child.pid)], 'cwd': str(self.work)}
            result = subprocess.run([str(f.JASH), '--exec-json', json.dumps(request)], env=self.env,
                                    stdout=sink, stderr=subprocess.PIPE, timeout=10)
            self.assertEqual(result.returncode, 125, result.stderr)
        self.env['JASH_LLM_BUDGET'] = '1'
        result = self.run_command(['ps', '--json', '-p', str(self.child.pid)])
        self.assertEqual(result.returncode, 75, result.stderr)
        self.assertEqual(json.loads(result.stdout)['error']['code'], 'output_budget_exceeded')

    def test_invalid_options_are_failed_native_streams(self):
        for args in (['-p', '0'], ['--pid', '12garbage'], ['--pid', '12,'], ['--sort', 'bogus'], ['--limit', '-1']):
            with self.subTest(args=args):
                result = self.run_command(['ps', '--data'] + args)
                self.assertEqual(result.returncode, 2, result.stderr)
                output = f.stream(result.stdout, credit_kind='none')
                self.assertEqual((output.state, output.producer, output.count), (2, 2, 0))


if __name__ == '__main__':
    unittest.main()
