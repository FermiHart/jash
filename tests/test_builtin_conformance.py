#!/usr/bin/env python3
"""Negative controls for qualification evidence; never execute product gates."""
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest import mock
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    'builtin_conformance', ROOT / 'tools/check_builtin_conformance.py')
CONFORMANCE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CONFORMANCE)
GENERATOR_SPEC = importlib.util.spec_from_file_location(
    'builtin_contract_generator', ROOT / 'tools/gen_builtin_contracts.py')
GENERATOR = importlib.util.module_from_spec(GENERATOR_SPEC)
GENERATOR_SPEC.loader.exec_module(GENERATOR)


def junit(*entries):
    document = ET.Element('testsuite')
    for name, properties, output, failure in entries:
        case = ET.SubElement(document, 'testcase', name=name, **properties)
        if output is not None:
            stream, text = output
            ET.SubElement(case, stream).text = text
        if failure:
            ET.SubElement(case, failure)
    return document


class BuiltinConformance(unittest.TestCase):
    def test_generator_rejects_unresolvable_projection_schema(self):
        document = json.loads(GENERATOR.SOURCE.read_text())
        alias = next(row for row in document['builtins'] if row['name'] == 'alias')
        mode = next(row for row in alias['modes'] if row['id'] == 'list-lookup-data')
        reference = mode['resource_limits']['projection_schema']
        self.assertEqual(reference, 'urn:jash:alias:v1#/properties/items/items')
        self.assertIn('c/core/builtin_mode_contracts.inc', GENERATOR.generate(document))
        invalid = [reference.replace('#', ''),
                   'urn:jash:unknown:v1#/properties/items/items',
                   'urn:jash:alias:v1#/properties/not_present',
                   'urn:jash:alias:v1#/properties/items/type',
                   'urn:jash:alias:v1#/properties/~2', '']
        for broken in invalid:
            with self.subTest(reference=broken):
                mode['resource_limits']['projection_schema'] = broken
                with self.assertRaisesRegex(ValueError, 'unresolved projection schema'):
                    GENERATOR.generate(document)

    def test_projection_schema_json_pointer_resolves_exact_keys_and_indices(self):
        schema = {'type': 'object', 'properties': {
            'a/b': {'type': 'boolean'}, '~tilde': {'type': 'null'}},
            'allOf': [{'type': 'string'}, True]}
        schemas = {'urn:jash:test:v1': schema}
        cases = {'': schema, '#': schema,
                 '#/properties/a~1b': {'type': 'boolean'},
                 '#/%70roperties/~0tilde': {'type': 'null'},
                 '#/allOf/0': {'type': 'string'}, '#/allOf/1': True}
        for fragment, expected in cases.items():
            with self.subTest(fragment=fragment):
                self.assertEqual(GENERATOR.schema_reference_node(
                    'urn:jash:test:v1' + fragment, schemas), expected)
        invalid = ['#named-anchor', '#/allOf/01', '#/allOf/-1', '#/allOf/2',
                   '#/properties/~2tilde', '#/properties/%', '#/properties/%ff',
                   '#/type', '#/properties/a/b']
        for fragment in invalid:
            with self.subTest(fragment=fragment):
                with self.assertRaises(ValueError):
                    GENERATOR.schema_reference_node('urn:jash:test:v1' + fragment, schemas)

    def test_registration_without_selected_label_is_insufficient(self):
        evidence, errors = CONFORMANCE.semantic_evidence(
            {'tests/test_required.py'}, [('present_only', ['python3', 'tests/test_required.py'])],
            {'different_selected_gate'})
        self.assertEqual(evidence, {})
        self.assertEqual(len(errors), 1)
        self.assertIn('no builtin-contracts label', errors[0])

    def test_master_registration_cannot_supply_ctest_evidence(self):
        result = SimpleNamespace(stdout=json.dumps({'tests': [{
            'name': 'selected', 'command': ['python3', 'tests/test_selected.py'],
            'properties': [{'name': 'LABELS', 'value': ['builtin-contracts']}]}]}))
        with mock.patch.object(CONFORMANCE.subprocess, 'run', return_value=result) as run:
            commands, labelled = CONFORMANCE.registered_tests(Path('/unused/build'), 'ctest')
        self.assertEqual(run.call_count, 1)
        self.assertEqual(labelled, {'selected'})
        evidence, errors = CONFORMANCE.semantic_evidence(
            {'tests/test_only_in_master.py'}, commands, labelled)
        self.assertFalse(evidence)
        self.assertIn('not registered in CTest', errors[0])

    def test_real_harness_drivers_and_direct_units_bind_to_selected_gates(self):
        required = {'tests/test_digest_engine_unit.c', 'tests/test_assert_delivery.c',
                    'tests/test_data_filter.c'}
        commands = [('digest', ['python3', '/source/tests/test_digest_project.py']),
                    ('assert', ['python3', '/source/tests/test_assert_native.py']),
                    ('filter', ['/build/test_data_filter'])]
        evidence, errors = CONFORMANCE.semantic_evidence(
            required, commands, {'digest', 'assert', 'filter'})
        self.assertFalse(errors)
        self.assertEqual(evidence, {'tests/test_digest_engine_unit.c': ['digest'],
                                   'tests/test_assert_delivery.c': ['assert'],
                                   'tests/test_data_filter.c': ['filter']})

    def test_required_execution_must_appear_in_junit(self):
        document = junit(('other', {'status': 'run'}, None, None))
        errors = CONFORMANCE.junit_errors(
            document, {'other', 'required'}, {'tests/test_required.py': ['required']})
        self.assertTrue(any('Labelled gate missing' in error for error in errors))
        self.assertTrue(any('no executed JUnit evidence' in error for error in errors))

    def test_completed_evidence_accepts_real_assertion_output(self):
        document = junit(('required', {'status': 'run'},
                          ('system-out', 'Ran 23 tests in 1.0s\n\nOK\n'), None))
        self.assertEqual(CONFORMANCE.junit_errors(
            document, {'required'}, {'tests/test_required.py': ['required']}), [])

    def test_ctest_removed_output_cannot_hide_inner_skip_summary(self):
        # These gates have a successful outer status and no visible SKIP.
        # CTest's removal marker makes the missing summary unknowable.
        for threshold in (1024, 16777216):
            output = ('test_visible_assertion ... ok\n' +
                      '[This part of the test output was removed since it exceeds '
                      f'the threshold of {threshold} bytes.]')
            for stream in ('system-out', 'system-err'):
                with self.subTest(threshold=threshold, stream=stream):
                    errors = CONFORMANCE.junit_errors(
                        junit(('required', {'status': 'run'}, (stream, output), None)),
                        {'required'}, {'tests/test_required.py': ['required']})
                    self.assertEqual(errors, [
                        'Truncated CTest output is not qualification: required'])

    def test_complete_output_above_old_ctest_limit_is_accepted(self):
        output = 'test_observation ... ok\n' * 100 + 'Ran 100 tests in 1.0s\n\nOK\n'
        self.assertGreater(len(output), 1024)
        self.assertEqual(CONFORMANCE.junit_errors(
            junit(('required', {'status': 'run'}, ('system-out', output), None)),
            {'required'}, {'tests/test_required.py': ['required']}), [])

    def test_failures_unfinished_and_duplicate_cases_are_rejected(self):
        for failure in ('failure', 'error', 'skipped'):
            with self.subTest(failure=failure):
                self.assertTrue(CONFORMANCE.junit_errors(
                    junit(('required', {'status': 'run'}, None, failure)), {'required'}, {}))
        for status in ('notrun', 'disabled', 'skipped'):
            with self.subTest(status=status):
                self.assertTrue(CONFORMANCE.junit_errors(
                    junit(('required', {'status': status}, None, None)), {'required'}, {}))
        errors = CONFORMANCE.junit_errors(junit(
            ('required', {'status': 'run'}, None, None),
            ('required', {'status': 'run'}, None, None)), {'required'}, {})
        self.assertTrue(any('Duplicate' in error for error in errors))

    def test_exit_zero_inner_skips_and_empty_suites_are_rejected(self):
        outputs = ['Ran 23 tests\nOK (skipped=1)',
                   'OK (skipped=1, expected failures=1)',
                   'unittest.case.SkipTest: unavailable', 'Ran 0 tests\nOK',
                   'SKIP: binary unavailable', '  SKIPPED missing dependency',
                   '✓ JSON validity (python3 absent — skipped)']
        for text in outputs:
            for stream in ('system-out', 'system-err'):
                with self.subTest(text=text, stream=stream):
                    self.assertTrue(CONFORMANCE.junit_errors(
                        junit(('required', {'status': 'run'}, (stream, text), None)),
                        {'required'}, {}))

    def test_product_skip_fields_do_not_masquerade_as_runner_skips(self):
        output = ('{"skipped":0,"status":"complete","truncated":true}\n'
                  'test_truncated_value_is_observed ... ok\n'
                  'test_skipped_action_is_observed ... ok\nRan 1 test\nOK\n')
        self.assertEqual(CONFORMANCE.junit_errors(
            junit(('required', {'status': 'run'}, ('system-out', output), None)),
            {'required'}, {}), [])


if __name__ == '__main__':
    unittest.main(verbosity=2)
