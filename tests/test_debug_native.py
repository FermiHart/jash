#!/usr/bin/env python3
"""Debug observations and checked in-session profiler receipts."""
import base64
import copy
import json
import os
import shlex
import subprocess
import unittest
from jsonschema import Draft202012Validator
import test_management_builtins as management
from test_discovery_builtins import native_stream, project


class DebugNative(unittest.TestCase):
    run_cli = management.ManagementBuiltins.run_cli
    execute = management.ManagementBuiltins.execute

    def setUp(self):
        management.ManagementBuiltins.setUp(self)
        self.validators = {}
        for name in ('debug', 'debug-profile'):
            schema = json.loads((management.ROOT / f'schemas/builtin-{name}-v1.schema.json').read_text())
            Draft202012Validator.check_schema(schema)
            self.validators[name] = Draft202012Validator(schema)

    def validate(self, row):
        self.validators['debug-profile' if row['schema'] == 'jash.debug.profile.v1' else 'debug'].validate(row)
        return row

    def report(self, *arguments, status=0):
        return self.validate(json.loads(self.execute(['debug', '--json', *arguments], status=status).stdout))

    def native(self, *arguments, status=0, selector='--data-v2'):
        result = self.execute(['debug', selector, *arguments], status=status)
        rows, terminal = native_stream(result.stdout)
        self.assertEqual(terminal['status'], status)
        self.assertEqual(terminal['state'], 0 if not status else 2)
        self.assertEqual(terminal['errors'], int(bool(status)))
        self.assertEqual(terminal['logical_bytes'], 0)
        return [self.validate(project(row)) for row in rows], terminal, rows, result

    def test_parse_uses_exact_scanner_without_claiming_shell_parse(self):
        line = 'echo "one two"; x\t\'three four\''
        row = self.report('--parse', line)
        self.assertEqual([item['text'] for item in row['tokens']], ['echo', '"one two";', 'x', "'three four'"])
        self.assertEqual(row['tokens'][1]['byte_offset'], 5)
        self.assertFalse(row['executed'])
        self.assertFalse(row['single_quote_open']); self.assertFalse(row['double_quote_open'])
        typed, _, raw, _ = self.native('--parse', line)
        self.assertEqual(typed, [row])
        self.assertEqual(raw[0][1]['tokens'][1][0][1]['byte_offset'][0], 3)
        unbalanced = self.report('--parse', 'echo "unterminated')
        self.assertTrue(unbalanced['double_quote_open'])
        self.assertIn('not_shell_parser', unbalanced['analysis_scope'])
        self.assertEqual(self.report('--parse', '')['tokens'], [])

    def test_parse_preserves_binary_and_literal_format_arguments(self):
        value = b'echo binary-\xff'
        row = self.report('--parse', value)
        self.assertEqual(row['input'], {'encoding': 'base64', 'data': base64.b64encode(value).decode()})
        self.assertEqual(self.report('--parse', '--data')['tokens'][0]['text'], '--data')
        self.native('--parse', 'x ' * 4097, status=75)

    def test_locals_observe_values_unset_and_long_names_list(self):
        names = ['LOCAL_' + str(i) for i in range(180)]
        self.env['__LOCAL_VARS__'] = ' '.join(names + ['UNSET_LOCAL'])
        for index, name in enumerate(names):
            self.env[name] = 'exact --json ' + str(index)
        row = self.report('--locals')
        self.assertEqual(row['total'], 181)
        self.assertTrue(row['complete'])
        self.assertEqual(row['locals'][179], dict(name=names[179], set=True, value='exact --json 179'))
        self.assertEqual(row['locals'][-1], dict(name='UNSET_LOCAL', set=False, value=None))
        typed, _, _, _ = self.native('--locals')
        self.assertEqual(typed, [row])
        self.env['__LOCAL_VARS__'] = ' '.join('v' + str(i) for i in range(257))
        self.assertEqual(self.report('--locals', status=75)['error'], 'local_count_limit')

    def test_profile_executes_real_grouped_statements_in_current_state(self):
        script = self.directory / 'profile "literal".sh'
        script.write_text('export PROFILE_VALUE=retained\nif true; then\n echo block\nfi\n'
                          'echo "$PROFILE_VALUE" > result\nfalse\necho final\n')
        rows, terminal, raw, process = self.native('--profile', str(script))
        self.assertEqual(rows[0]['statements_total'], 5)
        self.assertEqual(rows[0]['command_output_destination'], 'stderr')
        statements = rows[1:-1]
        self.assertEqual([(row['first_line'], row['last_line']) for row in statements], [(1, 1), (2, 4), (5, 5), (6, 6), (7, 7)])
        self.assertEqual([row['command_status'] for row in statements], [0, 0, 0, 1, 0])
        self.assertEqual((self.directory / 'result').read_text(), 'retained\n')
        self.assertIn(b'block\n', process.stderr); self.assertIn(b'final\n', process.stderr)
        self.assertTrue(rows[-1]['complete']); self.assertEqual(rows[-1]['failed_commands'], 1)
        self.assertEqual(terminal['state'], 0)
        for row in raw[1:-1]:
            self.assertEqual(row[1]['duration_ns'][0], 6)
            self.assertGreaterEqual(row[1]['duration_ns'][1], 0)

    def test_profile_jsonl_is_semantic_and_restores_redirects(self):
        script = self.directory / '--json'
        script.write_text('echo redirected > inner-output\necho stderr-routed\n')
        result = self.execute(['debug', '--json', '--profile', '--', '--json'])
        rows = [self.validate(json.loads(line)) for line in result.stdout.splitlines() if line.strip()]
        self.assertEqual([row['kind'] for row in rows], ['header', 'statement', 'statement', 'summary'])
        self.assertEqual((self.directory / 'inner-output').read_text(), 'redirected\n')
        self.assertEqual(result.stderr, b'stderr-routed\n')
        after = self.run_cli('-c', 'debug --json --profile -- --json; echo --json restored')
        self.assertEqual(json.loads(after.stdout.splitlines()[-1]), ['restored'])

    def test_profile_prevalidates_entire_source_before_effects(self):
        path = self.directory / 'bad.sh'
        for source, error, status in [(b'touch unexpected\n' + b'x' * 4096, 'line_byte_limit', 75),
                                      (b'touch unexpected\n' + b'echo x\n' * 128, 'statement_limit', 75),
                                      (b'touch unexpected\nif true; then\necho x\n', 'unclosed_block', 2),
                                      (b'touch unexpected\0\n', 'embedded_nul_not_supported', 2)]:
            with self.subTest(error=error):
                path.write_bytes(source)
                self.assertEqual(self.report('--profile', str(path), status=status)['error'], error)
                self.assertFalse((self.directory / 'unexpected').exists())
        fifo = self.directory / 'fifo'; os.mkfifo(fifo)
        self.assertEqual(self.report('--profile', str(fifo), status=1)['error'], 'non_regular_input')

    def test_failed_admission_and_budget_prevent_profile_effects(self):
        script = self.directory / 'mutation.sh'; script.write_text('touch unexpected\n')
        for selector in ('--json', '--data-v2'):
            with open('/dev/full', 'wb') as sink:
                self.execute(['debug', selector, '--profile', str(script)], stdout=sink, status=125)
            self.assertFalse((self.directory / 'unexpected').exists())
        self.env['JASH_LLM_BUDGET'] = '1'
        value = json.loads(self.execute(['debug', '--json', '--profile', str(script)], status=75).stdout)
        self.assertEqual(value['schema'], 'jash.builtin.error.v1')
        self.assertFalse((self.directory / 'unexpected').exists())
        self.native('--profile', str(script), selector='--data')
        self.assertTrue((self.directory / 'unexpected').exists())

    def test_invalid_modes_and_interactive_trace_are_explicit(self):
        for arguments in [('--parse',), ('--locals', 'extra'), ('--profile', '--bad'), ('--data', '--locals')]:
            self.native(*arguments, status=2)
        row = self.report('--trace', 'missing.sh', status=2)
        self.assertEqual(row['error'], 'interactive_trace_requires_terminal_control')

    def test_native_aliases_bypass_presentation_and_schema_is_exact(self):
        self.env['JASH_LLM_BUDGET'] = '1'
        for selector in ('--data', '--data-v2'):
            rows, _, _, _ = self.native('--parse', 'echo safe', selector=selector)
            bad = copy.deepcopy(rows[0]); bad['executed'] = True
            self.assertFalse(self.validators['debug'].is_valid(bad))
        value = json.loads(self.execute(['debug', '--json', '--locals'], status=75).stdout)
        self.assertEqual(value['schema'], 'jash.builtin.error.v1')
        self.env.pop('JASH_LLM_BUDGET')
        script = self.directory / 'one.sh'; script.write_text('true\n')
        rows, _, _, _ = self.native('--profile', str(script))
        bad = copy.deepcopy(rows[1]); bad['duration_ns'] = -1
        self.assertFalse(self.validators['debug-profile'].is_valid(bad))


if __name__ == '__main__':
    unittest.main(verbosity=2)
