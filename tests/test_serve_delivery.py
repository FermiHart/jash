#!/usr/bin/env python3
"""Protocol delivery must handle short writes and stop before the next effect."""
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
JASH = Path(os.environ.get('JASH', ROOT / 'build/jash')).resolve()
AGENT = Path(os.environ.get('JASH_AGENTD_BIN', ROOT / 'build/jash-agentd')).resolve()

class Delivery(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='jash-serve-faults-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.shim = Path(cls.temp.name) / 'faults.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror', str(ROOT / 'tests/serve_delivery_faults.c'), '-ldl', '-o', str(cls.shim)], check=True)

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='jash-serve-delivery-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.env = dict(PATH=os.defpath, HOME=str(self.root), JASH_SIGIL='0', JASH_NUDGE='0',
            JASH_WORLD='0', JASH_TIMELINE='0', JASH_ADOPT='0', JASH_MEMO='0', LD_PRELOAD=str(self.shim))

    def invoke(self, strict, mode, requests):
        self.env['JASH_TEST_DELIVERY'] = mode
        return subprocess.run([str(AGENT)] if strict else [str(JASH), '--serve'],
            input=('\n'.join(json.dumps(x) for x in requests)+'\n').encode(), cwd=self.root,
            env=self.env, capture_output=True, timeout=20)

    def test_eintr_and_short_writes_preserve_every_protocol_record(self):
        for strict in (False, True):
            args = dict(id='query', argv=['echo', '--json', 'two words', 'Olá ∞'], protocol_version=2, caps=['effect:read']) if strict else dict(id='query', argv=['echo', '--json', 'two words', 'Olá ∞'])
            result = self.invoke(strict, 'short', [args])
            self.assertEqual(result.returncode, 0, result.stderr)
            records = [json.loads(row) for row in result.stdout.splitlines()]
            terminal = [r for r in records if r.get('id') == 'query' and 'rc' in r]
            self.assertEqual(len(terminal), 1, records)
            self.assertEqual(terminal[0]['rc'], 0, terminal)

    def test_failed_handshake_admits_no_command(self):
        for strict in (False, True):
            marker = self.root / ('handshake-' + str(strict))
            request = dict(id='effect', argv=['touch', '--json', str(marker)], protocol_version=2, caps=['effect:mutate'])
            result = self.invoke(strict, 'full', [request])
            self.assertEqual(result.returncode, 125, result.stderr)
            self.assertFalse(marker.exists())

    def test_failed_response_stops_following_effect(self):
        first, second = self.root / 'first', self.root / 'second'
        requests = [dict(id='one', argv=['touch', '--json', str(first)]), dict(id='two', argv=['touch', '--json', str(second)])]
        result = self.invoke(False, 'after_handshake', requests)
        self.assertEqual(result.returncode, 125, result.stderr)
        self.assertTrue(first.exists())
        self.assertFalse(second.exists())

    def test_failed_strict_start_admits_no_effect(self):
        marker = self.root / 'strict-start'
        request = dict(id='effect', argv=['touch', '--json', str(marker)], protocol_version=2, caps=['effect:mutate'])
        result = self.invoke(True, 'after_handshake', [request])
        self.assertEqual(result.returncode, 125, result.stderr)
        self.assertFalse(marker.exists())

if __name__ == '__main__':
    unittest.main()
