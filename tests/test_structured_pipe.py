#!/usr/bin/env python3
"""Semantic regressions for JSONL operators and explicit JTV2 adapters."""
import base64
import json
import math
from fractions import Fraction
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from semantic_schema_fixture import semantic_validator

JASH = Path(os.environ.get('JASH', str(Path(__file__).resolve().parents[1] / 'build/jash')))
ROOT = Path(__file__).resolve().parents[1]


def native_rows(wire, records=True):
    """Small independent decoder used only to assert native kinds and credits."""
    assert wire[:4] in (b'JTV1', b'JTV2')
    version2 = wire[:4] == b'JTV2'
    rows, terminal = [], None
    at = 4
    while at < len(wire):
        tag = wire[at:at + 1]
        size = struct.unpack_from('>Q', wire, at + 1)[0]
        at += 9
        payload = wire[at:at + size]
        assert len(payload) == size
        at += size
        if tag == b'I':
            credit = struct.unpack_from('>Q', payload)[0] if version2 else 0
            start = 8 if version2 else 0
            assert payload[start:start + 4] == b'JDV1'
            pos = start + 4

            def number(signed=False):
                nonlocal pos
                value = struct.unpack_from('>q' if signed else '>Q', payload, pos)[0]
                pos += 8
                return value

            def text():
                nonlocal pos
                length = number()
                value = payload[pos:pos + length]
                assert len(value) == length
                pos += length
                return value

            def value(depth=0):
                nonlocal pos
                assert depth < 64
                kind = payload[pos]
                pos += 1
                if kind == 0:
                    item = None
                elif kind == 1:
                    item = bool(payload[pos])
                    pos += 1
                elif kind in (2, 5, 6):
                    item = number(True)
                elif kind in (3, 4):
                    item = number()
                elif kind in (7, 8, 9, 10):
                    item = text()
                elif kind == 11:
                    item = [value(depth + 1) for _ in range(number())]
                elif kind == 12:
                    item = {}
                    for _ in range(number()):
                        key = text().decode('utf-8')
                        assert key not in item
                        item[key] = value(depth + 1)
                else:
                    raise AssertionError(kind)
                return kind, item

            kind, item = value()
            assert (not records or kind == 12) and pos == len(payload)
            rows.append((item if records else (kind, item), credit))
        elif tag == b'T':
            assert at == len(wire) and terminal is None
            terminal = {'state': payload[0], 'completeness': payload[1], 'truncated': payload[2],
                        'hash_present': payload[3], 'items': struct.unpack_from('>Q', payload, 8)[0],
                        'bytes': struct.unpack_from('>Q', payload, 16)[0],
                        'errors': struct.unpack_from('>Q', payload, 24)[0],
                        'producer_status': struct.unpack_from('>q', payload, 32)[0],
                        'transform_status': struct.unpack_from('>q', payload, 40)[0],
                        'transport_status': struct.unpack_from('>q', payload, 48)[0]}
    assert terminal is not None and terminal['items'] == len(rows)
    if version2:
        assert terminal['bytes'] == sum(credit for _, credit in rows)
    return rows, terminal


def native_projection(value):
    """JSON field projection of the independent decoder's typed tuples."""
    kind, item = value
    if kind == 12:
        return {key: native_projection(field) for key, field in item.items()}
    if kind == 11:
        return [native_projection(field) for field in item]
    if kind in (7, 10):
        return item.decode('utf-8')
    if kind == 9:
        try:
            return item.decode('utf-8')
        except UnicodeDecodeError:
            pass
    if kind in (8, 9):
        return {'encoding': 'base64', 'data': base64.b64encode(item).decode('ascii')}
    return item

