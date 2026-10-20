#!/usr/bin/env python3
"""Native coordination results checked against files, channel state and argv."""
import base64
import json
import os
import shlex
import subprocess
import unittest
from pathlib import Path
import native_data_fixture as f
from contract_schema_fixture import validate_contract

def decoded(value):
    return value.encode() if isinstance(value, str) else base64.b64decode(value['data'], validate=True)

class Coordination(f.NativeQueryCase):
    def native_report(self, args, strict=False):
        result = self.run_command(args, strict=strict, caps=['effect:read', 'effect:mutate', 'effect:exec'])
        self.assertEqual(result.returncode, 0, result.stderr)
        stream = f.stream(result.stdout, credit_kind='none')
        self.assertEqual((stream.state, stream.producer), (0, 0))
        return stream

    def report(self, args, rc=0, strict=False):
        result = self.run_command(args, strict=strict, caps=['effect:read', 'effect:mutate', 'effect:exec'])
        self.assertEqual(result.returncode, rc, result.stderr)
        value = json.loads(result.stdout)
        if args[0] in ('gossip', 'weave'):
            validate_contract('gossip-native' if args[0] == 'gossip' else 'weave', value)
        return value

    def test_gossip_channel_persists_exact_messages_and_pages(self):
        messages = [b'two words " \\', b'A\xffB', 'Olá ∞'.encode()]
        for number, message in enumerate(messages, 1):
            report = self.report(['gossip', '--json', '--channel', 'fixture', 'send', message])
            self.assertTrue(report['persisted']); self.assertTrue(report['durable'])
            self.assertEqual(report['sequence'], number)
        journal = Path(self.env['HOME']) / '.jash/gossip-fixture.bin'
        data = journal.read_bytes()
        self.assertEqual(data[:8], b'JASHG001')
        self.assertEqual(int.from_bytes(data[8:16], os.sys.byteorder), 3)
        self.assertEqual(journal.stat().st_mode & 0o777, 0o600)
        for message in messages: self.assertIn(message + b'\0', data)
        for strict in (False, True):
            report = self.report(['gossip', '--json', '--channel', 'fixture', 'receive', '--limit', '2'], strict=strict)
            self.assertEqual([decoded(m['message']) for m in report['messages']], messages[:2])
            self.assertTrue(report['has_more']); self.assertEqual(report['next_cursor'], 2)
            stream = self.native_report(['gossip', '--data', '--channel', 'fixture', 'receive', '--after', '2'], strict=strict)
            row = stream.items[0]
            remaining = self.scalar(row, 'messages', 'list')
            self.assertEqual(len(remaining), 1)
            self.scalar(remaining[0], 'message', 'string', 'Olá ∞')
            self.scalar(row, 'has_more', 'bool', False)

    def test_gossip_read_is_readonly_and_corruption_is_explicit(self):
        self.report(['gossip', '--json', '--channel', 'absent', 'receive'], strict=True)
        self.assertFalse((Path(self.env['HOME']) / '.jash/gossip-absent.bin').exists())
        self.report(['gossip', '--json', '--channel', 'absent', 'receive', '--after', '1'], 2)
        self.report(['gossip', '--json', '--channel', 'bad', 'send', 'original'])
        path = Path(self.env['HOME']) / '.jash/gossip-bad.bin'
        path.write_bytes(b'corrupt')
        report = self.report(['gossip', '--json', '--channel', 'bad', 'receive'], 1)
        self.assertFalse(report['complete'])
        self.assertEqual(report['error'], 'journal_invalid')

    def test_local_gossip_has_process_scope_and_legacy_json_values(self):
        message = 'quote " and newline\n'
        command = shlex.join(['gossip', 'send', message]) + ' >/dev/null; gossip receive --json'
        result = subprocess.run([str(f.JASH), '-c', command], env=self.env, cwd=self.work, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)[0]['message'], message)
        stream = self.native_report(['gossip', '--data', 'receive'])
        self.scalar(stream.items[0], 'scope', 'symbol', 'process-local-ring')
        self.assertEqual(self.scalar(stream.items[0], 'messages', 'list'), [])

    def test_gossip_failed_admission_never_publishes(self):
        for mode in ('--json', '--data', '--data-v2'):
            with open('/dev/full', 'wb') as sink:
                request = {'request_version': 2, 'argv': ['gossip', mode, '--channel', 'not-published', 'send', 'value']}
                result = subprocess.run([str(f.JASH), '--exec-json', json.dumps(request)], env=self.env, cwd=self.work,
                    stdout=sink, stderr=subprocess.PIPE, timeout=10)
            self.assertEqual(result.returncode, 125, result.stderr)
            self.assertFalse((Path(self.env['HOME']) / '.jash/gossip-not-published.bin').exists())

    def test_weave_order_is_declaration_order_and_outputs_are_byte_exact(self):
        tasks = {'z_first': 'printf expected > ordered', 'a_second': 'cat ordered', 'binary': "/usr/bin/printf '\\000\\377'"}
        for strict in (False, True):
            report = self.report(['weave', '--json', json.dumps(tasks)], strict=strict)
            self.assertEqual(report['status'], 'completed')
            self.assertEqual([x['name'] for x in report['results']], list(tasks))
            self.assertEqual(report['results'][1]['out'], 'expected')
            self.assertEqual(decoded(report['results'][2]['out']), b'\0\xff')
            self.assertEqual(report['results'][2]['out_bytes'], 2)
            result = self.run_command(['weave', '--data', json.dumps(tasks)], strict=strict, caps=['effect:exec'])
            self.assertEqual(result.returncode, 0, result.stderr)
            stream = f.stream(result.stdout, credit_kind='none')
            self.assertEqual([r.value['name'].value for r in stream.items], list(tasks))
            self.scalar(stream.items[2], 'out', 'bytes', b'\0\xff')

    def test_weave_prefix_is_bounded_with_actual_observed_count(self):
        report = self.report(['weave', '--json', json.dumps({'large': '/usr/bin/head -c 20000 /dev/zero', 'fail': 'false'})], 1)
        first = report['results'][0]
        self.assertTrue(first['truncated'])
        self.assertEqual(first['out_bytes'], 20000)
        self.assertEqual(first['retained_bytes'], 16384)
        self.assertEqual(len(decoded(first['out'])), 16384)
        self.assertEqual(report['failed'], 1)
        self.assertEqual(report['results'][1]['rc'], 1)

    def test_weave_rejects_invalid_input_before_effects(self):
        for value in ['{"x":"touch forbidden", "x":"true"}', '{"x":42}', '{"x":"' + 'a'*1024 + '"}', '{"x":"touch forbidden"} tail']:
            self.report(['weave', '--json', value], 2)
            self.assertFalse((self.work / 'forbidden').exists())
        for mode in ('--json', '--data'):
            with open('/dev/full', 'wb') as sink:
                request = {'request_version': 2, 'argv': ['weave', mode, '{"x":"touch forbidden"}']}
                result = subprocess.run([str(f.JASH), '--exec-json', json.dumps(request)], env=self.env, cwd=self.work,
                    stdout=sink, stderr=subprocess.PIPE, timeout=10)
            self.assertEqual(result.returncode, 125, result.stderr)
            self.assertFalse((self.work / 'forbidden').exists())

    def test_weave_delivery_failure_stops_before_next_task(self):
        shim = self.work / 'delivery.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror', str(f.ROOT / 'tests/serve_delivery_faults.c'), '-ldl', '-o', str(shim)], check=True)
        self.env.update(LD_PRELOAD=str(shim), JASH_TEST_DELIVERY='after_first_write')
        result = self.run_command(['weave', '--json', '{"first":"touch first", "second":"touch second"}'])
        self.assertEqual(result.returncode, 125, result.stderr)
        self.assertTrue((self.work / 'first').exists())
        self.assertFalse((self.work / 'second').exists())

    def test_mirror_reports_actual_availability_and_verification_failure(self):
        for strict in (False, True):
            report = self.report(['mirror', '--json', '--history'], strict=strict)
            self.assertFalse(report['recording']); self.assertFalse(report['available'])
            stream = self.native_report(['mirror', '--data', '--history'], strict=strict)
            self.scalar(stream.items[0], 'recording', 'bool', False)
            result = self.run_command(['mirror', '--data', '--receipt', 'a'*64 + '.dsse', '--blake3', 'b'*64, '--pubkey', 'c'*64], strict=strict)
            self.assertEqual(result.returncode, 1, result.stderr)
            stream = f.stream(result.stdout, credit_kind='none')
            self.scalar(stream.items[0], 'verified', 'bool', False)
            self.scalar(stream.items[0], 'error', 'symbol', 'receipt_unavailable')

    def test_mirror_invalid_receipt_arguments_preserve_selected_transport(self):
        for strict in (False, True):
            for mode in ('--json', '--data', '--data-v2'):
                for args in (['--receipt', 'bad'], ['--receipt', 'a'*64+'.dsse', '--request', 'one'],
                             ['--receipt', 'a'*64+'.dsse', '--unknown']):
                    result = self.run_command(['mirror', mode, *args], strict=strict)
                    self.assertEqual(result.returncode, 2, result.stderr)
                    if mode == '--json':
                        document = json.loads(result.stdout)
                        validate_contract('error', document)
                        self.assertFalse(document['complete'])
                    else:
                        stream = f.stream(result.stdout, credit_kind='none')
                        self.assertEqual((stream.state, stream.producer), (2, 2))
                        self.assertGreater(stream.errors, 0)

if __name__ == '__main__':
    unittest.main()
