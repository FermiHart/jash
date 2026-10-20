#!/usr/bin/env python3
"""History providers describe actual records and the limits of their provenance."""
import json
import os
from pathlib import Path
import shlex
import time
import unittest

import test_management_builtins as management
from test_discovery_builtins import native_stream, project
from jsonschema import Draft202012Validator


class HistoryNative(unittest.TestCase):
    run_cli = management.ManagementBuiltins.run_cli
    execute = management.ManagementBuiltins.execute

    def setUp(self):
        management.ManagementBuiltins.setUp(self)
        (self.directory / 'tmp').mkdir(mode=0o700)
        (self.directory / '.jash').mkdir(mode=0o700)
        self.env['TMPDIR'] = str(self.directory / 'tmp')
        self.env['JASH_TIMELINE'] = '1'

    def validate(self, command, value):
        path = management.ROOT / 'schemas' / ('builtin-' + command + '-v1.schema.json')
        schema = json.loads(path.read_text())
        Draft202012Validator.check_schema(schema)
        Draft202012Validator(schema).validate(value)
        return value

    def report(self, command, *arguments, status=0):
        value = json.loads(self.execute([command, '--json', *arguments], status=status).stdout)
        self.assertEqual(value['schema'], 'jash.' + command + '.v1')
        return self.validate(command, value)

    def native(self, command, *arguments, status=0):
        rows, terminal = native_stream(self.execute([command, '--data-v2', *arguments], status=status).stdout)
        self.assertEqual(len(rows), 1)
        self.assertEqual(terminal['status'], status)
        return self.validate(command, project(rows[0])), terminal, rows[0]

    def chain(self, entries):
        data = bytearray()
        for sequence, timestamp, status, command, tag in entries:
            prefix = f'{sequence} {timestamp}.123456789 ' + '0' * 64 + f' tag={tag} rc={status} cwd=/recorded_dir repo=fixture dur=19 sess=synthetic cmd='
            data.extend(prefix.encode() + command + b'\n')
        (self.directory / '.jash/chain.log').write_bytes(data)

    def test_journal_exact_recorded_command_and_native_integer(self):
        command = b'printf "quoted"; ' + b'x' * 5000 + b'\xff'
        sequence = 9007199254740993
        self.chain([(sequence, 1700000000, 7, command, 'err:7')])
        value, terminal, raw = self.native('journal')
        self.assertEqual(value['source']['records'], 1)
        self.assertTrue(value['source']['read_complete'])
        self.assertFalse(value['source']['cryptographically_verified'])
        self.assertEqual(value['entries'][0]['sequence'], sequence)
        self.assertEqual(value['entries'][0]['status'], 7)
        import base64
        self.assertEqual(value['entries'][0]['command_recorded'], {'encoding': 'base64', 'data': base64.b64encode(command).decode()})
        self.assertEqual(value['entries'][0]['cwd_recorded'], '/recorded_dir')
        self.assertEqual(terminal['state'], 0)
        self.assertEqual(raw[1]['entries'][1][0][1]['sequence'][0], 3)

    def test_journal_pages_filters_and_tag_sidecar(self):
        now = int(time.time())
        self.chain([(1, now - 10, 0, b'build target a', 'build'),
                    (2, now - 5, 1, b'build target b', 'err:1'),
                    (3, now, 0, b'other', 'start')])
        first = self.report('journal', 'build target', '--limit', '1')
        self.assertEqual(first['matched'], 2)
        self.assertEqual(first['entries'][0]['sequence'], 2)
        self.assertEqual(first['next_offset'], 1)
        second = self.report('journal', 'build target', '--limit', '1', '--offset', '1')
        self.assertEqual(second['entries'][0]['sequence'], 1)
        self.assertIsNone(second['next_offset'])
        failed = self.report('journal', '--failed')
        self.assertEqual([entry['sequence'] for entry in failed['entries']], [2])
        sidecar = self.directory / '.jash/journal.tags'
        sidecar.write_text(f'{now}.000000001 err:1 rc=1 cmd=sidecar exact\n')
        tagged = self.report('journal', '--tag', 'err:1', '--since', '1h')
        self.assertEqual(tagged['source_scope'], 'global_tag_sidecar')
        self.assertIsNone(tagged['entries'][0]['sequence'])
        self.assertEqual(tagged['entries'][0]['command_recorded'], 'sidecar exact')

    def test_journal_malformed_source_cannot_be_empty_success(self):
        self.chain([(1, 1700000000, 0, b'valid', '-')])
        with (self.directory / '.jash/chain.log').open('ab') as file:
            file.write(b'2 malformed record\n')
        value, terminal, _ = self.native('journal', status=1)
        self.assertFalse(value['source']['read_complete'])
        self.assertEqual(value['source']['error'], 'invalid_store')
        self.assertEqual(value['entries'], [])
        self.assertIsNone(value['matched'])
        self.assertEqual(terminal['state'], 2)

    def test_zero_based_ledger_sequence_is_valid_but_negative_is_not(self):
        self.chain([(0, 1700000000, 0, b'initial ledger record', '-')])
        value, terminal, raw = self.native('journal')
        self.assertEqual(value['entries'][0]['sequence'], 0)
        self.assertEqual(raw[1]['entries'][1][0][1]['sequence'], (3, 0))
        self.assertEqual(terminal['state'], 0)
        self.chain([(-1, 1700000000, 0, b'invalid negative sequence', '-')])
        value, terminal, _ = self.native('journal', status=1)
        self.assertFalse(value['source']['read_complete'])
        self.assertEqual(value['entries'], [])
        self.assertEqual(terminal['state'], 2)

    def test_journal_invalid_arguments_are_explicit(self):
        for arguments in [('--last', '-1'), ('--since', '1hour'), ('--bogus',), ('--what-happened', '--failed')]:
            result = self.execute(['journal', '--json', *arguments], status=2)
            value = json.loads(result.stdout)
            self.assertEqual(value['error'], 'invalid_arguments')
        empty = self.report('journal')
        self.assertFalse(empty['source']['present'])
        self.assertTrue(empty['source']['read_complete'])
        self.assertEqual(empty['entries'], [])

    def test_journal_transitions_are_observed_process_tags(self):
        # The -c dispatcher records the completed command after evaluation;
        # a query inside the same command cannot invent those later tags.
        one_command = self.run_cli('-c', 'false; journal --json --what-happened')
        self.assertEqual(json.loads(one_command.stdout)['transitions'], [])
        # The input loop records each completed line.  Two actual failures
        # create the error/retry observations even when Sigil is disabled.
        result = self.run_cli(input=b'false\nfalse\njournal --json --what-happened\n')
        value = self.validate('journal', json.loads(result.stdout))
        self.assertEqual(value['source_scope'], 'process_tag_history')
        self.assertIsNone(value['source'])
        self.assertEqual(value['entries'], [])
        self.assertTrue(any(tag['kind'] == 'error' for tag in value['transitions']), value)
        sidecar = (self.directory / '.jash/journal.tags').read_bytes()
        self.assertIn(b'rc=1 cmd=false\n', sidecar)

    def test_why_exact_key_and_recorded_status(self):
        def legacy_aliases(value):
            for legacy, current in [('cmd', 'command_recorded'), ('rc', 'status'),
                                    ('msg', 'message_recorded'),
                                    ('mutations_this_session', 'mutations_recorded')]:
                self.assertEqual(value[legacy], value[current])
            self.assertEqual(value['last_ok_ts'], value['success_fingerprint_timestamp'] or 0)
            self.assertEqual(value['reason'], None if value['known'] else 'no last command recorded')
            self.assertEqual(value['verdict'], 'ok' if value['known'] and value['status'] == 0 else None)

        ledger = self.directory / '.jash/drift.tsv'
        ledger.write_text('0123456789abcdef\t999\thash\ttool\t0\t0\t?\tenv\tfalse\n')
        value = json.loads(self.run_cli('-c', 'false; why --json').stdout)
        self.validate('why', value)
        self.assertTrue(value['known'])
        self.assertEqual(value['status'], 1)
        self.assertEqual(value['command_recorded'], 'false')
        self.assertIsNone(value['success_fingerprint_timestamp'])
        self.assertFalse(value['causation_established'])
        legacy_aliases(value)
        h = 1469598103934665603
        for byte in b'false\1' + os.fsencode(self.directory):
            h = ((h ^ byte) * 1099511628211) & ((1 << 64) - 1)
        with ledger.open('a') as file:
            file.write(f'{h:016x}\t123\thash\ttool\t0\t0\t?\tenv\tfalse\n')
        rows, terminal = native_stream(self.run_cli('-c', 'false; why --data').stdout)
        value = self.validate('why', project(rows[0]))
        self.assertEqual(value['success_fingerprint_timestamp'], 123)
        self.assertEqual(value['lookup_cwd'], str(self.directory))
        self.assertEqual(terminal['state'], 0)
        legacy_aliases(value)
        for selector in ('--json', '--data-v2'):
            process = self.run_cli('-c', 'why ' + selector)
            value = (json.loads(process.stdout) if selector == '--json' else
                     project(native_stream(process.stdout)[0][0]))
            self.validate('why', value)
            self.assertFalse(value['known'])
            self.assertIsNone(value['rc'])
            self.assertIsNone(value['cmd'])
            legacy_aliases(value)
        value = self.validate('why', json.loads(self.run_cli('-c', 'true; why --json').stdout))
        self.assertEqual(value['verdict'], 'ok')
        legacy_aliases(value)
        ledger.write_text('invalid drift source\n')
        for selector in ('--json', '--data-v2'):
            process = self.run_cli('-c', 'false; why ' + selector, status=1)
            if selector == '--json':
                value = json.loads(process.stdout)
            else:
                rows, terminal = native_stream(process.stdout)
                value = project(rows[0])
                self.assertEqual((terminal['state'], terminal['status']), (2, 1))
            self.validate('why', value)
            self.assertEqual(value['error'], 'invalid_store')
            self.assertIsNone(value['success_fingerprint_timestamp'])
            self.assertEqual(value['last_ok_ts'], 0)
            legacy_aliases(value)

    def test_blame_uses_timeline_and_labels_temporal_match(self):
        file = self.directory / 'a quoted file'
        now = int(time.time())
        self.chain([(1, now, 0, b'first candidate', '-'), (2, now, 0, b'second candidate', '-')])
        # The timeline ring belongs to this process. The structured mutator
        # records its actual operation before the following query inspects it.
        quoted = shlex.quote(str(file))
        result = self.run_cli('-c', f'touch --json {quoted} > /dev/null; blame-file --data {quoted}')
        rows, terminal = native_stream(result.stdout)
        self.assertEqual(len(rows), 1)
        raw = rows[0]
        value = self.validate('blame-file', project(raw))
        self.assertGreaterEqual(value['matched'], 1)
        self.assertEqual(value['path'], str(file))
        entry = value['entries'][0]
        self.assertEqual(entry['candidate_count'], 2)
        self.assertEqual(entry['attribution'], 'temporal_proximity_not_proof')
        self.assertFalse(entry['restoration_assessed'])
        self.assertEqual(raw[1]['path'][0], 9)
        self.assertEqual(terminal['state'], 0)

    def test_blame_untracked_is_explicit_and_does_not_create(self):
        file = self.directory / 'not created'
        value, terminal, _ = self.native('blame-file', str(file), status=2)
        self.assertEqual(value['error'], 'not_recorded')
        self.assertEqual(value['entries'], [])
        self.assertFalse(file.exists())
        self.assertEqual(terminal['state'], 2)


if __name__ == '__main__':
    unittest.main(verbosity=2)