class StructuredPipe(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture_temp = tempfile.TemporaryDirectory(prefix='jash-pipe-native-fixture-')
        cls.addClassCleanup(cls.fixture_temp.cleanup)
        cls.fixture = Path(cls.fixture_temp.name) / 'native-fixture'
        sources = ['tests/data_pipe_fixture.c', 'c/core/data_emit.c', 'c/core/data_value.c',
                   'c/core/data_stream.c', 'c/core/data_wire.c', 'c/module/blake3.c']
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'c'), *(str(ROOT / path) for path in sources),
                        '-o', str(cls.fixture)], check=True, capture_output=True, timeout=60)

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='jash-pipe-contract-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.env = dict(os.environ, HOME=str(self.root), JASH_SIGIL='0', JASH_NUDGE='0', JASH_BRIEF='0', JASH_MEMO='0', JASH_AGENT='0')

    def runop(self, command, value=b'', ok=True):
        if isinstance(value, list):
            value = ''.join(json.dumps(x, ensure_ascii=True) + '\n' for x in value).encode()
        p = subprocess.run([str(JASH), '-c', command], input=value, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, env=self.env, cwd=self.root, timeout=10)
        if ok:
            self.assertEqual(p.returncode, 0, (command, p.stderr, p.stdout[:200]))
        return p

    def rows(self, command, value):
        rows = [json.loads(x) for x in self.runop(command, value).stdout.splitlines()]
        validator = semantic_validator('operator-' + command.split()[0] + '-row')
        for row in rows:
            validator.validate(row)
        return rows

    def fixture_data(self, case):
        return subprocess.run([str(self.fixture), case], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, check=True, timeout=10).stdout

    def test_nested_boolean_null_and_unicode(self):
        row = {'a': {'nested': [True, None, '\U0001f63a', {'x': 2}]}, 'b': False, 'c': None, 's': 'null'}
        self.assertEqual(self.rows('pick a,b,c,s', [row]), [row])

    def test_embedded_nul_string_is_not_truncated(self):
        self.assertEqual(self.rows('pick s', [{'s': 'a\0b'}]), [{'s': 'a\0b'}])

    def test_sort_resolves_key_in_each_row_stably(self):
        rows = [{'x': 5, 'id': 'first'}, {'id': 'second', 'x': 2}, {'x': 2, 'id': 'third'}, {'id': 'missing'}]
        self.assertEqual([r['id'] for r in self.rows('sortby x', rows)], ['second', 'third', 'first', 'missing'])

    def test_integer_sort_and_filter_are_exact(self):
        rows = [{'n': 9007199254740993}, {'n': 9007199254740992}, {'n': 18446744073709551615}]
        self.assertEqual(self.rows('sortby n', rows), [rows[1], rows[0], rows[2]])
        self.assertEqual(self.rows("where 'n>9007199254740992'", rows), [rows[0], rows[2]])

    def test_integer_sum_and_average_are_exact(self):
        self.assertEqual(self.rows('sum n', [{'n': 9007199254740993}, {'n': 2}]), [{'sum': 9007199254740995}])
        self.assertEqual(self.rows('avg n', [{'n': 9007199254740993}, {'n': 9007199254740995}]), [{'avg': 9007199254740994}])

    def test_decimal_arithmetic_is_finite_and_exact(self):
        self.assertEqual(self.rows('sum n', b'{"n":0.1}\n{"n":0.2}\n'), [{'sum': 0.3}])
        self.assertEqual(self.rows('avg n', [{'n': 1}, {'n': 2}]), [{'avg': 1.5}])
        p = self.runop('sum n', b'{"n":1e999999}\n', ok=False)
        self.assertNotEqual(p.returncode, 0)
        self.assertNotIn(b'inf', p.stdout)

    def test_count_counts_records_not_blank_lines(self):
        self.assertEqual(self.rows('count', b'\n {"x":1}\n\r\n{"x":2}\n'), [{'count': 2}])

    def test_typed_groups_do_not_merge_bool_string_null(self):
        rows = [{'k': True}, {'k': 'true'}, {'k': None}, {'k': 'null'}, {'k': True}]
        self.assertEqual(self.rows('count -g k', rows), [{'k': True, 'count': 2}, {'k': 'true', 'count': 1}, {'k': None, 'count': 1}, {'k': 'null', 'count': 1}])

    def test_uniq_count_waits_for_final_counts(self):
        self.assertEqual(self.rows('uniqby -c k', [{'k': 'x'}, {'k': 'x'}, {'k': 'y'}]), [{'k': 'x', 'count': 2}, {'k': 'y', 'count': 1}])

    def test_nested_group_values_preserved(self):
        rows = [{'k': {'a': 1, 'b': [False]}}, {'k': {'b': [False], 'a': 1}}]
        self.assertEqual(self.rows('count -g k', rows), [{'k': rows[0]['k'], 'count': 2}])

    def test_pick_missing_fields_preserves_row_cardinality(self):
        self.assertEqual(self.rows('pick missing', [{'x': 1}]), [{}])

    def test_malformed_rows_fail_without_partial_results(self):
        bad = [b'{"x":1,}', b'{"x":01}', b'{"x":true}junk', b'{"x":1,"x":2}', b'{"x":"\\ud800"}', b'{"x":"\\q"}', b'{"x":"\xff"}', b'{"x":NaN}', b'{"x":[1,]}']
        for command in ['where x=1', 'pick x', 'sortby x', 'count', 'sum x', 'avg x', 'uniqby x', 'describe']:
            for row in bad:
                with self.subTest(command=command, row=row):
                    result = self.runop(command, b'{"x":1}\n' + row + b'\n', ok=False)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(result.stdout, b'')

    def test_oversized_line_fails(self):
        result = self.runop('count', b'{"x":"' + b'x' * 70000 + b'"}\n', ok=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, b'')

    def test_invalid_options_and_conditions_fail(self):
        for command in ["where 'x>1garbage'", 'where x=1,', 'pick --invalid x', 'sortby x extra', 'count --invalid', 'sum', 'avg x -g', 'uniqby', 'describe extra']:
            with self.subTest(command=command):
                p = self.runop(command, b'{"x":1}\n', ok=False)
                self.assertNotEqual(p.returncode, 0)

    def test_jtv2_roundtrip_composition_and_exact_types(self):
        data = self.runop('pick --to-data n,k', [{'n': 9007199254740993, 'k': True}, {'n': 4, 'k': False}]).stdout
        self.assertEqual(data[:4], b'JTV2')
        filtered = self.runop("where --data 'n>4'", data).stdout
        self.assertEqual(self.rows('pick --from-data n,k', filtered), [{'n': 9007199254740993, 'k': True}])
        self.assertEqual(self.rows('count --from-data', data), [{'count': 2}])

    def test_native_ls_bytes_survive_projection(self):
        (self.root/'file').write_bytes(b'a')
        wire = self.runop('ls --data-v2 .').stdout
        projected = self.runop('pick --data name,path', wire).stdout
        row = self.rows('pick --from-data name,path', projected)
        self.assertTrue(any(r['name'] == {'encoding': 'base64', 'data': 'ZmlsZQ=='} for r in row))

    def test_native_input_requires_complete_verified_terminal(self):
        wire = self.runop('pick --to-data x', [{'x': 1}]).stdout
        for bad in [wire[:-1], b'JTV1' + wire[4:], wire[:-8] + b'garbage!']:
            result = self.runop('count --from-data', bad, ok=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b'')

    def test_fractional_native_output_is_explicitly_unsupported(self):
        p = self.runop('pick --to-data n', [{'n': 0.5}], ok=False)
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual(p.stdout, b'')

    def test_native_groups_keep_distinct_units_bytes_and_user_objects(self):
        wire = self.fixture_data('groups')
        rows, terminal = native_rows(self.runop('count --data -g k', wire).stdout)
        self.assertEqual(terminal['state'], 0)
        self.assertEqual(len(rows), 8)
        self.assertEqual({row['k'][0] for row, _ in rows}, {2, 3, 4, 7, 8, 9, 10, 12})
        self.assertEqual([row['count'][1] for row, _ in rows if row['k'][0] == 8], [2])

    def test_native_sort_bytes_preserves_values_and_logical_credits(self):
        wire = self.fixture_data('units')
        rows, terminal = native_rows(self.runop('sortby --data name', wire).stdout)
        self.assertEqual([row['name'] for row, _ in rows], [(8, b'a'), (8, b'z')])
        self.assertEqual([credit for _, credit in rows], [200, 100])
        self.assertEqual(terminal['bytes'], 300)

    def test_native_sum_and_average_preserve_unit(self):
        for case, kind, total, mean in [('units', 4, 8, 4), ('duration', 6, -4, -2)]:
            wire = self.fixture_data(case)
            for command, field, expected in [('sum', 'sum', total), ('avg', 'avg', mean)]:
                with self.subTest(case=case, command=command):
                    rows, _ = native_rows(self.runop(f'{command} --data n', wire).stdout)
                    self.assertEqual(rows[0][0][field], (kind, expected))
        for case, command in [('mixedunits', 'sum'), ('mixedunits', 'avg'), ('timestamp', 'sum')]:
            with self.subTest(case=case, command=command):
                result = self.runop(f'{command} --data n', self.fixture_data(case), ok=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, b'')

    def test_native_fractional_average_is_exact_versioned_and_unit_aware(self):
        cases = [('fractional', Fraction(3, 2), 'bytes'),
                 ('fractional-duration', Fraction(-1, 2), 'nanoseconds'),
                 ('fractional-timestamp', Fraction(-3, 2), 'unix_nanoseconds'),
                 ('avg-unsigned-limit', Fraction(2 * (2**64 - 1) - 1, 2), 'scalar'),
                 ('avg-signed-limit', Fraction(-2 * 2**63 + 1, 2), 'scalar'),
                 ('avg-mixed-int', Fraction(2**63 - 1, 2), 'scalar')]
        for case, expected, unit in cases:
            with self.subTest(case=case):
                for mode in ('--data', '--data-v2'):
                    wire = self.runop(f'avg {mode} n', self.fixture_data(case)).stdout
                    rows, terminal = native_rows(wire)
                    number = rows[0][0]['avg']
                    self.assertEqual(number[0], 12)
                    projected = native_projection(number)
                    semantic_validator('operator-avg-row').validate({'avg': projected})
                    self.assertEqual(projected['schema'], 'jash.number.mixed_rational.v1')
                    self.assertEqual(projected['unit'], unit)
                    self.assertLess(projected['numerator'], projected['denominator'])
                    self.assertEqual(math.gcd(projected['numerator'], projected['denominator']), 1)
                    actual = Fraction(projected['whole']) + Fraction(projected['numerator'], projected['denominator'])
                    if projected['negative']: actual = -actual
                    self.assertEqual(actual, expected)
                    self.assertEqual(number[1]['whole'][0], 3)
                    self.assertEqual(number[1]['unit'][0], 10)
                    self.assertEqual(terminal['state'], 0); self.assertEqual(terminal['bytes'], 0)
                    imported = self.runop('pick --from-data avg', wire)
                    self.assertEqual(json.loads(imported.stdout)['avg'], projected)

    def test_json_integer_average_to_data_is_exact_and_grouped(self):
        inputs = [{'n': 0, 'g': 'two-thirds'}, {'n': 2, 'g': 'two-thirds'}, {'n': 0, 'g': 'two-thirds'},
                  {'n': -1, 'g': 'negative'}, {'n': 0, 'g': 'negative'},
                  {'n': 3, 'g': 'integer'}, {'n': 5, 'g': 'integer'}]
        rows, terminal = native_rows(self.runop('avg --to-data n -g g', inputs).stdout)
        projected = {row['g'][1].decode(): native_projection(row['avg']) for row, _ in rows}
        self.assertEqual(projected['integer'], 4)
        self.assertEqual(projected['two-thirds']['whole'], 0)
        self.assertEqual(projected['two-thirds']['numerator'], 2)
        self.assertEqual(projected['two-thirds']['denominator'], 3)
        self.assertTrue(projected['negative']['negative'])
        self.assertEqual(projected['negative']['unit'], 'scalar')
        self.assertEqual(terminal['bytes'], 0)
        for value in projected.values(): semantic_validator('operator-avg-row').validate({'avg': value})
        fractional = projected['negative']
        for field, value in [('denominator', 0), ('numerator', 0), ('whole', '0'), ('unit', 'implicit'), ('negative', 1)]:
            bad = dict(fractional); bad[field] = value
            self.assertTrue(list(semantic_validator('operator-avg-row').iter_errors({'avg': bad})))

    def test_native_average_rejects_unrepresentable_json_input_without_coercion(self):
        for inputs in ([{'n': 0.5}, {'n': 1.5}], [{'n': 1.0}], [{'n': 2**64}, {'n': 0}], b'{"n":1e0}\n'):
            result = self.runop('avg --to-data n', inputs, ok=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, b'')
        # Numeric JSON remains the established JSON operator contract.
        self.assertEqual(self.rows('avg n', [{'n': 0.5}, {'n': 1.5}]), [{'avg': 1}])

    def test_native_describe_reports_real_kinds_and_native_samples(self):
        rows, _ = native_rows(self.runop('describe --data', self.fixture_data('units')).stdout)
        fields = {row['field'][1]: row for row, _ in rows}
        self.assertEqual(fields[b'n']['type'], (7, b'filesize'))
        self.assertEqual(fields[b'name']['type'], (7, b'bytes'))
        self.assertEqual(fields[b'n']['samples'], (11, [(4, 3), (4, 5)]))
        self.assertEqual(fields[b'name']['samples'], (11, [(8, b'z'), (8, b'a')]))

    def test_native_input_does_not_launder_partial_or_failed_outcomes(self):
        for case in ['failed', 'incomplete', 'cancelled', 'errors', 'status']:
            with self.subTest(case=case):
                result = self.runop('count --from-data', self.fixture_data(case), ok=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, b'')

if __name__ == '__main__':
    unittest.main()
