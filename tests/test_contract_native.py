#!/usr/bin/env python3
"""Literal command contracts observe bytes, failed predicates and capture errors."""
import base64
import copy
import errno
import json
import os
import sys
import unittest
from jsonschema import Draft202012Validator
import test_management_builtins as management
from test_discovery_builtins import native_stream, project


class ContractNative(unittest.TestCase):
    run_cli = management.ManagementBuiltins.run_cli
    execute = management.ManagementBuiltins.execute

    def setUp(self):
        management.ManagementBuiltins.setUp(self)
        schema = json.loads((management.ROOT / 'schemas/builtin-contract-v1.schema.json').read_text())
        Draft202012Validator.check_schema(schema)
        self.validator = Draft202012Validator(schema)

    def validate(self, row):
        self.validator.validate(row)
        return row

    def report(self, *arguments, status=0):
        return self.validate(json.loads(self.execute(['contract', '--json', *arguments], status=status).stdout))

    def native(self, *arguments, status=0, failed=False, selector='--data-v2'):
        result = self.execute(['contract', selector, *arguments], status=status)
        rows, terminal = native_stream(result.stdout)
        self.assertEqual(len(rows), 1)
        self.assertEqual((terminal['state'], terminal['status'], terminal['errors']),
                         (2 if failed else 0, status, int(failed)))
        self.assertEqual(terminal['logical_bytes'], 0)
        self.assertEqual(terminal['completeness'], 2 if failed else 1)
        return self.validate(project(rows[0])), terminal, rows[0], result

    def test_actual_nonzero_status_and_separate_binary_channels(self):
        source = 'import os,sys;os.write(1,b"prefix\\0needle\\xff");os.write(2,b"diagnostic");sys.exit(7)'
        args = ['--expect', 'rc', '7', '--expect', 'stdout-contains', 'needle',
                '--expect', 'stderr-contains', 'diagnostic', '--', sys.executable, '-c', source]
        value, _, raw, process = self.native(*args)
        self.assertTrue(value['matched']); self.assertTrue(value['complete'])
        self.assertEqual(value['execution']['command_status'], 7)
        self.assertEqual(base64.b64decode(value['execution']['stdout']['data']), b'prefix\0needle\xff')
        self.assertEqual(base64.b64decode(value['execution']['stderr']['data']), b'diagnostic')
        self.assertTrue(all(item['evaluated'] and item['passed'] for item in value['expectations']))
        self.assertEqual(raw[1]['execution'][1]['stdout'][0], 8)
        self.assertEqual(process.stderr, b'')
        self.assertEqual(self.report(*args), value)
        checked = json.loads(self.run_cli('-c', 'json --inspect-data', input=process.stdout).stdout)
        self.assertTrue(checked['terminal']['hash_verified'])

    def test_false_predicates_are_complete_and_all_checks_are_observed(self):
        value, _, _, _ = self.native('--expect', 'rc', '0', '--expect', 'stdout-empty',
                                     '--expect', 'stdout-matches', '^other$', '--', 'echo', 'hello', status=2)
        self.assertTrue(value['complete']); self.assertFalse(value['matched'])
        self.assertEqual(value['predicate_status'], 2)
        self.assertEqual([row['passed'] for row in value['expectations']], [True, False, False])
        self.assertIsNone(value['error'])
        signaled, _, _, _ = self.native('--expect', 'rc', '143', '--', sys.executable, '-c',
                                        'import os,signal;os.kill(os.getpid(),signal.SIGTERM)')
        self.assertEqual(signaled['execution']['command_status'], 143)

    def test_literal_argv_does_not_evaluate_shell_syntax_or_format_substrings(self):
        literal = 'one two; touch unexpected --json'
        value = self.report('--expect', 'stdout-contains', literal, '--', 'printf', '%s', literal)
        self.assertEqual(value['argv'], ['printf', '%s', literal])
        self.assertFalse((self.directory / 'unexpected').exists())
        empty = self.report('--expect', 'stdout-empty', '--expect', 'stderr-empty', '--', 'true')
        self.assertTrue(empty['matched'])

    def test_both_large_channels_are_drained_without_deadlock(self):
        source = 'import os;os.write(2,b"E"*131072);os.write(1,b"O"*131072)'
        value, _, _, _ = self.native('--expect', 'stdout-contains', 'OOOO', '--expect',
                                     'stderr-contains', 'EEEE', '--', sys.executable, '-c', source)
        self.assertEqual(value['execution']['stdout_bytes_seen'], 131072)
        self.assertEqual(value['execution']['stderr_bytes_seen'], 131072)
        self.assertTrue(value['execution']['complete'])

    def test_capture_overflow_keeps_prefix_but_never_evaluates_missing_suffix(self):
        source = 'import os;open("effect","w").write("applied");os.write(1,b"x"*1024+b"needle");os.write(2,b"e"*2048)'
        value, _, _, _ = self.native('--capture-bytes', '64', '--expect', 'stdout-contains', 'needle',
                                     '--', sys.executable, '-c', source, status=75, failed=True)
        self.assertEqual(value['error'], 'output_capture_limit')
        self.assertFalse(value['complete']); self.assertIsNone(value['matched'])
        self.assertTrue(value['effects_may_have_applied'])
        self.assertEqual((self.directory / 'effect').read_text(), 'applied')
        self.assertEqual(len(base64.b64decode(value['execution']['stdout']['data'])), 64)
        self.assertEqual(value['execution']['stdout_bytes_seen'], 1030)
        self.assertTrue(value['execution']['stdout_truncated']); self.assertTrue(value['execution']['stderr_truncated'])
        self.assertFalse(value['expectations'][0]['evaluated'])

    def test_invalid_regex_grammar_and_missing_program_have_distinct_errors(self):
        for args in [('--expect', 'rc', 'bad'), ('--expect', 'stdout-matches', '['),
                     ('--expect', 'unknown'), ('--capture-bytes', '0'), ('--unknown',)]:
            self.native(*args, '--', 'touch', 'unexpected', status=2, failed=True)
            self.assertFalse((self.directory / 'unexpected').exists())
        missing, _, _, _ = self.native('--expect', 'rc', '127', '--', '/nonexistent-jash-contract-fixture',
                                       status=127, failed=True)
        self.assertEqual(missing['execution']['adapter_errno'], errno.ENOENT)
        self.assertIsNone(missing['matched'])

    def test_regex_binary_input_fails_as_observation_not_mismatch(self):
        value, _, _, _ = self.native('--expect', 'stdout-matches', '^prefix', '--', sys.executable, '-c',
                                     'import os;os.write(1,b"prefix\\0suffix")', status=1, failed=True)
        self.assertEqual(value['error'], 'regex_binary_output_not_supported')
        self.assertTrue(value['execution']['complete'])
        self.assertFalse(value['expectations'][0]['evaluated'])

    def test_delivery_and_budget_admission_precede_command_effects(self):
        for selector in ('--json', '--data'):
            with open('/dev/full', 'wb') as sink:
                self.execute(['contract', selector, '--', 'touch', 'unexpected'], stdout=sink, status=125)
            self.assertFalse((self.directory / 'unexpected').exists())
        self.env['JASH_LLM_BUDGET'] = '1'
        value = json.loads(self.execute(['contract', '--json', '--', 'touch', 'unexpected'], status=75).stdout)
        self.assertEqual(value['schema'], 'jash.builtin.error.v1')
        self.assertFalse((self.directory / 'unexpected').exists())
        for selector in ('--data', '--data-v2'):
            self.native('--expect', 'rc', '0', '--', 'true', selector=selector)
        self.env['JASH_LLM_BUDGET'] = '4096'
        self.assertTrue(self.report('--capture-bytes', '32', '--expect', 'stdout-contains', 'yes', '--', 'echo', 'yes')['matched'])

    def test_repeated_capture_restores_shell_descriptors_and_schema_is_strict(self):
        script = '; '.join(['contract --json --capture-bytes 32 --expect rc 0 -- true'] * 12 + ['echo --json final'])
        result = self.run_cli('-c', script)
        rows = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
        self.assertEqual(rows[-1], ['final'])
        self.assertEqual(len(rows), 13)
        for value in rows[:-1]: self.validate(value)
        wrong = copy.deepcopy(rows[0]); wrong['execution']['stdout_truncated'] = True
        self.assertFalse(self.validator.is_valid(wrong))
        wrong = copy.deepcopy(rows[0]); wrong['complete'] = False
        self.assertFalse(self.validator.is_valid(wrong))


if __name__ == '__main__':
    unittest.main(verbosity=2)
