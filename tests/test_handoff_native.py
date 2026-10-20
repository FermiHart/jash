#!/usr/bin/env python3
"""Session summaries retain source limits, failure evidence, and real Git state."""
import base64
import json
import os
import subprocess
import unittest

import test_management_builtins as management
from test_discovery_builtins import native_stream, project
from jsonschema import Draft202012Validator


class HandoffNative(unittest.TestCase):
    run_cli = management.ManagementBuiltins.run_cli
    execute = management.ManagementBuiltins.execute

    def setUp(self):
        management.ManagementBuiltins.setUp(self)
        (self.directory / '.jash').mkdir(mode=0o700)
        (self.directory / '.jash_sessions').mkdir(mode=0o700)
        self.env['JASH_SESSION'] = 'handoff_fixture'
        self.validator = Draft202012Validator(json.loads((management.ROOT / 'schemas/builtin-handoff-v1.schema.json').read_text()))

    def records(self, data):
        file = self.directory / '.jash_sessions/session_handoff_fixture.jash'
        file.write_bytes(data)
        return file

    def report(self, *args, status=0):
        value = json.loads(self.execute(['handoff', '--json', *args], status=status).stdout)
        self.validator.validate(value); return value

    def native(self, *args, status=0):
        rows, terminal = native_stream(self.execute(['handoff', '--data', *args], status=status).stdout)
        self.assertEqual(len(rows), 1)
        value = project(rows[0]); self.validator.validate(value)
        self.assertEqual(terminal['status'], status)
        return value, terminal, rows[0]

    def git(self, *args):
        result = subprocess.run(['git', *args], cwd=self.directory, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def test_session_counts_exact_failures_and_source_fields(self):
        self.records(b'100|0|12|echo ok\n101|2|9007199254740993|build target one\n102|3|9|build target two\n103|1|1|test suite broken\xff\n')
        value, terminal, raw = self.native('--no-git')
        self.assertEqual((value['commands_total'], value['failed_commands']), (4, 3))
        self.assertEqual((value['first_timestamp'], value['last_timestamp']), (100, 103))
        self.assertEqual(value['failure_groups_total'], 2)
        self.assertEqual(value['failures'][0]['count'], 2)
        self.assertEqual(value['failures'][0]['last_status'], 3)
        self.assertEqual(base64.b64decode(value['failures'][1]['last_command_recorded']['data']), b'test suite broken\xff')
        self.assertTrue(value['commands_source']['read_complete'])
        self.assertFalse(value['causation_established'])
        self.assertIsNone(value['git'])
        self.assertEqual(terminal['state'], 0)
        self.assertEqual(raw[1]['commands_total'][0], 3)

    def test_latest_file_change_per_exact_session_and_pagination(self):
        self.records(b'100|1|1|build one\n101|1|1|build two\n')
        (self.directory / '.jash/fsjournal.tsv').write_bytes(b'100\thandoff_fixture\t+\ta file\n101\tother\t-\tother file\n102\thandoff_fixture\t~\ta file\n103\thandoff_fixture\t+\tz\twith tab\n')
        value = self.report('--no-git', '--limit', '1')
        self.assertEqual(value['files_total'], 2)
        self.assertEqual(value['files'][0]['path_recorded'], 'a file')
        self.assertEqual(value['files'][0]['kind'], '~')
        self.assertEqual((value['next_file_offset'], value['next_failure_offset']), (1, 1))
        second = self.report('--no-git', '--offset', '1', '--limit', '1')
        self.assertEqual(second['files'][0]['path_recorded'], 'z\twith tab')
        self.assertFalse(second['truncated'])
        self.assertEqual(second['files_scope'], 'exact_session_field')

    def test_missing_history_and_session_normalization_are_explicit(self):
        value = self.report('--no-git')
        self.assertTrue(value['commands_source']['selected'])
        self.assertFalse(value['commands_source']['present'])
        self.assertEqual(value['commands_total'], 0)
        self.env['JASH_SESSION'] = 'ambiguous/name'
        value = self.report('--no-git')
        self.assertTrue(value['session_identity_lossy'])
        self.assertEqual(value['session_key'], 'ambiguous_name')
        self.env.pop('JASH_SESSION')
        value = self.report('--no-git')
        self.assertFalse(value['commands_source']['selected'])
        self.assertEqual(value['files_scope'], 'all_retained_sessions')

    def test_malformed_or_unsafe_sources_do_not_report_empty_success(self):
        self.records(b'100|0|1|valid\nbroken line\n')
        value, terminal, _ = self.native('--no-git', status=1)
        self.assertFalse(value['commands_source']['read_complete'])
        self.assertIsNone(value['commands_total'])
        self.assertEqual(value['error'], 'invalid_store')
        self.assertEqual(terminal['state'], 2)
        self.records(b'100|0|1|valid\n')
        (self.directory / '.jash/fsjournal.tsv').symlink_to(self.directory / 'missing')
        value = self.report('--no-git', status=1)
        self.assertFalse(value['fs_source']['read_complete'])
        self.assertIsNone(value['files_total'])

    def test_record_writer_boundary_is_declared(self):
        prefix = b'100|2|1|'
        self.records(prefix + b'x' * (2040 - len(prefix)) + b'\n')
        value = self.report('--no-git')
        self.assertTrue(value['failures'][0]['last_record_maybe_truncated'])
        self.assertIn('2040', value['commands_source']['record_fidelity'])

    def test_real_git_branch_dirty_path_and_latest_commit(self):
        self.git('init', '-q')
        self.git('config', 'user.name', 'Synthetic Fixture')
        self.git('config', 'user.email', 'fixture@example.invalid')
        (self.directory / 'tracked').write_text('one\n')
        self.git('add', 'tracked'); self.git('commit', '-qm', 'synthetic initial')
        expected_head = self.git('rev-parse', 'HEAD').decode().strip()
        (self.directory / 'with newline\nquote"').write_text('untracked\n')
        value = self.report()
        git = value['git']
        self.assertTrue(git['status']['available'])
        self.assertTrue(git['status']['payload']['git']['dirty'])
        self.assertIn('with newline\nquote"', [f['path'] for f in git['status']['payload']['files']])
        self.assertEqual(git['latest_commit']['payload']['commits'][0]['hash'], expected_head)
        self.assertFalse(git['cryptographically_verified'])
        self.assertEqual(git['consistency'], 'independent-subprocess-observations')

    def test_git_failure_is_unavailable_not_clean(self):
        value, terminal, _ = self.native(status=1)
        self.assertEqual(value['error'], 'git_observation_failed')
        self.assertFalse(value['git']['status']['available'])
        self.assertIsNone(value['git']['status']['payload'])
        self.assertNotEqual(value['git']['status']['command_status'], 0)
        self.assertIsNone(value['git']['latest_commit'])
        self.assertEqual(terminal['state'], 2)

    def test_arguments_and_schema_negative_controls(self):
        for args in [('--limit', '0'), ('--offset', '-1'), ('--limit', '257'), ('--bogus',), ('--limit', '1', '--limit', '2')]:
            self.assertEqual(self.report(*args, status=2)['error'], 'invalid_arguments')
        value = self.report('--no-git')
        for mutate in [lambda v: v.pop('files_scope'), lambda v: v.update(causation_established=True), lambda v: v.update(commands_total='0')]:
            broken = json.loads(json.dumps(value)); mutate(broken)
            self.assertTrue(list(self.validator.iter_errors(broken)))


if __name__ == '__main__':
    unittest.main(verbosity=2)
