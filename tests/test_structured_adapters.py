#!/usr/bin/env python3
"""B24: explicit native conversion, exact argv and effect-free plans."""
import base64
import copy
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

from test_structured_pipe import native_projection, native_rows
from semantic_schema_fixture import semantic_validator

ROOT = Path(__file__).resolve().parents[1]
JASH = Path(os.environ.get('JASH', ROOT / 'build/jash')).resolve()


class StructuredAdapters(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix='jash-adapter-contract-')
        self.addCleanup(tmp.cleanup)
        self.work = Path(tmp.name) / 'work'
        self.work.mkdir()
        home = Path(tmp.name) / 'home'
        home.mkdir()
        self.env = {k: v for k, v in os.environ.items() if not k.startswith('JASH_')}
        self.env.update(HOME=str(home), JASH_SIGIL='0', JASH_TIMELINE='0',
                        JASH_MEMO='0', JASH_NUDGE='0', JASH_ADOPT='0', JASH_BRIEF='0',
                        JASH_LOOPGUARD='off', JASH_WORLD='0', JASH_PREFETCH='0')

    def runj(self, *argv, data=b'', ok=True, code=None):
        result = subprocess.run([str(JASH), '-c', code or shlex.join(argv)], input=data,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                cwd=self.work, env=self.env, timeout=15)
        if ok:
            self.assertEqual(result.returncode, 0, (argv, result.stderr, result.stdout[:200]))
        return result

    def test_echo_json_preserves_long_values_and_bytes(self):
        long = 'x' * 9000 + '"\\\n∞😀'
        raw = os.fsdecode(b'raw-\xff\x80')
        result = json.loads(self.runj('echo', '--json', '', long, raw).stdout)
        semantic_validator('echo').validate(result)
        self.assertEqual(result[:2], ['', long])
        self.assertEqual(base64.b64decode(result[2]['data']), os.fsencode(raw))

    def test_echo_native_is_one_list_and_preserves_native_bytes(self):
        for selector in ('--data', '--data-v2'):
            wire = self.runj('echo', selector, '-e', '', 'literal\\n', os.fsdecode(b'\xff')).stdout
            rows, terminal = native_rows(wire, records=False)
            semantic_validator('echo').validate(native_projection(rows[0][0]))
            self.assertEqual(rows[0][0], (11, [(7, b''), (7, b'literal\\n'), (8, b'\xff')]))
            self.assertEqual(terminal['state'], 0)
            self.assertEqual(len(rows), 1)
            projection = self.runj('json', '--from-data', data=wire)
            self.assertEqual(json.loads(projection.stdout)[1], 'literal\\n')

    def test_echo_selector_boundaries_and_checked_delivery(self):
        self.assertEqual(self.runj('echo', 'literal', '--data').stdout, b'literal --data\n')
        result = self.runj('echo', '--json', '--data', 'x', ok=False)
        self.assertNotEqual(result.returncode, 0)
        for flag in ('--json', '--data'):
            result = self.runj(ok=False, code=f'echo {flag} hello > /dev/full')
            self.assertEqual(result.returncode, 125)

    def test_json_document_bridge_keeps_array_scalar_and_exact_integers(self):
        values = [None, True, 9007199254740993, 18446744073709551615,
                  -9223372036854775808, 'NUL\0∞😀', [True, None, {'nested': [1, 'x']}]]
        for value in values:
            wire = self.runj('json', '--to-data', data=json.dumps(value).encode()).stdout
            rows, terminal = native_rows(wire, records=False)
            self.assertEqual(len(rows), 1)
            self.assertEqual(terminal['state'], 0)
            self.assertEqual(json.loads(self.runj('json', '--from-data', data=wire).stdout), value)

    def test_jsonl_bridge_preserves_cardinality_and_tag_objects(self):
        values = [{'x': 1}, {'x': None}, {'type': 'bytes', 'encoding': 'base64', 'data': 'eA=='}]
        data = b'\n' + b'\n'.join(json.dumps(x).encode() for x in values) + b'\n'
        wire = self.runj('json', '--to-data', '--jsonl', data=data).stdout
        rows, _ = native_rows(wire)
        self.assertEqual(len(rows), 3)
        self.assertEqual([json.loads(x) for x in self.runj('json', '--from-data', data=wire).stdout.splitlines()], values)

    def test_json_bridge_rejects_late_bad_rows_and_unsupported_native_numbers(self):
        for data in (b'{"x":1}\n{bad}', b'{"x":1}\n{"x":1,"x":2}', b'{"x":1}\n{"x":1e999}'):
            result = self.runj('json', '--to-data', '--jsonl', data=data, ok=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b'')
        for data in (b'0.5', b'1e3', b'18446744073709551616', b'-9223372036854775809'):
            result = self.runj('json', '--to-data', data=data, ok=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b'')

    def test_json_bridge_refuses_corrupt_and_truncated_streams(self):
        wire = self.runj('json', '--to-data', data=b'{"x":1}').stdout
        for malformed in (wire[:-1], b'JTV1' + wire[4:], wire + b'x'):
            result = self.runj('json', '--from-data', data=malformed, ok=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b'')

    def test_xargs_plans_are_exact_and_never_execute(self):
        fixed = 'fixed space; $(touch injection)'
        values = [b'one two', b'quote\'\nline', b'\xff']
        data = b'\0'.join(values) + b'\0'
        result = self.runj('xargs', '--plan', '--json', '-0', '-n', '1', 'touch', fixed, data=data)
        plan = json.loads(result.stdout)
        semantic_validator('xargs-plan').validate(plan)
        self.assertEqual(plan[0]['argv'], ['touch', fixed, 'one two'])
        self.assertEqual(base64.b64decode(plan[2]['argv'][2]['data']), b'\xff')
        self.assertEqual(list(self.work.iterdir()), [])
        wire = self.runj('xargs', '--plan', '--data', '-0', '-n1', 'touch', fixed, data=data).stdout
        rows, terminal = native_rows(wire)
        for row, _ in rows:
            semantic_validator('xargs-plan-row').validate(native_projection((12, row)))
        self.assertEqual(rows[2][0]['argv'][1][-1], (8, b'\xff'))
        self.assertEqual(terminal['state'], 0)
        self.assertEqual(list(self.work.iterdir()), [])

    def test_xargs_format_without_plan_is_rejected_before_effect(self):
        for flag in ('--json', '--data', '--data-v2'):
            result = self.runj('xargs', flag, 'touch', data=b'marker', ok=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((self.work / 'marker').exists())

    def test_xargs_nul_replacement_precedence_and_exact_execution_argv(self):
        values = [b'one\ntwo', b'with space']
        plan = json.loads(self.runj('xargs', '--plan', '-0', '-I{}', 'echo', 'pre{}post',
                                   data=b'\0'.join(values) + b'\0').stdout)
        semantic_validator('xargs-plan').validate(plan)
        self.assertEqual([x['argv'][1] for x in plan], ['preone\ntwopost', 'prewith spacepost'])
        output = self.runj('xargs', '-0', '/usr/bin/printf', '[%s]\\n', 'fixed space',
                           data=b'one two\0').stdout
        self.assertEqual(output, b'[fixed space]\n[one two]\n')

    def test_xargs_malformed_input_is_not_a_partial_plan_or_effect(self):
        for data in (b'good "unterminated', b"good 'unterminated", b'good dangling\\', b'good\0lost'):
            result = self.runj('xargs', '--plan', 'touch', data=data, ok=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b'')
        for size in ('no', '0', '-1', '1tail', '999999999999999999999999999999'):
            result = self.runj('xargs', '--plan', '-n', size, data=b'item', ok=False)
            self.assertNotEqual(result.returncode, 0)

    def test_xargs_plan_sink_failure_and_operand_selector(self):
        result = self.runj(ok=False, code='xargs --plan echo > /dev/full', data=b'item')
        self.assertEqual(result.returncode, 125)
        result = self.runj('xargs', 'echo', '--plan', data=b'item')
        self.assertEqual(result.stdout, b'--plan item\n')

    def test_published_adapters_and_projection_schemas_reject_wrong_shapes(self):
        echo = json.loads(self.runj('echo', '--json', 'one').stdout)
        semantic_validator('echo').validate(echo)
        self.assertFalse(semantic_validator('echo').is_valid([True]))
        self.assertFalse(semantic_validator('echo').is_valid([{'encoding': 'base64', 'data': '!'}]))
        plan = json.loads(self.runj('xargs', '--plan', 'echo', data=b'one').stdout)
        semantic_validator('xargs-plan').validate(plan)
        self.assertFalse(semantic_validator('xargs-plan').is_valid([{'argv': []}]))
        self.assertFalse(semantic_validator('xargs-plan-row').is_valid({'argv': ['echo'], 'executed': True}))
        for expression, expected in (('1+1', 'complete'), ('1/0', 'failed')):
            wire = self.runj('calc', '--data', expression, ok=expected == 'complete').stdout
            document = json.loads(self.runj('json', '--inspect-data', data=wire).stdout)
            validator = semantic_validator('data-projection')
            validator.validate(document)
            self.assertEqual(document['terminal']['state'], expected)
            invalid = copy.deepcopy(document); invalid['terminal']['producer_status'] = '0'
            self.assertFalse(validator.is_valid(invalid))
            invalid = copy.deepcopy(document); invalid['terminal']['hash'] = None
            self.assertFalse(validator.is_valid(invalid))


if __name__ == '__main__':
    unittest.main()
