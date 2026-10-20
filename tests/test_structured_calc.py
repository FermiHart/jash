#!/usr/bin/env python3
"""B23: declared exact rational / finite binary64 calculator results."""
import copy
from fractions import Fraction
import json
import math
import os
from pathlib import Path
import random
import shlex
import subprocess
import tempfile
import unittest

from test_structured_pipe import native_projection, native_rows
from semantic_schema_fixture import semantic_validator

ROOT = Path(__file__).resolve().parents[1]
JASH = Path(os.environ.get('JASH', ROOT / 'build/jash')).resolve()


def rational(value):
    assert value['schema'] == 'jash.number.rational.v1'
    return Fraction((-1 if value['negative'] else 1) * value['numerator'], value['denominator'])


class StructuredCalc(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix='jash-calc-contract-')
        self.addCleanup(tmp.cleanup)
        self.work = Path(tmp.name)
        self.env = {k: v for k, v in os.environ.items() if not k.startswith('JASH_')}
        self.env.update(HOME=str(self.work), JASH_SIGIL='0', JASH_TIMELINE='0',
                        JASH_MEMO='0', JASH_NUDGE='0', JASH_ADOPT='0', JASH_BRIEF='0',
                        JASH_LOOPGUARD='off', JASH_WORLD='0', JASH_PREFETCH='0')

    def runj(self, expression, mode='--json', ok=True, prefix=''):
        command = prefix + shlex.join(['calc'] + ([mode] if mode else []) + [expression])
        p = subprocess.run([str(JASH), '-c', command], stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, cwd=self.work, env=self.env, timeout=10)
        if ok:
            self.assertEqual(p.returncode, 0, (command, p.stderr, p.stdout[:200]))
            if mode == '--json':
                semantic_validator('calc').validate(json.loads(p.stdout))
            elif mode in ('--data', '--data-v2'):
                rows, _ = native_rows(p.stdout)
                for row, _ in rows:
                    semantic_validator('calc').validate(native_projection((12, row)))
        return p

    def exact(self, expression, expected, **kwargs):
        result = json.loads(self.runj(expression, **kwargs).stdout)
        self.assertEqual(result['schema'], 'jash.calc.v1')
        self.assertTrue(result['exact'])
        self.assertEqual(rational(result['result']), expected)
        return result

    def test_large_integers_are_exact(self):
        for text in ('9007199254740993', '18446744073709551615', '-9223372036854775808'):
            self.exact(text, Fraction(int(text)))
        self.exact('9007199254740993 + 2', Fraction(9007199254740995))

    def test_rationals_and_decimal_literals_do_not_round(self):
        for text, expected in (('0.1+0.2', Fraction(3, 10)), ('1/3+1/6', Fraction(1, 2)),
                               ('5/2', Fraction(5, 2)), ('-5/2', Fraction(-5, 2)),
                               ('2**-3', Fraction(1, 8)), ('(3/7)*(14/9)', Fraction(2, 3))):
            self.exact(text, expected)

    def test_functions_keep_exactness_when_mathematically_available(self):
        self.exact('sqrt(81/16)', Fraction(9, 4))
        self.exact('abs(-18446744073709551615)', Fraction(18446744073709551615))

    def test_finite_approximations_have_explicit_binary64_representation(self):
        for expression, expected in (('sqrt(2)', math.sqrt(2)), ('sin(1)', math.sin(1)),
                                     ('cos(1)', math.cos(1))):
            result = json.loads(self.runj(expression).stdout)
            self.assertFalse(result['exact'])
            number = result['result']
            self.assertEqual(number['schema'], 'jash.number.binary64.v1')
            observed = math.ldexp(number['significand'], number['exponent2'])
            self.assertTrue(math.isfinite(observed))
            self.assertAlmostEqual(observed, expected, places=15)

    def test_native_results_have_versioned_records_and_integer_payloads(self):
        for selector in ('--data', '--data-v2'):
            p = self.runj('9007199254740993/3', selector)
            rows, terminal = native_rows(p.stdout)
            self.assertEqual(terminal['state'], 0)
            self.assertEqual(len(rows), 1)
            result = rows[0][0]['result']
            self.assertEqual(result[0], 12)
            self.assertEqual(result[1]['schema'], (10, b'jash.number.rational.v1'))
            self.assertEqual(result[1]['numerator'], (3, 3002399751580331))
            self.assertEqual(result[1]['denominator'], (3, 1))
            self.assertEqual(result[1]['negative'], (1, False))

    def test_invalid_and_nonfinite_expressions_are_explicit_errors(self):
        for expression in ('1/0', '1%0', 'sqrt(-1)', '1 +', '(1+2', '1+2)', '2 garbage',
                           'unknown(1)', '.', '2**64', '18446744073709551616', '1e1000'):
            for mode in ('--json', '--data'):
                with self.subTest(expression=expression, mode=mode):
                    p = self.runj(expression, mode, ok=False)
                    self.assertNotEqual(p.returncode, 0)
                    if mode == '--data' and p.stdout:
                        rows, terminal = native_rows(p.stdout)
                        self.assertEqual(rows, [])
                        self.assertEqual(terminal['state'], 2)

    def test_expression_depth_bound_and_complete_variable_values(self):
        self.exact('n+2', Fraction(9007199254740995), prefix='n=9007199254740993; ')
        for expression, prefix in (('n', 'n=123garbage; '), ('(' * 80 + '1' + ')' * 80, '')):
            self.assertNotEqual(self.runj(expression, ok=False, prefix=prefix).returncode, 0)

    def test_seeded_exact_arithmetic_matches_fraction_oracle(self):
        rng = random.Random(431)
        for _ in range(60):
            a, b, c, d = [rng.randrange(1, 1000) for _ in range(4)]
            expression = f'({a}/{b})+({c}/{d})'
            self.exact(expression, Fraction(a, b) + Fraction(c, d))

    def test_checked_writer_and_ordinary_text(self):
        self.assertEqual(self.runj('2+3*4', mode='').stdout, b'14\n')
        self.assertEqual(self.runj('5/2', mode='').stdout, b'2.5\n')
        for selector in ('--json', '--data'):
            p = subprocess.run([str(JASH), '-c', f'calc {selector} 1+2 > /dev/full'],
                               cwd=self.work, env=self.env, capture_output=True, timeout=10)
            self.assertEqual(p.returncode, 125)

    def test_published_result_schema_rejects_false_exactness_and_bad_denominator(self):
        value = json.loads(self.runj('1/3').stdout)
        validator = semantic_validator('calc')
        invalid = copy.deepcopy(value); invalid['exact'] = False
        self.assertFalse(validator.is_valid(invalid))
        invalid = copy.deepcopy(value); invalid['result']['denominator'] = 0
        self.assertFalse(validator.is_valid(invalid))
        invalid = copy.deepcopy(value); invalid['result']['numerator'] = '1'
        self.assertFalse(validator.is_valid(invalid))
        invalid = copy.deepcopy(value); invalid['result']['unclaimed'] = True
        self.assertFalse(validator.is_valid(invalid))


if __name__ == '__main__':
    unittest.main()
