#!/usr/bin/env python3
"""Memory providers: exact values, observed persistence, and typed outcomes."""
import base64
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
from jsonschema import Draft202012Validator

import test_management_builtins as management
from test_discovery_builtins import native_stream, project


class MemoryNative(unittest.TestCase):
    run_cli = management.ManagementBuiltins.run_cli
    execute = management.ManagementBuiltins.execute

    @classmethod
    def setUpClass(cls):
        directory = tempfile.TemporaryDirectory(prefix='jash-memory-faults-')
        cls.addClassCleanup(directory.cleanup)
        cls.fault_library = str(Path(directory.name) / 'faults.so')
        subprocess.run(['cc', '-std=gnu11', '-shared', '-fPIC', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                        str(management.ROOT / 'tests/memory_io_faults.c'), '-ldl', '-o', cls.fault_library],
                       check=True, capture_output=True, timeout=25)

    def setUp(self):
        management.ManagementBuiltins.setUp(self)
        temporary = self.directory / 'tmp'
        temporary.mkdir(mode=0o700)
        self.env['TMPDIR'] = str(temporary)

    def report(self, command, *arguments, status=0):
        row = json.loads(self.execute([command, '--json', *arguments], status=status).stdout)
        self.assertEqual(row['schema'], 'jash.' + command + '.v1')
        return self.validate(command, row)

    def validate(self, command, row):
        schema = json.loads((management.ROOT / 'schemas' / ('builtin-' + command + '-v1.schema.json')).read_text())
        Draft202012Validator.check_schema(schema)
        Draft202012Validator(schema).validate(row)
        return row

    def native(self, command, *arguments, status=0):
        records, terminal = native_stream(self.execute([command, '--data-v2', *arguments], status=status).stdout)
        self.assertEqual(terminal['status'], status)
        self.assertEqual(len(records), 1)
        row = project(records[0])
        self.assertEqual(row['schema'], 'jash.' + command + '.v1')
        return self.validate(command, row), terminal, records[0]

    def test_kv_exact_values_and_cross_process_state(self):
        value = 'tabs\tline\nslash\\"quoted" --json'
        saved = self.report('kv', 'set', 'precise key', value)
        self.assertTrue(saved['applied'])
        self.assertTrue(saved['durable'])
        got, terminal, _ = self.native('kv', 'get', 'precise key')
        self.assertEqual(got['value'], value)
        self.assertEqual(terminal['state'], 0)
        self.assertTrue(got['found'])
        raw = b'raw-\xff\t\n\\'
        self.native('kv', 'set', 'binary', raw)
        got = self.report('kv', 'get', 'binary')
        self.assertEqual(got['value'], {'encoding': 'base64', 'data': base64.b64encode(raw).decode()})
        deleted = self.report('kv', 'del', 'precise key')
        self.assertTrue(deleted['applied'])
        got, terminal, _ = self.native('kv', 'get', 'precise key', status=1)
        self.assertFalse(got['found'])
        self.assertIsNone(got['value'])
        self.assertEqual(terminal['state'], 2)

    def test_kv_pages_are_exact_and_format_values_are_literal(self):
        for key in ('a', 'b', 'c'):
            self.report('kv', 'set', key, '--data-v2')
        first = self.report('kv', 'list', '--limit', '2')
        self.assertEqual(first['total'], 3)
        self.assertEqual([row['key'] for row in first['entries']], ['a', 'b'])
        self.assertEqual(first['next_offset'], 2)
        second, terminal, _ = self.native('kv', 'list', '--offset', '2', '--limit', '2')
        self.assertEqual(second['entries'], [{'key': 'c', 'value': '--data-v2'}])
        self.assertIsNone(second['next_offset'])
        self.assertEqual(terminal['state'], 0)
        self.report('kv', 'set', '--json', '--data')
        self.assertEqual(self.report('kv', 'get', '--json')['value'], '--data')

    def test_kv_invalid_store_is_not_silently_rewritten(self):
        self.report('kv', 'set', 'keep', 'original')
        store = self.directory / '.jash/kv.db'
        for malformed in (b'no tab\n', b'key\tbad\\q\n', b'key\tvalue', b'a\tone\na\ttwo\n'):
            store.write_bytes(malformed)
            row = self.report('kv', 'set', 'new', 'value', status=1)
            self.assertFalse(row['applied'])
            self.assertEqual(row['error'], 'invalid_store')
            self.assertEqual(store.read_bytes(), malformed)

    def test_kv_invalid_arguments_and_admission_preserve_store(self):
        self.report('kv', 'set', 'keep', 'original')
        store = self.directory / '.jash/kv.db'
        before = store.read_bytes()
        for arguments in [('set', 'bad\tkey', 'v'), ('set', 'k'), ('list', '--limit', '-1'), ('--json', 'list')]:
            with self.subTest(arguments=arguments):
                self.execute(['kv', '--data', *arguments], status=2)
                self.assertEqual(store.read_bytes(), before)
        for selector in ('--json', '--data'):
            with self.subTest(selector=selector), open('/dev/full', 'wb') as sink:
                self.execute(['kv', selector, 'set', 'keep', 'changed'], stdout=sink, status=125)
            self.assertEqual(store.read_bytes(), before)

    def test_kv_refuses_symlink_and_oversize_store(self):
        outside = self.directory / 'outside'
        outside.write_bytes(b'keep\tvalue\n')
        store = self.directory / '.jash/kv.db'
        store.parent.mkdir(mode=0o700, exist_ok=True)
        store.symlink_to(outside)
        row = self.report('kv', 'set', 'keep', 'changed', status=1)
        self.assertFalse(row['applied'])
        self.assertEqual(outside.read_bytes(), b'keep\tvalue\n')
        store.unlink()
        store.write_bytes(b'x' * (512 * 1024 + 1))
        row, terminal, _ = self.native('kv', 'list', status=1)
        self.assertEqual(row['error'], 'store_limit')
        self.assertEqual(terminal['state'], 2)

    def test_intent_set_clear_and_environment_agree(self):
        text = 'fix the quoted "parser"'
        row = self.report('intent', 'set', text)
        self.assertEqual(row['value'], text)
        self.assertTrue(row['applied'])
        self.assertTrue(row['durable'])
        row, terminal, _ = self.native('intent')
        self.assertEqual(row['value'], text)
        self.assertEqual(terminal['state'], 0)
        result = self.run_cli('-c', 'intent --json clear; env --json')
        documents = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
        self.assertIsNone(documents[0]['value'])
        self.assertEqual(documents[1].get('JASH_INTENT', ''), '')
        self.assertFalse((self.directory / '.jash/intent').exists())

    def test_intent_rejects_unrepresentable_and_failed_admission(self):
        self.report('intent', 'set', 'keep')
        for text in ('x' * 512, 'two\nlines', 'carriage\rreturn'):
            self.execute(['intent', '--data', 'set', text], status=2)
        for selector in ('--json', '--data'):
            with open('/dev/full', 'wb') as sink:
                self.execute(['intent', selector, 'set', 'changed'], stdout=sink, status=125)
        self.assertEqual(self.report('intent')['value'], 'keep')

    def test_resume_records_saved_cwd_and_checks_that_directory(self):
        project_dir = self.directory / 'project'
        project_dir.mkdir()
        watched = project_dir / 'source.c'
        watched.write_text('synthetic source\n')
        script = 'cd ' + shlex.quote(str(project_dir)) + '; resume --json set ' + shlex.quote('Inspect source.c')
        saved = json.loads(self.run_cli('-c', script).stdout)
        self.assertTrue(saved['applied'])
        self.assertTrue(saved['durable'])
        row, terminal, raw = self.native('resume')
        self.assertEqual(row['note'], 'Inspect source.c')
        self.assertEqual(row['cwd'], str(project_dir))
        self.assertEqual(row['files'][0]['resolved_path'], str(watched))
        self.assertFalse(row['files'][0]['missing'])
        self.assertFalse(row['stale'])
        self.assertEqual(raw[1]['cwd'][0], 9)
        self.assertEqual(terminal['state'], 0)
        watched.unlink()
        row = self.report('resume', status=1)
        self.assertTrue(row['files'][0]['missing'])
        self.assertTrue(row['stale'])
        self.assertIn('truth_of_note', row['not_verified'])

    def test_resume_forget_and_failed_admission(self):
        self.report('resume', 'set', 'a note')
        store = self.directory / '.jash/resume.txt'
        before = store.read_bytes()
        for selector in ('--json', '--data'):
            with open('/dev/full', 'wb') as sink:
                self.execute(['resume', selector, '--forget'], status=125, stdout=sink)
            self.assertEqual(store.read_bytes(), before)
        row = self.report('resume', '--forget')
        self.assertTrue(row['applied'])
        self.assertTrue(row['durable'])
        self.assertFalse(store.exists())
        row, terminal, _ = self.native('resume')
        self.assertIsNone(row['note'])
        self.assertEqual(terminal['state'], 0)

    def test_resume_rejects_truncation_and_malformed_store(self):
        self.execute(['resume', '--data', 'set', 'x' * 4000], status=2)
        self.report('resume', 'set', 'original')
        store = self.directory / '.jash/resume.txt'
        store.write_bytes(b'invalid\nheaders\n')
        row, terminal, _ = self.native('resume', status=1)
        self.assertEqual(row['error'], 'invalid_store')
        self.assertEqual(terminal['state'], 2)

    def test_goal_checkpoints_match_real_persistent_transitions(self):
        initial = self.report('goal')
        self.assertIsNone(initial['current'])
        begun, terminal, _ = self.native('goal', 'begin', 'fix synthetic flow')
        identifier = begun['current']['id']
        self.assertTrue(begun['applied'])
        self.assertTrue(begun['durable'])
        self.assertEqual(terminal['state'], 0)
        stepped = self.report('goal', 'step', 'inspect exact inputs')
        self.assertEqual(stepped['current']['id'], identifier)
        self.assertEqual(stepped['current']['kind'], 'step')
        verified = self.report('goal', 'verify', 'the caller claims a check passed')
        self.assertEqual(verified['current']['verification'], 'user_recorded_claim')
        self.assertEqual(verified['current']['rc_hint'], -1)
        log = self.directory / '.jash/goals' / f'goal_{identifier:05}.log'
        self.assertIn('inspect exact inputs', log.read_text())
        self.assertIn('the caller claims a check passed', log.read_text())
        closed = self.report('goal', 'close', '--summary')
        self.assertTrue(closed['current_applied'])
        self.assertIsNone(closed['current'])
        self.assertIn('[close]', log.read_text())
        self.assertIsNone(self.report('goal')['current'])
        missing, terminal, _ = self.native('goal', 'step', status=1)
        self.assertEqual(missing['error'], 'no_active_goal')
        self.assertFalse(missing['applied'])
        self.assertEqual(terminal['state'], 2)

    def test_goal_recall_uses_actual_logs_and_pages(self):
        identifiers = []
        for text in ('needle alpha', 'different', 'needle beta'):
            identifiers.append(self.report('goal', 'begin', text)['current']['id'])
            self.report('goal', 'close')
        first = self.report('goal', 'recall', 'needle', '--limit', '1')
        self.assertEqual(first['total'], 2)
        self.assertEqual(first['next_offset'], 1)
        second, terminal, _ = self.native('goal', 'recall', 'needle', '--offset', '1', '--limit', '1')
        expected = [str(self.directory / '.jash/goals' / f'goal_{identifiers[i]:05}.log') for i in (0, 2)]
        self.assertEqual(first['matches'] + second['matches'], expected)
        self.assertIsNone(second['next_offset'])
        self.assertEqual(terminal['state'], 0)

    def test_goal_admission_and_invalid_input_do_not_add_checkpoint(self):
        self.report('goal', 'begin', 'keep')
        directory = self.directory / '.jash/goals'
        before = {p.name: p.read_bytes() for p in directory.iterdir() if not p.name.startswith('.')}
        for selector in ('--json', '--data'):
            with open('/dev/full', 'wb') as sink:
                self.execute(['goal', selector, 'step', 'changed'], status=125, stdout=sink)
            self.assertEqual(before, {p.name: p.read_bytes() for p in directory.iterdir() if not p.name.startswith('.')})
        for arguments in [('begin', 'x' * 512), ('step', 'line\nbreak'), ('recall', '['), ('status', 'unexpected')]:
            self.execute(['goal', '--data', *arguments], status=2)
        self.assertEqual(before, {p.name: p.read_bytes() for p in directory.iterdir() if not p.name.startswith('.')})

    def test_json_receipt_budget_is_reserved_before_memory_effects(self):
        commands = [('kv', 'set', 'budget-key', 'value'),
                    ('intent', 'set', 'budget-intent'),
                    ('resume', 'set', 'budget-note'),
                    ('goal', 'begin', 'budget-goal')]
        self.env['JASH_LLM_BUDGET'] = '1'
        for command in commands:
            with self.subTest(command=command[0]):
                value = json.loads(self.execute([command[0], '--json', *command[1:]], status=75).stdout)
                self.assertEqual(value['schema'], 'jash.builtin.error.v1')
                self.assertFalse(value['complete'])
                self.assertFalse((self.directory / '.jash/kv.db').exists())
                self.assertFalse((self.directory / '.jash/intent').exists())
                self.assertFalse((self.directory / '.jash/resume.txt').exists())
                self.assertFalse((self.directory / '.jash/goals').exists())
        # Presentation budgets must not disable explicitly native mutations.
        for command in commands:
            row, terminal, _ = self.native(command[0], *command[1:])
            self.assertTrue(row['applied'])
            self.assertEqual(terminal['state'], 0)

    def test_kv_native_writers_do_not_lose_concurrent_updates(self):
        processes = []
        for i in range(12):
            spec = json.dumps({'protocol_version': 2, 'argv': ['kv', '--json', 'set', f'key-{i}', f'value-{i}']})
            processes.append(subprocess.Popen([str(management.JASH), '--exec-json', spec], cwd=self.directory,
                                             env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE))
        for process in processes:
            stdout, stderr = process.communicate(timeout=25)
            self.assertEqual(process.returncode, 0, (stdout, stderr))
            self.assertTrue(json.loads(stdout)['durable'])
        rows = self.report('kv', 'list')['entries']
        self.assertEqual({row['key']: row['value'] for row in rows}, {f'key-{i}': f'value-{i}' for i in range(12)})

    def test_kv_checked_publication_and_durability_faults(self):
        self.report('kv', 'set', 'key', 'old')
        store = self.directory / '.jash/kv.db'
        before = store.read_bytes()
        for fault in ('read', 'file_sync', 'file_close', 'rename', 'directory_sync'):
            with self.subTest(fault=fault):
                store.write_bytes(before)
                self.env.update(LD_PRELOAD=self.fault_library, JASH_TEST_MEMORY_ROOT=str(self.directory), JASH_TEST_MEMORY_FAULT=fault)
                try:
                    row, terminal, _ = self.native('kv', 'set', 'key', 'new', status=1)
                finally:
                    for key in ('LD_PRELOAD', 'JASH_TEST_MEMORY_ROOT', 'JASH_TEST_MEMORY_FAULT'):
                        self.env.pop(key, None)
                self.assertEqual(row['applied'], fault == 'directory_sync')
                self.assertFalse(row['durable'])
                self.assertIsNotNone(row['error'])
                self.assertEqual(terminal['state'], 2)
                self.assertEqual(store.read_bytes(), b'key\tnew\n' if fault == 'directory_sync' else before)
                self.assertFalse(list(store.parent.glob('.memory-*.tmp')))

    def test_resume_preserves_trailing_bytes_and_legacy_text_header(self):
        note = b'exact note\n\n\xff'
        row, _, _ = self.native('resume', 'set', note)
        expected = {'encoding': 'base64', 'data': base64.b64encode(note).decode()}
        self.assertEqual(row['note'], expected)
        self.assertEqual(self.report('resume')['note'], expected)
        store = self.directory / '.jash/resume.txt'
        store.write_bytes(b'100\n?\n' + os.fsencode(self.directory) + b'\nlegacy note\n')
        self.assertEqual(self.report('resume')['note'], 'legacy note')

    def test_result_schemas_require_observed_fields(self):
        for command in ('kv', 'intent', 'resume', 'goal'):
            with self.subTest(command=command):
                schema = json.loads((management.ROOT / 'schemas' / ('builtin-' + command + '-v1.schema.json')).read_text())
                validator = Draft202012Validator(schema)
                row = self.report(command)
                removed = dict(row)
                del removed['operation']
                self.assertFalse(validator.is_valid(removed))
                extra = dict(row, invented_success=True)
                self.assertFalse(validator.is_valid(extra))
                invalid_boolean = dict(row, applied='false')
                self.assertFalse(validator.is_valid(invalid_boolean))


if __name__ == '__main__':
    unittest.main(verbosity=2)
