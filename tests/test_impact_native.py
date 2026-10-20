#!/usr/bin/env python3
"""Observed retained correlation, checked execution and honest failure outcomes."""
import base64
import copy
import json
import os
import unittest
from jsonschema import Draft202012Validator
import test_management_builtins as management
from test_discovery_builtins import native_stream, project


class ImpactNative(unittest.TestCase):
    run_cli = management.ManagementBuiltins.run_cli
    execute = management.ManagementBuiltins.execute

    def setUp(self):
        management.ManagementBuiltins.setUp(self)
        schema = json.loads((management.ROOT / 'schemas/builtin-impact-v1.schema.json').read_text())
        Draft202012Validator.check_schema(schema)
        self.validator = Draft202012Validator(schema)

    def validate(self, value):
        self.validator.validate(value)
        return value

    def report(self, *args, status=0):
        result = self.execute(['impact', '--json', *args], status=status)
        return [self.validate(json.loads(row)) for row in result.stdout.splitlines() if row.strip()], result

    def native(self, *args, status=0, failed=False, selector='--data-v2'):
        result = self.execute(['impact', selector, *args], status=status)
        rows, end = native_stream(result.stdout)
        self.assertEqual((end['state'], end['status'], end['errors']),
                         (2 if failed else 0, status, int(failed)))
        self.assertEqual(end['logical_bytes'], 0)
        return [self.validate(project(row)) for row in rows], end, result

    def history(self, rows, changes=None):
        directory = self.directory / '.jash_sessions'
        directory.mkdir(mode=0o700, exist_ok=True)
        payload = b''.join(str(timestamp).encode() + b'|' + str(status).encode() + b'|500|' +
                           (command if isinstance(command, bytes) else command.encode()) + b'\n'
                           for timestamp, status, command in rows)
        (directory / 'session_fixture.jash').write_bytes(payload)
        store = self.directory / '.jash'
        store.mkdir(mode=0o700, exist_ok=True)
        (store / 'fsjournal.tsv').write_bytes(b''.join(
            str(timestamp).encode() + b'\tsynthetic\t~\t' +
            (path if isinstance(path, bytes) else path.encode()) + b'\n'
            for timestamp, path in ([(100, 'src/core.c')] if changes is None else changes)))

    def runnable(self):
        rows = []
        for name, status in [('a', 0), ('b', 7), ('c', 0)]:
            path = self.directory / ('test_' + name + '.sh')
            path.write_text('echo ' + name + '-output\necho applied > ' + name + '-effect\nexit ' + str(status) + '\n')
            rows.append((101, 0, 'sh test_' + name + '.sh'))
        self.history(rows)

    def test_missing_history_is_cold_and_observation_does_not_create_stores(self):
        # Ordinary process startup creates metrics/config files even for true.
        # Compare that baseline; the provider must not create either source.
        self.execute(['true'])
        before = {path.relative_to(self.directory) for path in self.directory.rglob('*')}
        rows, _, _ = self.native('src/core.c')
        value = rows[0]
        self.assertTrue(value['complete']); self.assertTrue(value['cold'])
        self.assertFalse(value['sessions_source']['directory_present'])
        self.assertFalse(value['fs_source']['present'])
        self.assertEqual(value['recommendations'], [])
        self.assertFalse((self.directory / '.jash_sessions').exists())
        self.assertFalse((self.directory / '.jash/fsjournal.tsv').exists())
        self.assertEqual({path.relative_to(self.directory) for path in self.directory.rglob('*')}, before)

    def test_chronological_scoring_shared_weights_and_exact_timestamp_bounds(self):
        self.history([(130, 0, 'sh test_a.sh'), (110, 1, 'sh test_a.sh'),
                      (120, 0, 'sh test_b.sh'), (1901, 0, 'sh test_outside.sh')])
        value = self.report('core.c')[0][0]
        self.assertEqual([(r['command_recorded'], r['score']) for r in value['recommendations']],
                         [('sh test_a.sh', 9), ('sh test_b.sh', 2)])
        self.assertEqual(value['touches'], 1)
        self.assertEqual((value['known_test_commands'], value['test_runs']), (3, 4))
        self.assertEqual(value['analysis_scope'], 'retained_temporal_correlation_not_dependency_or_causality_proof')
        self.assertEqual(self.native('core.c')[0][0], value)
        maximum = 2**64 - 1
        self.history([(maximum - 5, 0, 'sh test_max.sh')], [(maximum - 10, 'src/core.c')])
        self.assertEqual(self.native('core.c')[0][0]['recommendations'][0]['score'], 2)

    def test_stable_ties_pagination_and_nonmatching_path_boundary(self):
        self.history([(101, 0, 'sh test_' + name + '.sh') for name in 'gfedcba'])
        first = self.report('--limit', '2', 'src/core.c')[0][0]
        second = self.native('--offset', '2', '--limit', '2', 'src/core.c')[0][0]
        self.assertEqual([r['command_recorded'] for r in first['recommendations']], ['sh test_a.sh', 'sh test_b.sh'])
        self.assertEqual([r['rank'] for r in second['recommendations']], [3, 4])
        self.assertEqual((first['ranked_total'], first['next_offset'], first['truncated']), (7, 2, True))
        self.assertTrue(self.report('ore.c')[0][0]['cold'])
        self.assertEqual(self.report('--offset', '64', 'src/core.c')[0][0]['recommendations'], [])

    def test_literal_recorded_bytes_and_pipe_are_preserved_without_execution(self):
        command = b'sh test_bytes.sh --json | literal-\xff'
        self.history([(101, 0, command)], [(100, b'path-\xff')])
        value = self.native(b'path-\xff')[0][0]
        self.assertEqual(base64.b64decode(value['recommendations'][0]['command_recorded']['data']), command)
        self.assertEqual(base64.b64decode(value['files'][0]['data']), b'path-\xff')
        self.assertEqual(self.report(b'path-\xff')[0][0], value)

    def test_malformed_or_unsafe_history_never_becomes_cold_success(self):
        self.runnable()
        source = self.directory / '.jash_sessions/session_fixture.jash'
        for payload in (b'broken\n', b'101|0|1|sh test_a.sh', b'101|0|1|sh test_a.sh\0\n',
                        b'18446744073709551616|0|1|sh test_a.sh\n'):
            source.write_bytes(payload)
            value = self.native('--run', 'src/core.c', status=1, failed=True)[0][0]
            self.assertFalse(value['complete']); self.assertIsNone(value['cold'])
            self.assertFalse((self.directory / 'a-effect').exists())
        source.unlink(); source.symlink_to(self.directory / 'test_a.sh')
        value = self.report('--run', 'src/core.c', status=1)[0][0]
        self.assertFalse(value['sessions_source']['read_complete'])
        self.assertFalse((self.directory / 'a-effect').exists())

    def test_limits_refuse_selected_command_truncation_and_excess_test_catalogue(self):
        for rows in ([(101, 0, 'sh test_' + 'x'*512)],
                     [(101, 0, 'sh test_' + str(i) + '.sh') for i in range(65)]):
            self.history(rows)
            value = self.native('--run', 'src/core.c', status=75, failed=True)[0][0]
            self.assertFalse(value['complete']); self.assertEqual(value['error'], 'store_limit')
            self.assertIsNone(value['ranked_total'])
        source = self.directory / '.jash_sessions/session_fixture.jash'
        source.write_bytes(b'x' * (4*1024*1024 + 1))
        value = self.report('src/core.c', status=75)[0][0]
        self.assertFalse(value['complete'])

    def test_run_stops_after_real_nonzero_status_and_keeps_structured_stdout(self):
        self.runnable()
        rows, end, result = self.native('--run', 'src/core.c', status=7)
        self.assertEqual([row['kind'] for row in rows], ['analysis', 'execution', 'execution', 'summary'])
        self.assertEqual([row['command_status'] for row in rows[1:3]], [0, 7])
        self.assertEqual(rows[-1]['planned'], 3); self.assertEqual(rows[-1]['executed'], 2)
        self.assertTrue(rows[-1]['complete']); self.assertEqual(rows[-1]['stopped_reason'], 'first_nonzero_test_status')
        self.assertTrue((self.directory / 'a-effect').exists()); self.assertTrue((self.directory / 'b-effect').exists())
        self.assertFalse((self.directory / 'c-effect').exists())
        self.assertIn(b'a-output', result.stderr); self.assertIn(b'b-output', result.stderr)
        json_rows, _ = self.report('--run', 'src/core.c', status=7)
        self.assertEqual(json_rows, rows)

    def test_delivery_budget_and_invalid_arguments_precede_execution(self):
        self.runnable()
        for selector in ('--json', '--data'):
            with open('/dev/full', 'wb') as sink:
                self.execute(['impact', selector, '--run', 'src/core.c'], stdout=sink, status=125)
            self.assertFalse((self.directory / 'a-effect').exists())
        for args in [('--bad',), ('--limit', '0'), ('--offset', '65'), ('--limit', '1', '--limit', '2')]:
            self.native('--run', *args, 'src/core.c', status=2, failed=True)
            self.assertFalse((self.directory / 'a-effect').exists())
        self.env['JASH_LLM_BUDGET'] = '1'
        budget = json.loads(self.execute(['impact', '--json', '--run', 'src/core.c'], status=75).stdout)
        self.assertEqual(budget['schema'], 'jash.builtin.error.v1')
        self.assertFalse((self.directory / 'a-effect').exists())
        for selector in ('--data', '--data-v2'):
            self.native('src/core.c', selector=selector)

    def test_double_dash_preserves_option_named_path_and_invalid_fsjournal_fails(self):
        self.history([(101, 0, 'sh test_a.sh')], [(100, '--run')])
        value = self.report('--', '--run')[0][0]
        self.assertEqual(value['files'], ['--run']); self.assertFalse(value['cold'])
        (self.directory / '.jash/fsjournal.tsv').write_bytes(b'bad\n')
        value = self.native('--', '--run', status=1, failed=True)[0][0]
        self.assertFalse(value['fs_source']['read_complete']); self.assertIsNone(value['cold'])

    def test_schema_requires_sources_and_rejects_false_completion(self):
        self.history([(101, 0, 'sh test_a.sh')])
        value = self.report('src/core.c')[0][0]
        missing = copy.deepcopy(value); del missing['sessions_source']['read_complete']
        self.assertFalse(self.validator.is_valid(missing))
        wrong = copy.deepcopy(value); wrong['sessions_source']['read_complete'] = False
        self.assertFalse(self.validator.is_valid(wrong))
        wrong = copy.deepcopy(value); wrong['complete'] = False
        self.assertFalse(self.validator.is_valid(wrong))


if __name__ == '__main__':
    unittest.main(verbosity=2)
