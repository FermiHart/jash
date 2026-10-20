"""Native B19 stores, precise heuristic scope, and checked effects."""
import json
import os
from pathlib import Path
import unittest

import test_structured_models as models


class StructuredLearning(unittest.TestCase):
    setUpClass = classmethod(models.StructuredModels.setUpClass.__func__)
    tearDownClass = classmethod(models.StructuredModels.tearDownClass.__func__)
    setUp = models.StructuredModels.setUp
    runj = models.StructuredModels.runj
    report = models.StructuredModels.report

    def test_learning_roundtrip_uses_exact_argv_and_store_records(self):
        pattern = 'quote " and backslash \\ and line\nnext ∞'
        _, written, native, _ = self.report('learn', '--', pattern, '--repo', 'sample')
        self.assertEqual(written['operation'], 'learn')
        self.assertTrue(written['persisted'])
        self.assertEqual(written['details']['pattern'], pattern)
        self.assertEqual(native['path'][0], 9)
        self.assertEqual(native['details'][1]['bytes_written'][0], 4)
        for mode in ('--json', '--data', '--data-v2'):
            _, listing, _, _ = self.report('learn', 'list', mode=mode)
            self.assertEqual(listing['repositories'][0]['repository'], 'sample')
            self.assertEqual(listing['repositories'][0]['patterns'][0]['pattern'], pattern)
            _, recalled, _, _ = self.report('learn', 'recall', 'line\nnext', mode=mode)
            self.assertEqual(len(recalled['repositories']), 1)
        self.report('learn', 'forget', 'sample')
        self.assertFalse((self.home / '.jash/patterns/sample.yaml').exists())

    def test_stored_null_escape_is_preserved_in_native_strings(self):
        directory = self.home / '.jash/patterns'; directory.mkdir()
        (directory / 'raw.yaml').write_text('- pattern: "a\\u0000b"\n  confidence: 0.5\n')
        _, value, native, _ = self.report('learn', 'list')
        row = value['repositories'][0]['patterns'][0]
        self.assertEqual(row['pattern'], 'a\x00b')
        self.assertEqual(row['confidence_recorded']['significand'], 1)
        self.assertEqual(row['confidence_recorded']['exponent2'], -1)
        field = native['repositories'][1][0][1]['patterns'][1][0][1]['pattern']
        self.assertEqual(field, (7, b'a\x00b'))

    def test_malformed_memory_fails_without_appending(self):
        directory = self.home / '.jash/patterns'; directory.mkdir()
        path = directory / 'bad.yaml'
        for content in (b'- pattern: "x"\n  pattern: "duplicate"\n',
                        b'- pattern: "\\ud800"\n', b'- pattern: "x"\n  confidence: NaN\n',
                        b'- pattern: "x"\n  extra: true\n', b'- pattern: "x"\x00\n'):
            with self.subTest(content=content):
                path.write_bytes(content)
                for args in (('list',), ('value', '--repo', 'bad')):
                    failed, value, _, terminal = self.report('learn', *args, ok=False)
                    self.assertNotEqual(failed.returncode, 0)
                    self.assertEqual(value['error'], 'invalid_or_oversized_memory_store')
                    self.assertEqual(terminal['state'], 2)
                    self.assertEqual(path.read_bytes(), content)

    def test_preference_native_apply_reports_actual_environment_changes(self):
        self.report('preference', 'learn', 'color: off', '--category', 'display')
        self.report('preference', 'learn', 'output-format: json')
        _, value, _, _ = self.report('preference', 'recall', 'display')
        self.assertEqual(value['preferences'][0]['category'], 'display')
        result = self.runj([], code='preference --json apply; /usr/bin/env')
        lines = [line for line in result.stdout.splitlines() if line.strip()]
        applied = json.loads(lines[0])
        environment = dict(line.split(b'=', 1) for line in lines[1:] if b'=' in line)
        self.assertEqual(applied['applied'], 2)
        effects = {item['name']: item['value'] for item in applied['effects']}
        self.assertEqual(effects, {'JASH_COLOR': '0', 'JASH_OUTPUT_FORMAT': 'json'})
        self.assertEqual(applied['scope'], 'shell-process-environment')
        self.assertEqual(environment[b'JASH_COLOR'], b'0')
        self.assertEqual(environment[b'JASH_OUTPUT_FORMAT'], b'json')

    def test_invalid_preference_is_rejected_before_any_application(self):
        path = self.home / '.jash/preferences.yaml'
        path.write_text('- category: "x"\n  preference: "color: invalid"\n')
        failed, value, _, terminal = self.report('preference', 'apply', ok=False)
        self.assertEqual(failed.returncode, 2)
        self.assertEqual(value['error'], 'invalid_preference_store_or_application')
        self.assertEqual(terminal['state'], 2)
        path.write_text('- category: "x"\n  preference: "output: j\\u0000son"\n')
        self.assertEqual(self.report('preference', 'apply', ok=False)[0].returncode, 2)

    def test_mistakes_preserve_fields_and_reject_ambiguous_legacy_rows(self):
        self.report('mistake', 'log', 'quoted " error ∞', '--context', 'git status')
        _, value, _, _ = self.report('mistake', 'avoid')
        self.assertEqual(value['analysis'], 'stored-observations')
        self.assertEqual(value['mistakes'][0]['error'], 'quoted " error ∞')
        self.assertEqual(value['mistakes'][0]['context'], 'git status')
        path = self.home / '.jash/mistakes.log'; before = path.read_bytes()
        for text in ('line\nbreak', 'x | y'):
            failed, _, _, terminal = self.report('mistake', 'log', text, ok=False)
            self.assertEqual(failed.returncode, 2)
            self.assertEqual(terminal['state'], 2)
            self.assertEqual(path.read_bytes(), before)
        path.write_bytes(b'unparseable line\n')
        self.assertEqual(self.report('mistake', 'list', ok=False)[0].returncode, 1)

    def test_prediction_reads_sources_and_never_executes(self):
        self.report('risk', 'avoid', 'danger')
        self.report('learn', 'danger', '--repo', 'sample')
        command = 'danger; touch unexpected'
        _, value, native, _ = self.report('predict', command)
        self.assertEqual(value['command'], command)
        self.assertEqual(value['risk_score'], 35)
        self.assertEqual(value['features']['avoid_matches'], 1)
        self.assertEqual(value['features']['learned_file_matches'], 1)
        self.assertEqual(value['engine'], 'substring-heuristic')
        self.assertIsNone(value['failure_probability'])
        self.assertFalse(value['executed'])
        self.assertEqual(native['risk_score'][0], 3)
        self.assertFalse((self.work / 'unexpected').exists())

    def test_risk_history_missing_is_unknown_and_counts_whole_lines(self):
        _, value, _, _ = self.report('risk', 'assess')
        self.assertFalse(value['available'])
        self.assertIsNone(value['history_lines'])
        (self.home / '.bash_history').write_text('ls\nrm -rf target\n' + 'x' * 5000 + '\n')
        for command, args in (('risk', ('assess',)), ('predict', ('risk', 'assess'))):
            _, value, _, _ = self.report(command, *args)
            self.assertEqual(value['history_lines'], 3)
            self.assertEqual(value['high_risk_commands'], 1)

    def test_reflection_uses_real_records_and_improve_apply_is_only_proposal(self):
        question = 'error "review"\nsecond line'
        _, value, _, _ = self.report('reflect', question)
        self.assertTrue(value['persisted'])
        self.assertEqual(value['details']['question'], question)
        self.assertEqual(value['details']['reflections_today_before'], 0)
        _, second, _, _ = self.report('reflect', question)
        self.assertEqual(second['details']['reflections_today_before'], 1)
        files = list((self.home / '.jash/reflections').glob('*.txt'))
        self.assertEqual(len(files), 1)
        before = files[0].read_bytes()
        _, proposal, _, _ = self.report('reflect', 'improve', 'apply', 'do something')
        self.assertEqual(proposal['status'], 'proposal-only')
        self.assertFalse(proposal['applied'])
        self.assertFalse(proposal['recorded'])
        self.report('reflect', 'improve', 'suggest')
        self.assertEqual(files[0].read_bytes(), before)

    def test_each_memory_mutation_checks_sink_before_store_effect(self):
        cases = [('learn', ['value', '--repo', 'sample'], 'patterns/sample.yaml'),
                 ('preference', ['learn', 'output: json'], 'preferences.yaml'),
                 ('mistake', ['log', 'error'], 'mistakes.log'),
                 ('risk', ['avoid', 'danger'], 'risk_patterns.yaml'),
                 ('reflect', ['question'], 'reflections')]
        import shlex
        for command, args, relative in cases:
            for mode in ('--json', '--data'):
                with self.subTest(command=command, mode=mode):
                    path = self.home / '.jash' / relative
                    self.assertFalse(path.exists())
                    result = self.runj([], code=shlex.join([command, mode, *args]) + ' > /dev/full', ok=False)
                    self.assertEqual(result.returncode, 125)
                    self.assertFalse(path.exists())

    def test_checked_append_failures_have_failed_terminal_and_partial_receipt(self):
        cases = [('learn', ['value', '--repo', 'sample'], 'sample.yaml'),
                 ('preference', ['learn', 'output: json'], 'preferences.yaml'),
                 ('mistake', ['log', 'error'], 'mistakes.log'),
                 ('risk', ['avoid', 'danger'], 'risk_patterns.yaml')]
        for command, args, basename in cases:
            with self.subTest(command=command):
                self.env.update(LD_PRELOAD=str(self.fault), JASH_TEST_MODEL_FAIL_WRITE=basename)
                result, value, _, terminal = self.report(command, *args, ok=False)
                self.assertEqual(result.returncode, 1)
                self.assertFalse(value['persisted'])
                self.assertTrue(value['effects_may_have_applied'])
                self.assertEqual(terminal['state'], 2)
                self.env.pop('LD_PRELOAD'); self.env.pop('JASH_TEST_MODEL_FAIL_WRITE')

    def test_store_limits_and_non_regular_inputs_fail_explicitly(self):
        path = self.home / '.jash/preferences.yaml'
        path.write_bytes(b'x' * (512 * 1024 + 1))
        self.assertEqual(self.report('preference', 'list', ok=False)[0].returncode, 1)
        path.unlink(); path.mkdir()
        self.assertEqual(self.report('preference', 'list', ok=False)[0].returncode, 1)

    def test_fifo_store_never_blocks_query_or_append(self):
        for name, command, args in (('token_usage.log', 'token', ['history']),
                                     ('mistakes.log', 'mistake', ['log', 'value'])):
            path = self.home / '.jash' / name
            os.mkfifo(path)
            failed, _, _, terminal = self.report(command, *args, ok=False)
            self.assertNotEqual(failed.returncode, 0)
            self.assertEqual(terminal['state'], 2)
            path.unlink()

    def test_presentation_budget_refuses_json_before_store_mutation(self):
        self.env['JASH_LLM_BUDGET'] = '1'
        for command, args, name in (('learn', ['value', '--repo', 'a'], 'patterns/a.yaml'),
                                    ('preference', ['learn', 'value'], 'preferences.yaml'),
                                    ('mistake', ['log', 'value'], 'mistakes.log'),
                                    ('risk', ['avoid', 'value'], 'risk_patterns.yaml'),
                                    ('reflect', ['value'], 'reflections')):
            failed = self.runj([command, '--json', *args], ok=False)
            self.assertEqual(failed.returncode, 75)
            self.assertEqual(json.loads(failed.stdout)['command_status'], 75)
            self.assertFalse((self.home / '.jash' / name).exists())
        self.env['JASH_LLM_BUDGET'] = '1024'
        failed = self.runj(['learn', '--json', 'long-value ' * 200, '--repo', 'larger'], ok=False)
        self.assertEqual(failed.returncode, 75)
        self.assertFalse((self.home / '.jash/patterns/larger.yaml').exists())

    def test_native_presentation_budget_is_separate_from_store_and_argument_limits(self):
        from test_structured_pipe import native_rows
        self.env['JASH_LLM_BUDGET'] = '1'
        for mode in ('--data', '--data-v2'):
            made = self.runj(['learn', mode, 'actual value', '--repo', 'native'])
            rows, terminal = native_rows(made.stdout)
            self.assertEqual(terminal['state'], 0)
            self.assertTrue(rows[0][0]['persisted'][1])
            oversized = self.runj(['learn', mode, 'x' * 16385, '--repo', 'refused'], ok=False)
            self.assertEqual(oversized.returncode, 2)
            self.assertEqual(native_rows(oversized.stdout)[1]['state'], 2)
            self.assertFalse((self.home / '.jash/patterns/refused.yaml').exists())
        oversized_store = self.home / '.jash/preferences.yaml'
        oversized_store.write_bytes(b'x' * (512 * 1024 + 1))
        failed = self.runj(['preference', '--data', 'list'], ok=False)
        self.assertNotEqual(failed.returncode, 0)
        self.assertEqual(native_rows(failed.stdout)[1]['state'], 2)


if __name__ == '__main__':
    unittest.main()
