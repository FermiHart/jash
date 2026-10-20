#!/usr/bin/env python3
"""Literal trash names, actual state transitions, retained recovery and IO admission."""
import base64
import json
import os
import shlex
import unittest

import test_management_builtins as management
from test_discovery_builtins import native_stream, project
from jsonschema import Draft202012Validator


class TrashNative(unittest.TestCase):
    setUp = management.ManagementBuiltins.setUp
    execute = management.ManagementBuiltins.execute
    run_cli = management.ManagementBuiltins.run_cli

    def validate(self, value, schema):
        contract = json.loads((management.ROOT / 'schemas' / (schema + '.schema.json')).read_text())
        Draft202012Validator.check_schema(contract)
        Draft202012Validator(contract).validate(value)

    def report(self, *arguments, status=0):
        result = self.execute(['trash', '--json', *arguments], status=status)
        report = json.loads(result.stdout)
        self.validate(report, 'builtin-trash-v1')
        self.assertEqual(report['schema'], 'jash.trash.v1')
        self.assertEqual(report['status'], status)
        return report['items']

    def native(self, *arguments, status=0):
        records, terminal = native_stream(self.execute(['trash', '--data-v2', *arguments], status=status).stdout)
        self.assertEqual(terminal['status'], status)
        rows = [project(record) for record in records]
        if arguments and arguments[0] == 'gc':
            for row in rows:
                self.validate(row, 'builtin-trash-gc-data-v1')
        else:
            self.validate({'schema': 'jash.trash.v1', 'command': 'trash', 'complete': True,
                           'status': status, 'items': rows}, 'builtin-trash-v1')
        return rows, terminal, records

    def test_move_list_restore_preserve_numeric_and_byte_names(self):
        names = [b'123_numeric name', b'byte-\xff\nquoted\'']
        for index, name in enumerate(names):
            absolute = os.fsencode(self.directory) + b'/' + name
            with open(absolute, 'wb') as file:
                file.write(b'owned fixture ' + str(index).encode())
            moved = self.report('move', name)
            action = next(row for row in moved if row['record'] == 'action')
            self.assertTrue(action['applied'])
            self.assertTrue(action['durable'])
            self.assertFalse(os.path.exists(absolute))
            rows, terminal, raw = self.native('list')
            self.assertEqual(terminal['state'], 0)
            entries = [row for row in rows if row['record'] == 'entry']
            expected = {'encoding': 'base64', 'data': base64.b64encode(absolute).decode()} if index else os.fsdecode(absolute)
            entry = next(row for row in entries if row['original_path'] == expected)
            self.assertEqual(entry['name_source'], 'metadata')
            self.assertEqual(next(row for row in raw if project(row).get('original_path') == expected)[1]['path'][0], 9)
            self.native('restore', name)
            with open(absolute, 'rb') as file:
                self.assertEqual(file.read(), b'owned fixture ' + str(index).encode())

    def test_snapshot_totals_and_protection_match_observed_state(self):
        (self.directory / 'first').write_bytes(b'123')
        (self.directory / 'second').write_bytes(b'45678')
        self.native('move', 'first', 'second')
        rows = self.report('info')
        summary = next(row for row in rows if row['record'] == 'summary')
        self.assertEqual((summary['entries'], summary['bytes']), (2, 8))
        result = self.run_cli('-c', 'trash --json on; trash --json status; trash --json off; trash --json auto')
        reports = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertTrue(reports[0]['items'][0]['protected'])
        self.assertTrue(reports[1]['items'][0]['protected'])
        self.assertFalse(reports[2]['items'][0]['protected'])
        self.assertEqual(reports[3]['items'][0]['source'], 'recording_default')

    def test_dry_run_and_failed_admission_have_no_effect(self):
        original = self.directory / 'original'
        original.write_bytes(b'keep')
        rows = self.report('move', '--dry-run', 'original')
        self.assertFalse(next(row for row in rows if row['record'] == 'action')['applied'])
        self.assertEqual(original.read_bytes(), b'keep')
        for selector in ('--json', '--data'):
            with self.subTest(selector=selector), open('/dev/full', 'wb') as sink:
                self.execute(['trash', selector, 'move', 'original'], status=125, stdout=sink)
            self.assertEqual(original.read_bytes(), b'keep')
        self.native('move', 'original')
        for selector in ('--json', '--data'):
            with self.subTest(selector=selector), open('/dev/full', 'wb') as sink:
                self.execute(['trash', selector, 'restore', 'original'], status=125, stdout=sink)
            self.assertFalse(original.exists())
        self.native('restore', '--dry-run', 'original')
        self.assertFalse(original.exists())

    def test_restore_conflict_preserves_both_contents(self):
        original = self.directory / 'original'
        original.write_bytes(b'old')
        self.native('move', 'original')
        original.write_bytes(b'new')
        rows, terminal, _ = self.native('restore', 'original', status=1)
        self.assertEqual(terminal['state'], 2)
        self.assertEqual(next(row for row in rows if row['record'] == 'action')['error'], 'restore_failed')
        self.assertEqual(original.read_bytes(), b'new')
        self.assertTrue(any(path.read_bytes() == b'old' for path in (self.directory / '.jash/trash').iterdir() if not path.name.endswith('.meta')))

    def test_legacy_ambiguous_metadata_is_reported_without_invented_path(self):
        trash = self.directory / '.jash/trash'
        trash.mkdir(parents=True)
        (trash / '100_123_name').write_bytes(b'legacy')
        (trash / '100_123_name.meta').write_text(str(self.directory) + '\n')
        rows, terminal, _ = self.native('list', status=1)
        row = next(row for row in rows if row['record'] == 'entry')
        self.assertIsNone(row['original_path'])
        self.assertEqual(row['error'], 'ambiguous_legacy_name')
        self.assertEqual(terminal['state'], 2)
        self.native('restore', '--all', status=1)
        self.assertTrue((trash / '100_123_name').exists())
        self.assertFalse((self.directory / 'name').exists())

    def test_gc_native_summary_matches_actual_deletion_and_pin_retention(self):
        trash = self.directory / '.jash/trash'
        trash.mkdir(parents=True)
        (trash / 'unrelated').write_bytes(b'discard')
        self.env['JASH_TIMELINE'] = '1'
        reference = json.loads(self.execute(['touch', '--json', str(trash / 'pinned')]).stdout)['items'][0]['undo']
        self.assertIsNotNone(reference)
        rows, terminal, _ = self.native('gc', status=1)
        summary = next(row for row in rows if row['record'] == 'summary')
        self.assertGreaterEqual(summary['retained'], 1)
        self.assertTrue(summary['pins_complete'])
        self.assertEqual(terminal['state'], 2)
        self.assertFalse((trash / 'unrelated').exists())
        self.assertTrue((trash / 'pinned').exists())

    def test_invalid_options_and_literal_format_name(self):
        for arguments in [('restore',), ('list', 'extra'), ('move', '--bogus', 'file')]:
            records, terminal = native_stream(self.execute(['trash', '--data', *arguments], status=2).stdout)
            self.assertEqual(records, [])
            self.assertEqual((terminal['state'], terminal['status']), (2, 2))
        (self.directory / '--json').write_bytes(b'literal')
        self.native('--', '--json')
        self.assertFalse((self.directory / '--json').exists())
        self.native('restore', '--', '--json')
        self.assertEqual((self.directory / '--json').read_bytes(), b'literal')


if __name__ == '__main__':
    unittest.main(verbosity=2)
