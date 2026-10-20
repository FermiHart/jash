#!/usr/bin/env python3
"""Recovery modes: native records must match real inverses and stored checkpoints."""
import json
import base64
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
import test_management_builtins as management
from test_discovery_builtins import native_stream, project
from jsonschema import Draft202012Validator


class RecoveryNative(unittest.TestCase):
    run_cli = management.ManagementBuiltins.run_cli
    execute = management.ManagementBuiltins.execute

    @classmethod
    def setUpClass(cls):
        temporary = tempfile.TemporaryDirectory(prefix='jash-recovery-faults-')
        cls.addClassCleanup(temporary.cleanup)
        cls.hook = str(Path(temporary.name) / 'faults.so')
        subprocess.run(['cc', '-std=gnu11', '-shared', '-fPIC', '-Wall', '-Wextra',
                        '-Wpedantic', '-Werror', str(management.ROOT / 'tests/recovery_io_faults.c'),
                        '-ldl', '-o', cls.hook], check=True, capture_output=True, timeout=25)

    def setUp(self):
        management.ManagementBuiltins.setUp(self)
        self.env['JASH_TIMELINE'] = '1'
        temporary = self.directory / 'tmp'
        temporary.mkdir(mode=0o700)
        self.env['TMPDIR'] = str(temporary)

    def native(self, command, status=0):
        result = self.run_cli('-c', command, status=status)
        rows, terminal = native_stream(result.stdout)
        self.assertEqual(terminal['status'], status)
        return [project(row) for row in rows], terminal, rows

    def validate(self, report):
        schemas = {'jash.timeline.action.v1': 'builtin-timeline-action-v1',
                   'jash.checkpoint.action.v1': 'builtin-checkpoint-action-v1',
                   'jash.verify.v1': 'builtin-verify-v1',
                   'jash.timeline.v1': 'agent-timeline-v1',
                   'jash.checkpoints.v1': 'agent-checkpoints-v1',
                   'jash.undo-result.v1': 'agent-undo-result-v1',
                   'jash.recovery-quota.v1': 'agent-recovery-quota-v1',
                   'jash.recovery-store.item.v1': 'builtin-recovery-store-item-v1'}
        schema = json.loads((management.ROOT / 'schemas' / (schemas[report['schema']] + '.schema.json')).read_text())
        Draft202012Validator.check_schema(schema)
        Draft202012Validator(schema).validate(report)
        return report

    def reference(self, *argv):
        report = json.loads(self.execute([argv[0], '--json', *argv[1:]]).stdout)
        reference = report['items'][-1]['undo']
        self.assertIsNotNone(reference, report)
        return reference['ref']

    def guarded(self, *arguments, status=0):
        rows, terminal = native_stream(self.execute(['undo', '--data-v2', *arguments], status=status).stdout)
        self.assertEqual(terminal['status'], status)
        return [self.validate(project(row)) for row in rows], terminal, rows

    def test_verify_hash_and_byte_paths(self):
        # Official BLAKE3 empty-input test vector, independent of the handler.
        digest = 'af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262'
        path = self.directory / 'empty file'
        path.write_bytes(b'')
        result = self.execute(['verify', '--json', str(path)])
        row = self.validate(json.loads(result.stdout))['items'][0]
        self.assertEqual(row['digest'], digest)
        self.assertTrue(row['read_complete'])
        self.assertEqual(row['bytes_read'], 0)
        self.assertIsNone(row['matches'])
        result = self.execute(['verify', '--data', str(path), digest])
        rows, terminal = native_stream(result.stdout)
        self.assertEqual(terminal['status'], 0)
        self.assertTrue(project(rows[0])['matches'])
        self.assertEqual(rows[0][1]['path'][0], 9)  # JDV_PATH
        self.assertEqual(rows[0][1]['bytes_read'][0], 4)  # JDV_FILESIZE
        result = self.execute(['verify', '--json', str(path), '0' * 64], status=1)
        row = json.loads(result.stdout)['items'][0]
        self.assertFalse(row['matches'])
        self.assertEqual(row['error'], 'digest_mismatch')

    def test_verify_rejects_nonregular_and_invalid_expected(self):
        fifo = self.directory / 'input-fifo'
        os.mkfifo(fifo)
        result = self.execute(['verify', '--json', str(fifo)], status=1)
        row = json.loads(result.stdout)['items'][0]
        self.assertFalse(row['read_complete'])
        self.assertIsNone(row['digest'])
        self.assertEqual(row['error'], 'not_regular_file')
        result = self.execute(['verify', '--data', 'never-opened', 'not-a-hash'], status=2)
        rows, terminal = native_stream(result.stdout)
        self.assertEqual(rows, [])
        self.assertEqual(terminal['state'], 2)

    def test_verify_literal_byte_path_and_terminator(self):
        literal = b'--json\tquote\'\nnonutf8-\xff'
        absolute = os.fsencode(self.directory) + b'/' + literal
        with open(absolute, 'wb') as file:
            file.write(b'')
        expected_path = {'encoding': 'base64', 'data': base64.b64encode(literal).decode()}
        result = self.execute(['verify', '--json', '--', literal])
        report = self.validate(json.loads(result.stdout))
        self.assertEqual(report['items'][0]['path'], expected_path)
        rows, terminal = native_stream(self.execute(['verify', '--data-v2', '--', literal]).stdout)
        self.assertEqual((terminal['state'], terminal['status']), (0, 0))
        self.assertEqual(rows[0][1]['path'], (9, literal))

    def test_verify_read_and_close_faults_never_publish_partial_digest(self):
        path = self.directory / 'verify-input'
        path.write_bytes(b'fixture\n' * 20000)
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_RECOVERY_ROOT=str(self.directory))
        for fault, error, complete_bytes in [('verify_read', 'read_failed', False),
                                            ('verify_short_eof', 'file_changed', False),
                                            ('verify_changed', 'file_changed', True),
                                            ('verify_close', 'close_failed', True)]:
            self.env['JASH_TEST_RECOVERY_FAULT'] = fault
            for selector in ('--json', '--data-v2'):
                with self.subTest(fault=fault, selector=selector):
                    result = self.execute(['verify', selector, str(path)], status=1)
                    if selector == '--json':
                        row = self.validate(json.loads(result.stdout))['items'][0]
                    else:
                        records, terminal = native_stream(result.stdout)
                        row = project(records[0])
                        self.assertEqual((terminal['state'], terminal['completeness'], terminal['status']), (2, 2, 1))
                    self.assertFalse(row['read_complete'])
                    self.assertIsNone(row['digest'])
                    self.assertEqual(row['error'], error)
                    self.assertEqual(row['bytes_read'], path.stat().st_size if complete_bytes else 65536)

    def test_verify_eintr_and_short_reads_match_uninterrupted_digest(self):
        path = self.directory / 'verify-input'
        path.write_bytes(b'deterministic verify short-read fixture\n' * 100)
        expected = json.loads(self.execute(['verify', '--json', str(path)]).stdout)['items'][0]['digest']
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_RECOVERY_ROOT=str(self.directory),
                        JASH_TEST_RECOVERY_FAULT='verify_eintr_short')
        rows, terminal = native_stream(self.execute(['verify', '--data', str(path), expected]).stdout)
        self.assertEqual((terminal['state'], terminal['status']), (0, 0))
        self.assertTrue(project(rows[0])['matches'])

    def test_timeline_native_exact_entries_and_paging(self):
        rows, terminal, raw = self.native('mkdir first second third; timeline --data --offset 1 --limit 1')
        self.assertEqual(terminal['state'], 0)
        self.assertEqual(len(rows), 1)
        report = self.validate(rows[0])
        self.assertEqual(report['schema'], 'jash.timeline.v1')
        self.assertEqual(report['mutations'], 3)
        self.assertEqual(report['count'], 1)
        self.assertEqual(report['next_offset'], 2)
        self.assertEqual(report['entries'][0]['a'], str(self.directory / 'second'))
        self.assertEqual(int(report['entries'][0]['entry_id']), report['entries'][0]['id'])
        self.assertEqual(raw[0][1]['entries'][1][0][1]['a'][0], 9)

    def test_undo_and_redo_native_match_actual_filesystem(self):
        rows, terminal, _ = self.native('mkdir created; undo --data')
        self.assertEqual(terminal['state'], 0)
        self.assertFalse((self.directory / 'created').exists())
        action = next(row for row in rows if row['record'] == 'action')
        self.assertEqual(action['status'], 'completed')
        self.assertEqual(action['operation'], 'undo')
        self.assertEqual(action['a'], str(self.directory / 'created'))
        rows, terminal, _ = self.native('mkdir redone; undo >/dev/null; redo --data')
        self.assertEqual(terminal['state'], 0)
        self.assertTrue((self.directory / 'redone').is_dir())
        self.assertEqual(next(row for row in rows if row['record'] == 'action')['operation'], 'redo')

    def test_undo_preview_and_failed_entry_remains_live(self):
        rows, _, _ = self.native('mkdir preview; undo --data --dry-run')
        self.assertTrue((self.directory / 'preview').exists())
        action = next(row for row in rows if row['record'] == 'action')
        self.assertEqual(action['status'], 'planned')
        self.assertEqual(action['effect'], 'none')
        result = self.run_cli('-c', 'mkdir retained; touch retained/child; undo --json id 1', status=1)
        document = self.validate(json.loads(result.stdout))
        self.assertEqual(document['status'], 1)
        action = next(row for row in document['items'] if row['record'] == 'action')
        self.assertEqual(action['status'], 'failed')
        self.assertTrue(action['retained'])
        self.assertTrue((self.directory / 'retained/child').exists())

    def test_undo_failed_admission_does_not_revert(self):
        for selector in ('--json', '--data'):
            with self.subTest(selector=selector):
                name = 'admitted_' + selector[2:]
                result = self.run_cli('-c', f'mkdir {name}; undo {selector} >/dev/full', status=125)
                self.assertEqual(result.stdout, b'')
                self.assertTrue((self.directory / name).exists())

    def test_checkpoint_creation_and_native_listing(self):
        result = self.execute(['checkpoint', '--json', 'base'])
        report = self.validate(json.loads(result.stdout))
        self.assertEqual(report['schema'], 'jash.checkpoint.action.v1')
        row = report['items'][0]
        self.assertEqual(row['name'], 'base')
        self.assertTrue(row['applied'])
        self.assertTrue(row['durable'])
        rows, terminal, raw = self.native('checkpoint --data')
        self.assertEqual(terminal['state'], 0)
        self.assertEqual(rows[0]['checkpoints'][0]['name'], 'base')
        self.assertEqual(raw[0][1]['checkpoints'][1][0][1]['cwd'][0], 9)

    def test_rewind_preview_and_actual_inverse(self):
        result = self.run_cli('-c', 'checkpoint base >/dev/null; mkdir later; checkpoint diff base --json')
        report = self.validate(json.loads(result.stdout))
        self.assertEqual(report['status'], 0)
        self.assertEqual(next(row for row in report['items'] if row['record'] == 'action')['status'], 'planned')
        self.assertTrue((self.directory / 'later').exists())
        rows, terminal, _ = self.native('checkpoint reset >/dev/null; mkdir inverse; rewind reset --data')
        self.assertEqual(terminal['state'], 0)
        self.assertFalse((self.directory / 'inverse').exists())
        self.assertEqual(next(row for row in rows if row['record'] == 'action')['operation'], 'rewind')

    def test_checkpoint_and_rewind_failed_admission_preserve_state(self):
        for selector in ('--json', '--data'):
            with self.subTest(selector=selector):
                name = 'never_' + selector[2:]
                with open('/dev/full', 'wb') as sink:
                    self.execute(['checkpoint', selector, name], status=125, stdout=sink)
                listed = json.loads(self.execute(['checkpoint', '--json']).stdout)
                self.assertNotIn(name, [row['name'] for row in listed['checkpoints']])
                directory = 'keep_' + selector[2:]
                self.run_cli('-c', f'checkpoint before >/dev/null; mkdir {directory}; rewind before {selector} >/dev/full', status=125)
                self.assertTrue((self.directory / directory).is_dir())

    def test_checkpoint_publication_phase_matches_actual_state(self):
        self.env.update(LD_PRELOAD=self.hook, JASH_TEST_RECOVERY_ROOT=str(self.directory))
        for fault, applied in [('checkpoint_file_sync', False), ('checkpoint_rename', False),
                               ('checkpoint_directory_sync', True)]:
            self.env['JASH_TEST_RECOVERY_FAULT'] = fault
            for selector in ('--json', '--data'):
                with self.subTest(fault=fault, selector=selector):
                    name = fault + selector[2:]
                    result = self.execute(['checkpoint', selector, name], status=1)
                    if selector == '--json':
                        row = self.validate(json.loads(result.stdout))['items'][0]
                    else:
                        records, terminal = native_stream(result.stdout)
                        self.assertEqual((terminal['state'], terminal['status']), (2, 1))
                        row = project(records[0])
                    self.assertEqual(row['applied'], applied)
                    self.assertFalse(row['durable'])
                    self.assertEqual(row['error'], 'checkpoint_directory_sync_failed' if applied else 'checkpoint_publication_failed')
                    names = [item['name'] for item in json.loads(self.execute(['checkpoint', '--json']).stdout)['checkpoints']]
                    self.assertEqual(name in names, applied)

    def test_rewind_scoped_selection_and_partial_failure_stop(self):
        (self.directory / 'scope').mkdir()
        (self.directory / 'elsewhere').mkdir()
        script = ('checkpoint before >/dev/null; mkdir scope/inside elsewhere/outside; '
                  'rewind --json before --scope scope')
        report = self.validate(json.loads(self.run_cli('-c', script).stdout))
        actions = [row for row in report['items'] if row['record'] == 'action']
        self.assertEqual([row['status'] for row in actions], ['kept', 'completed'])
        self.assertFalse((self.directory / 'scope/inside').exists())
        self.assertTrue((self.directory / 'elsewhere/outside').is_dir())
        script = ('mkdir earlier blocked; /usr/bin/touch blocked/foreign; undo --data 2')
        rows, terminal, _ = self.native(script, status=1)
        self.assertEqual(terminal['state'], 2)
        actions = [row for row in rows if row['record'] == 'action']
        self.assertEqual(len(actions), 1)
        self.assertEqual(actions[0]['status'], 'failed')
        self.assertTrue((self.directory / 'earlier').is_dir())

    def test_rewind_rejects_missing_history_and_malformed_checkpoint(self):
        operands = ' '.join('ring_' + str(i) for i in range(130))
        result = self.run_cli('-c', 'checkpoint before >/dev/null; mkdir ' + operands + '; rewind before --data', status=1)
        rows, terminal = native_stream(result.stdout)
        self.assertEqual((terminal['state'], terminal['status']), (2, 1))
        summary = next(project(row) for row in rows if project(row)['record'] == 'summary')
        self.assertEqual(summary['error'], 'history_unavailable')
        self.assertEqual(summary['completed'], 0)
        self.assertTrue(all((self.directory / ('ring_' + str(i))).exists() for i in range(130)))
        checkpoint = self.directory / '.jash/checkpoints.tsv'
        checkpoint.write_text('invalid\tdepth\tbroken\n')
        result = self.execute(['rewind', '--data'], status=1)
        rows, terminal = native_stream(result.stdout)
        self.assertEqual(rows, [])
        self.assertEqual((terminal['state'], terminal['status']), (2, 1))

    def test_guarded_native_inspect_preview_apply_and_consumed(self):
        name = b'guarded path-\xff'
        (self.directory / 'source').write_bytes(b'new\0content')
        token = self.reference('cp', 'source', name)
        expected = json.loads(self.execute(['undo', '--json', '--inspect', '--ref', token]).stdout)
        rows, terminal, raw = self.guarded('--inspect', '--ref', token)
        self.assertEqual(rows, [expected])
        self.assertEqual(terminal['state'], 0)
        self.assertEqual(raw[0][1]['record'][1]['path'][0], 9)
        self.assertEqual(raw[0][1]['record'][1]['payload_bytes'][0], 4)
        self.assertFalse(rows[0]['record']['target_validated'])
        rows, _, _ = self.guarded('--ref', token, '--dry-run')
        self.assertEqual((rows[0]['status'], rows[0]['effect']), ('ready', 'none'))
        self.assertTrue(os.path.exists(os.fsencode(self.directory) + b'/' + name))
        rows, terminal, _ = self.guarded('--ref', token)
        self.assertEqual((rows[0]['status'], rows[0]['effect']), ('completed', 'changed'))
        self.assertEqual(terminal['state'], 0)
        self.assertFalse(os.path.exists(os.fsencode(self.directory) + b'/' + name))
        rows, terminal, _ = self.guarded('--ref', token, status=1)
        self.assertEqual(rows[0]['code'], 'consumed')
        self.assertEqual(terminal['state'], 2)

    def test_guarded_native_restores_content_and_refuses_changed_target(self):
        (self.directory / 'source').write_bytes(b'new\0bytes')
        target = self.directory / 'target'
        target.write_bytes(b'old\0bytes')
        token = self.reference('cp', 'source', 'target')
        self.guarded('--ref', token)
        self.assertEqual(target.read_bytes(), b'old\0bytes')
        token = self.reference('cp', 'source', 'target')
        target.write_bytes(b'foreign update')
        rows, terminal, _ = self.guarded('--ref', token, status=1)
        self.assertEqual((rows[0]['code'], rows[0]['effect']), ('target_changed', 'none'))
        self.assertEqual(terminal['state'], 2)
        self.assertEqual(target.read_bytes(), b'foreign update')

    def test_guarded_native_store_gc_pin_and_quota(self):
        first = self.reference('mkdir', 'first')
        second = self.reference('mkdir', 'second')
        expected = json.loads(self.execute(['undo', '--json', '--store']).stdout)['items']
        rows, terminal, _ = self.guarded('--store')
        self.assertEqual(terminal['state'], 0)
        self.assertEqual({row['ref'] for row in rows}, {first, second})
        expected_by_ref = {row['ref']: row for row in expected}
        for row in rows:
            self.assertEqual({key: row[key] for key in expected_by_ref[row['ref']]}, expected_by_ref[row['ref']])
        self.assertEqual(rows[-1]['scanned'], 2)
        rows, terminal, raw = self.guarded('--usage')
        used = rows[0]['used_bytes']
        self.assertGreater(used, 0)
        self.assertEqual(raw[0][1]['used_bytes'][0], 4)
        rows, _, _ = self.guarded('--quota', str(used))
        self.assertTrue(rows[0]['applied'])
        self.assertEqual(rows[0]['limit_bytes'], used)
        rows, terminal, _ = self.guarded('--quota', '1', '--dry-run', status=1)
        self.assertEqual(rows[0]['error'], 'quota_below_usage')
        self.assertFalse(rows[0]['applied'])
        self.assertEqual(terminal['state'], 2)
        self.guarded('--ref', first)
        rows, _, _ = self.guarded('--pin', '--ref', first)
        self.assertEqual(rows[0]['action'], 'pinned')
        self.assertTrue(rows[0]['pinned'])
        rows, _, _ = self.guarded('--gc', '--dry-run')
        self.assertTrue(all(row['action'] == 'kept' for row in rows))
        self.guarded('--unpin', '--ref', first)
        rows, _, _ = self.guarded('--gc', '--dry-run')
        self.assertEqual(next(row for row in rows if row['ref'] == first)['action'], 'would_remove')
        rows, terminal, _ = self.guarded('--gc')
        self.assertEqual(terminal['state'], 0)
        self.assertEqual(next(row for row in rows if row['ref'] == first)['action'], 'removed')
        store = self.directory / '.jash/recovery-v1'
        self.assertFalse((store / first).exists())
        self.assertTrue((store / second).exists())

    def test_guarded_native_store_limit_is_authoritative_incomplete(self):
        self.reference('mkdir', 'first')
        self.reference('mkdir', 'second')
        rows, terminal, _ = self.guarded('--store', '--limit', '1', status=75)
        self.assertEqual(len(rows), 1)
        self.assertEqual((terminal['state'], terminal['completeness'], terminal['truncated']), (3, 2, True))
        self.assertEqual(terminal['reason'], 'scan_limit')

    def test_guarded_native_admission_precedes_all_effects(self):
        token = self.reference('mkdir', 'protected')
        store = self.directory / '.jash/recovery-v1'
        initial = (store / token).read_bytes()
        for arguments in [('--ref', token), ('--quota', '100000'), ('--pin', '--ref', token)]:
            with self.subTest(arguments=arguments), open('/dev/full', 'wb') as sink:
                self.execute(['undo', '--data', *arguments], status=125, stdout=sink)
            self.assertTrue((self.directory / 'protected').is_dir())
            self.assertEqual((store / token).read_bytes(), initial)
            self.assertFalse((store / '.quota').exists())
        self.guarded('--ref', token)
        with open('/dev/full', 'wb') as sink:
            self.execute(['undo', '--data', '--gc'], status=125, stdout=sink)
        self.assertTrue((store / token).exists())

    def test_guarded_native_malformed_refs_and_conflicting_operations(self):
        for token in ('../escape', '', '0' * 31):
            with self.subTest(token=token):
                rows, terminal, _ = self.guarded('--ref', token, status=2)
                self.assertEqual(rows[0]['code'], 'invalid_arguments')
                self.assertEqual(terminal['state'], 2)
        for arguments in [('--quota', '1', '--store'), ('--store', '--limit', '0'), ('--store', '--bogus')]:
            with self.subTest(arguments=arguments):
                rows, terminal = native_stream(self.execute(['undo', '--data', *arguments], status=2).stdout)
                self.assertEqual(rows, [])
                self.assertEqual((terminal['state'], terminal['status']), (2, 2))

    def test_gc_failed_first_receipt_stops_before_second_removal(self):
        for index, selector in enumerate(('--json', '--data', '--data-v2')):
            with self.subTest(selector=selector):
                first = self.reference('mkdir', 'first-' + str(index))
                second = self.reference('mkdir', 'second-' + str(index))
                self.guarded('--ref', first); self.guarded('--ref', second)
                store = self.directory / '.jash/recovery-v1'
                self.env.update(LD_PRELOAD=self.hook, JASH_TEST_RECOVERY_FAULT='gc_receipt',
                                JASH_TEST_RECOVERY_ROOT=str(self.directory))
                result = self.execute(['undo', selector, '--gc'], status=125)
                del self.env['LD_PRELOAD']; del self.env['JASH_TEST_RECOVERY_FAULT']
                self.assertGreater(len(result.stdout), 0)  # admission was delivered
                self.assertEqual(sum((store / token).exists() for token in (first, second)), 1)
                self.assertEqual(list(store.glob('.gc-*')), [])
                if selector == '--json':
                    with self.assertRaises(json.JSONDecodeError):
                        json.loads(result.stdout)
                else:
                    with self.assertRaises(AssertionError):
                        native_stream(result.stdout)
                rows, terminal, _ = self.guarded('--gc')
                self.assertEqual(len(rows), 1)
                self.assertEqual(rows[0]['action'], 'removed')
                self.assertEqual(terminal['state'], 0)


    def test_trash_gc_first_receipt_failure_preserves_second_entry(self):
        trash = self.directory / '.jash/trash'
        trash.mkdir(parents=True, exist_ok=True)
        for selector in ('--json', '--data', '--data-v2'):
            with self.subTest(selector=selector):
                (trash / 'first').write_bytes(b'first synthetic garbage')
                (trash / 'second').write_bytes(b'second synthetic garbage')
                self.env.update(LD_PRELOAD=self.hook, JASH_TEST_RECOVERY_FAULT='trash_gc_receipt',
                                JASH_TEST_RECOVERY_ROOT=str(self.directory))
                result = self.execute(['trash', selector, 'gc'], status=125)
                del self.env['LD_PRELOAD']; del self.env['JASH_TEST_RECOVERY_FAULT']
                self.assertGreater(len(result.stdout), 0)
                self.assertEqual(sum((trash / name).exists() for name in ('first', 'second')), 1)
                self.assertEqual(list(trash.glob('.jash-trash-gc-*')), [])
                if selector == '--json':
                    with self.assertRaises(json.JSONDecodeError): json.loads(result.stdout)
                else:
                    with self.assertRaises(AssertionError): native_stream(result.stdout)
                retry = self.execute(['trash', '--data-v2', 'gc'])
                rows, terminal = native_stream(retry.stdout)
                summary = next(project(row) for row in rows if project(row)['record'] == 'summary')
                self.assertEqual((summary['removed'], summary['failed'], terminal['state']), (1, 0, 0))
                self.assertEqual(list(trash.iterdir()), [])

    def test_checkpoint_incomplete_scan_is_failed_terminal(self):
        path = self.directory / '.jash/checkpoints.tsv'
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('invalid\tdepth\tbroken\n')
        for builtin, operation in [('checkpoint', 'list'), ('rewind', 'list')]:
            document = self.validate(json.loads(self.execute([builtin, '--json', operation], status=1).stdout))
            self.assertFalse(document['scan_complete'])
            self.assertGreater(document['invalid_rows'], 0)
            for selector in ('--data', '--data-v2'):
                rows, terminal = native_stream(self.execute([builtin, selector, operation], status=1).stdout)
                self.assertEqual(len(rows), 1)
                self.assertFalse(self.validate(project(rows[0]))['scan_complete'])
                self.assertEqual((terminal['state'], terminal['completeness'], terminal['status']), (2, 2, 1))
                self.assertGreater(terminal['errors'], 0)



if __name__ == '__main__':
    unittest.main(verbosity=2)
