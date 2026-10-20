#!/usr/bin/env python3
"""Native mutator receipts: real effects, typed identity and checked terminals."""
import copy
import json
import os
import shlex
import stat
import unittest

import test_agent_mutations_json as fixture
from test_structured_pipe import native_projection, native_rows
from semantic_schema_fixture import semantic_validator


class MutationData(unittest.TestCase):
    setUp = fixture.Mutations.setUp
    runj = fixture.Mutations.runj

    def manifest(self, *argv, rc=0):
        result = self.runj(shlex.join(argv))
        self.assertEqual(result.returncode, rc, result.stderr)
        rows, terminal = native_rows(result.stdout)
        self.assertEqual(terminal['producer_status'], rc)
        self.assertEqual(terminal['transport_status'], 0)
        self.assertEqual(terminal['transform_status'], 0)
        self.assertEqual(terminal['hash_present'], 1)
        self.assertEqual(terminal['state'], 0 if not rc else 1 if rc in (130, 143) else 2)
        self.assertEqual(terminal['completeness'], 1 if not rc else 2)
        records = []
        for row, credit in rows:
            semantic_validator('mutation-data-row').validate(native_projection((12, row)))
            self.assertEqual(row['schema'], (10, b'jash.mutation.data.v1'))
            self.assertEqual(row['operation'], (10, argv[0].encode()))
            self.assertEqual(row['kind'][0], 10)
            self.assertEqual(row['data'][0], 12)
            records.append((row['kind'][1].decode(), row['data'][1], credit))
        self.assertEqual(records[0][0], 'header')
        self.assertEqual(records[-1][0], 'summary')
        actions = [value for kind, value, _ in records if kind == 'action']
        summary = records[-1][1]
        self.assertEqual(summary['exit_code'], (2, rc))
        counts = {key: value[1] for key, value in summary['counts'][1].items()}
        observed = {key: 0 for key in ['completed', 'failed', 'partial', 'planned', 'skipped']}
        for index, action in enumerate(actions):
            self.assertEqual(action['index'], (3, index))
            self.assertEqual(action['status'][0], 10)
            observed[action['status'][1].decode()] += 1
        self.assertEqual(observed, counts)
        # Production JTV2 import independently validates schema, hash, credit
        # totals and terminal, rather than trusting this test's small decoder.
        imported = self.runj('count --from-data', input=result.stdout)
        if not rc:
            self.assertEqual(imported.returncode, 0, imported.stderr)
            self.assertEqual(json.loads(imported.stdout), {'count': len(records)})
        else:
            self.assertNotEqual(imported.returncode, 0)
            self.assertEqual(imported.stdout, b'')
        return actions, summary, terminal, records

    def test_published_native_receipt_schema_rejects_wrong_units_and_shapes(self):
        result = self.runj('touch --data --dry-run preview')
        self.assertEqual(result.returncode, 0)
        rows, _ = native_rows(result.stdout)
        action = native_projection((12, next(row for row, _ in rows if row['kind'][1] == b'action')))
        validator = semantic_validator('mutation-data-row')
        validator.validate(action)
        invalid = copy.deepcopy(action); invalid['data']['index'] = '0'
        self.assertFalse(validator.is_valid(invalid))
        invalid = copy.deepcopy(action); invalid['data']['effect'] = 'changed'
        self.assertFalse(validator.is_valid(invalid))
        invalid = copy.deepcopy(action); del invalid['data']['source']
        self.assertFalse(validator.is_valid(invalid))
        invalid = copy.deepcopy(action); invalid['kind'] = 'progress'
        self.assertFalse(validator.is_valid(invalid))

    def prepare(self, operation, suffix=''):
        src, dst = operation + suffix + '-src', operation + suffix + '-dst'
        if operation in ('cp', 'mv', 'ln'):
            (self.work / src).write_bytes(b'old\x00payload\n')
            args = [src, dst]
        elif operation == 'rmdir':
            (self.work / dst).mkdir()
            args = [dst]
        elif operation in ('rm', 'chmod', 'chown', 'sed'):
            (self.work / dst).write_text('old payload\n')
            (self.work / dst).chmod(0o600)
            args = ['0640', dst] if operation == 'chmod' else \
                [f'{os.getuid()}:{os.getgid()}', dst] if operation == 'chown' else \
                ['-i', 's/old/new/', dst] if operation == 'sed' else [dst]
        else:
            args = [dst]
        return args, src, dst

    def state(self):
        result = {}
        for path in self.work.iterdir():
            observed = path.lstat()
            result[path.name] = (observed.st_ino, stat.S_IFMT(observed.st_mode),
                                 stat.S_IMODE(observed.st_mode),
                                 path.read_bytes() if path.is_file() else None)
        return result

    def test_all_ten_providers_apply_and_alias(self):
        for flag in ['--data', '--data-v2']:
            for operation in ['cp', 'mv', 'rm', 'touch', 'mkdir', 'rmdir', 'ln', 'chmod', 'chown', 'sed']:
                with self.subTest(operation=operation, flag=flag):
                    args, src, dst = self.prepare(operation, flag)
                    actions, summary, _, _ = self.manifest(operation, flag, *args)
                    self.assertEqual(summary['mode'], (10, b'apply'))
                    self.assertEqual(summary['status'], (10, b'completed'))
                    self.assertEqual(len(actions), 1)
                    if operation in ('rm', 'rmdir'):
                        self.assertFalse((self.work / dst).exists())
                    elif operation == 'mv':
                        self.assertFalse((self.work / src).exists())
                        self.assertEqual((self.work / dst).read_bytes(), b'old\x00payload\n')
                    elif operation == 'cp':
                        self.assertEqual((self.work / dst).read_bytes(), b'old\x00payload\n')
                        self.assertEqual(actions[0]['bytes_written'], (4, 12))
                    elif operation == 'ln':
                        self.assertEqual((self.work / src).stat().st_ino, (self.work / dst).stat().st_ino)
                    elif operation == 'chmod':
                        self.assertEqual(stat.S_IMODE((self.work / dst).stat().st_mode), 0o640)
                    elif operation == 'sed':
                        self.assertEqual((self.work / dst).read_text(), 'new payload\n')
                    else:
                        self.assertTrue((self.work / dst).exists())

    def test_all_ten_previews_preserve_requested_state(self):
        for operation in ['cp', 'mv', 'rm', 'touch', 'mkdir', 'rmdir', 'ln', 'chmod', 'chown', 'sed']:
            with self.subTest(operation=operation):
                args, _, _ = self.prepare(operation)
                before = self.state()
                actions, summary, _, _ = self.manifest(operation, '--data', '--dry-run', *args)
                self.assertEqual(self.state(), before)
                self.assertEqual(summary['mode'], (10, b'preview'))
                self.assertTrue(all(action['status'] == (10, b'planned') for action in actions))

    @unittest.skipUnless(os.path.exists('/dev/full'), '/dev/full requires Linux')
    def test_all_ten_reject_full_sink_before_effects(self):
        for operation in ['cp', 'mv', 'rm', 'touch', 'mkdir', 'rmdir', 'ln', 'chmod', 'chown', 'sed']:
            with self.subTest(operation=operation):
                args, _, _ = self.prepare(operation)
                before = self.state()
                result = self.runj(shlex.join([operation, '--data', *args]) + ' > /dev/full')
                self.assertEqual(result.returncode, 125, result.stderr)
                self.assertEqual(self.state(), before)

    def test_paths_are_native_bytes_and_copy_verification_is_typed(self):
        raw = b'raw-\xff quote" space\n'
        (self.work / 'src').write_bytes(b'a\x00b')
        actions, _, terminal, _ = self.manifest('cp', '--data', '--verify', 'src', os.fsdecode(raw))
        self.assertEqual(actions[0]['source'], (9, b'src'))
        self.assertEqual(actions[0]['destination'], (9, raw))
        self.assertEqual(actions[0]['bytes_written'], (4, 3))
        self.assertEqual(actions[0]['verified'], (1, True))
        self.assertEqual(terminal['bytes'], 3)
        self.assertEqual((self.work / os.fsdecode(raw)).read_bytes(), b'a\x00b')

    def test_partial_copy_binds_effects_counts_and_failed_terminal(self):
        (self.work / 'src').write_bytes(b'payload')
        (self.work / 'dst').mkdir()
        actions, summary, terminal, _ = self.manifest('cp', '--data', 'missing', 'src', 'dst', rc=1)
        self.assertEqual([item['status'] for item in actions], [(10, b'failed'), (10, b'completed')])
        self.assertEqual(actions[0]['effect'], (10, b'none'))
        self.assertEqual(summary['status'], (10, b'partial'))
        self.assertEqual(terminal['errors'], 1)
        self.assertEqual((self.work / 'dst/src').read_bytes(), b'payload')

    def test_option_value_and_literal_format_names_keep_argv_identity(self):
        (self.work / 'src').write_text('payload')
        (self.work / '--json').mkdir()
        self.manifest('mv', '--data', '-t', '--json', 'src')
        self.assertTrue((self.work / '--json/src').exists())
        self.manifest('mkdir', '--data', '--', '--data')
        self.assertTrue((self.work / '--data').is_dir())
        self.manifest('rmdir', '--data', '--', '--data')
        (self.work / '--data').write_text('literal')
        (self.work / '--data').chmod(0o666)
        result = self.runj('chmod -w --data')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b'')
        self.assertFalse((self.work / '--data').stat().st_mode & 0o200)

    def test_conflicting_formats_reject_without_requested_effect(self):
        _, summary, _, _ = self.manifest('touch', '--data', '--json', 'unexpected', rc=2)
        self.assertEqual(summary['error_code'], (10, b'invalid_format_options'))
        self.assertFalse((self.work / 'unexpected').exists())

    def test_atomic_move_progress_is_not_an_action_or_success_claim(self):
        (self.work / 'one').write_text('1')
        (self.work / 'two').write_text('2')
        (self.work / 'dst').mkdir()
        actions, summary, _, records = self.manifest('mv', '--data', '--atomic', 'one', 'two', 'dst')
        self.assertEqual(summary['transaction'], (10, b'committed'))
        self.assertEqual(len(actions), 2)
        self.assertEqual([kind for kind, _, _ in records], ['header', 'progress', 'progress', 'action', 'action', 'summary'])
        for kind, payload, credit in records:
            if kind == 'progress':
                self.assertEqual(payload, {'phase': (10, b'atomic_pending')})
                self.assertEqual(credit, 0)
        self.assertFalse((self.work / 'one').exists())
        self.assertEqual((self.work / 'dst/one').read_text(), '1')

    def test_sed_data_never_delegates_unsupported_editor(self):
        (self.work / 'file').write_text('old\n')
        _, summary, _, _ = self.manifest('sed', '--data', '-i', '1d', 'file', rc=2)
        self.assertEqual(summary['error_code'], (10, b'native_edit_unsupported'))
        self.assertEqual((self.work / 'file').read_text(), 'old\n')


if __name__ == '__main__':
    unittest.main()
