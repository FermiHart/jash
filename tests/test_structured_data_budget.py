"""Native resource limits and the presentation governor are distinct contracts."""
import json
import shlex
import sys
import unittest

import test_structured_models as models
import test_agent_mutations_data as mutations
from test_structured_pipe import native_rows
from contract_schema_fixture import validate_contract


class StructuredDataBudget(unittest.TestCase):
    runj = models.StructuredModels.runj
    prepare = mutations.MutationData.prepare

    def setUp(self):
        models.StructuredModels.setUp(self)
        self.env['JASH_LLM_BUDGET'] = '1'

    def test_native_producers_and_explicit_bridges_keep_complete_streams(self):
        for mode in ('--data', '--data-v2'):
            for argv, data, records in ((['echo', mode, 'exact bytes'], b'', False),
                    (['xargs', '--plan', mode, '-n', '1', 'echo'], b'one two', True),
                    (['calc', mode, '9007199254740993/3'], b'', True),
                    (['cost', mode, '--dry-run', 'literal', 'argument'], b'', True),
                    (['agent-guide', mode], b'', True)):
                with self.subTest(argv=argv):
                    result = self.runj(argv, data=data)
                    rows, terminal = native_rows(result.stdout, records=records)
                    self.assertTrue(rows)
                    self.assertEqual(terminal['state'], 0)
        wire = self.runj(['json', '--to-data'], data=b'{"x":9007199254740993}').stdout
        rows, terminal = native_rows(wire)
        self.assertEqual(rows[0][0]['x'][1], 9007199254740993)
        self.assertEqual(terminal['state'], 0)

    def test_all_eight_native_operators_keep_their_streams(self):
        wire = self.runj(['json', '--to-data', '--jsonl'], data=b'{"n":1,"k":"a"}\n{"n":3,"k":"b"}\n').stdout
        for mode in ('--data', '--data-v2'):
            for operation, arguments in (('where', ['n>0']), ('pick', ['n']), ('sortby', ['n']),
                    ('count', []), ('sum', ['n']), ('avg', ['n']), ('uniqby', ['k']), ('describe', [])):
                with self.subTest(operation=operation, mode=mode):
                    rows, terminal = native_rows(self.runj([operation, mode, *arguments], data=wire).stdout)
                    self.assertTrue(rows)
                    self.assertEqual(terminal['state'], 0)

    def test_all_ten_mutators_keep_actual_receipts(self):
        for operation in ('cp', 'mv', 'rm', 'touch', 'mkdir', 'rmdir', 'ln', 'chmod', 'chown', 'sed'):
            arguments, source, destination = self.prepare(operation, '-budget')
            with self.subTest(operation=operation):
                rows, terminal = native_rows(self.runj([operation, '--data', *arguments]).stdout)
                self.assertEqual(terminal['state'], 0)
                self.assertEqual(rows[-1][0]['kind'], (10, b'summary'))
                if operation in ('rm', 'rmdir'):
                    self.assertFalse((self.work / destination).exists())
                else:
                    self.assertTrue((self.work / destination).exists())

    def test_operational_json_receipts_explicitly_bypass_presentation_budget(self):
        # A receipt is emitted after the observed effect. Applying the prose
        # cap at that point would hide operational state, so this documented
        # mode deliberately has no presentation governor.
        for operation in ('cp', 'mv', 'rm', 'touch', 'mkdir', 'rmdir', 'ln', 'chmod', 'chown', 'sed'):
            arguments, _, destination = self.prepare(operation, '-json-budget')
            with self.subTest(operation=operation):
                result = self.runj([operation, '--json', *arguments])
                value = json.loads(result.stdout)
                validate_contract('mutation', value)
                self.assertEqual(value['exit_code'], 0)
                self.assertEqual(value['counts']['completed'], 1)
                if operation in ('rm', 'rmdir'):
                    self.assertFalse((self.work / destination).exists())
                else:
                    self.assertTrue((self.work / destination).exists())
        failed = self.runj([], code='touch --json no-effect > /dev/full', ok=False)
        self.assertEqual(failed.returncode, 125)
        self.assertFalse((self.work / 'no-effect').exists())

    def test_native_execution_still_requires_real_sink_admission(self):
        child = [sys.executable, '-c', 'from pathlib import Path; Path("effect").touch(); print("abcd")']
        failed = self.runj([], code=shlex.join(['cost', '--data', *child]) + ' > /dev/full', ok=False)
        self.assertEqual(failed.returncode, 125)
        self.assertFalse((self.work / 'effect').exists())
        result = self.runj(['cost', '--data-v2', *child])
        rows, terminal = native_rows(result.stdout)
        self.assertEqual(terminal['state'], 0)
        self.assertEqual(rows[0][0]['bytes'], (4, 5))
        self.assertTrue((self.work / 'effect').exists())

    def test_native_resource_and_semantic_limits_still_reject_input(self):
        overlong = json.dumps({'x': 'x' * 65536}).encode() + b'\n'
        refused = self.runj(['pick', '--to-data', 'x'], data=overlong, ok=False)
        self.assertNotEqual(refused.returncode, 0)
        self.assertEqual(refused.stdout, b'')
        for value in (b'1.5', b'18446744073709551616'):
            refused = self.runj(['json', '--to-data'], data=value, ok=False)
            self.assertNotEqual(refused.returncode, 0)
            self.assertEqual(refused.stdout, b'')
        wire = self.runj(['json', '--to-data'], data=b'{"x":1}').stdout
        refused = self.runj(['pick', '--data', 'x'], data=wire[:-1], ok=False)
        self.assertNotEqual(refused.returncode, 0)
        self.assertEqual(refused.stdout, b'')


if __name__ == '__main__':
    unittest.main()
