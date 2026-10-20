"""B19/B20 engine state, explicit heuristics, and admission before effects."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

from test_structured_pipe import native_rows
from semantic_schema_fixture import semantic_validator, validate_semantic

ROOT = Path(__file__).resolve().parents[1]
JASH = Path(os.environ.get('JASH', ROOT / 'build/jash')).resolve()


class StructuredModels(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fault_dir = tempfile.TemporaryDirectory(prefix='jash-model-faults-')
        cls.fault = Path(cls.fault_dir.name) / 'model-faults.so'
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        str(ROOT / 'tests/model_io_faults.c'), '-ldl', '-o', str(cls.fault)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.fault_dir.cleanup()

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='jash-semantic-model-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.home = self.root / 'home'; self.home.mkdir()
        (self.home / '.jash').mkdir()
        self.work = self.root / 'work'; self.work.mkdir()
        self.env = {key: value for key, value in os.environ.items() if not key.startswith('JASH_')}
        self.env.update(HOME=str(self.home), JASH_SIGIL='0', JASH_TIMELINE='0', JASH_MEMO='0',
                        JASH_NUDGE='0', JASH_ADOPT='0', JASH_BRIEF='0', JASH_WORLD='0',
                        JASH_PREFETCH='0', JASH_LOOPGUARD='off')

    def runj(self, argv, *, code=None, data=b'', ok=True):
        result = subprocess.run([str(JASH), '-c', code or shlex.join(argv)], cwd=self.work,
                                env=self.env, input=data, capture_output=True, timeout=15)
        if ok:
            self.assertEqual(result.returncode, 0, (argv, result.stdout[:300], result.stderr))
        return result

    def report(self, command, *arguments, mode='--data', ok=True):
        result = self.runj([command, mode, *arguments], ok=ok)
        if mode != '--json':
            rows, terminal = native_rows(result.stdout)
            inspected = self.runj(['json', '--inspect-data'], data=result.stdout)
            projection = json.loads(inspected.stdout)
            self.assertEqual(len(projection['items']), 1)
            validate_semantic(projection['items'][0])
            return result, projection['items'][0], rows[0][0], terminal
        value = json.loads(result.stdout)
        validate_semantic(value)
        return result, value, None, None

    def test_policy_queries_keep_integer_counters_and_scope(self):
        for command, schema in (('adopt', 'jash.adopt.v1'), ('agent-guard', 'jash.agent-guard.v1')):
            for mode in ('--json', '--data', '--data-v2'):
                _, value, native, _ = self.report(command, 'status', mode=mode)
                self.assertEqual(value['schema'], schema)
                self.assertFalse(value['changed'])
                self.assertEqual(value['metrics_scope'], 'home-cumulative')
                self.assertIs(type(value['total']), int)
                if native:
                    self.assertEqual(native['total'][0], 3)
                    self.assertEqual(native['strict'][0], 1)

    def test_policy_mutation_admission_precedes_store_write(self):
        for command, operation, basename, before in (
            ('adopt', 'rewrite', 'adopt_mode', b'0\n'),
            ('agent-guard', 'strict', 'agent_guard_mode', b'watch\n')):
            path = self.home / '.jash' / basename
            path.write_bytes(before)
            for mode in ('--json', '--data'):
                result = self.runj([], code=f'{command} {mode} {operation} > /dev/full', ok=False)
                self.assertEqual(result.returncode, 125)
                self.assertEqual(path.read_bytes(), before)
            _, value, _, _ = self.report(command, operation)
            self.assertTrue(value['changed'])
            self.assertTrue(value['ok'])
            self.assertNotEqual(path.read_bytes(), before)

    def test_stance_lists_real_config_and_changes_process_state(self):
        _, value, rows, _ = self.report('stance', 'list')
        names = [item['name'] for item in value['stances']]
        self.assertEqual(names, ['explore', 'build', 'debug', 'review', 'deploy'])
        self.assertEqual(value['scope'], 'shell-process')
        self.assertEqual(rows['stances'][1][0][1]['context_budget_tokens'][0], 3)
        _, changed, _, _ = self.report('stance', 'set', 'debug')
        self.assertTrue(changed['changed'])
        self.assertEqual(changed['stances'][0]['name'], 'debug')
        bad, failed, _, terminal = self.report('stance', 'set', 'missing', ok=False)
        self.assertEqual(bad.returncode, 2)
        self.assertEqual(failed['error'], 'unknown_stance')
        self.assertEqual(terminal['state'], 2)

    def test_invoice_actual_session_and_estimator_metadata(self):
        _, before, _, _ = self.report('llm-invoice', 'status')
        self.assertEqual(before['token_estimator'], 'bytes-divided-by-four-with-native-discount')
        self.assertFalse(before['session']['active'])
        _, started, native, _ = self.report('llm-invoice', 'begin', 'quoted " intent ∞')
        self.assertTrue(started['session']['active'])
        self.assertEqual(started['session']['intent'], 'quoted " intent ∞')
        self.assertEqual(native['session'][1]['started_ns'][0], 3)
        failed = self.runj([], code='llm-invoice --data close > /dev/full', ok=False)
        self.assertEqual(failed.returncode, 125)
        _, active, _, _ = self.report('llm-invoice', 'status')
        self.assertTrue(active['session']['active'])
        _, closed, _, _ = self.report('llm-invoice', 'close')
        self.assertFalse(closed['session']['active'])
        self.assertIs(type(closed['session']['closed_ns']), int)

    def test_token_budget_is_existing_proposal_and_counters_are_estimated(self):
        _, value, native, _ = self.report('token', 'count')
        self.assertEqual(value['estimator'], 'bytes-divided-by-four')
        self.assertEqual(native['input_tokens_estimated'][0], 2)
        _, proposal, _, _ = self.report('token', 'budget', '9007199254740993')
        self.assertEqual(proposal['requested_budget'], 9007199254740993)
        self.assertFalse(proposal['applied'])
        self.assertEqual(proposal['scope'], 'proposal')
        _, advice, _, _ = self.report('token', 'optimize')
        self.assertEqual(advice['suggestion_source'], 'static-heuristic')
        self.assertEqual(len(advice['suggestions']), 5)
        invalid, _, _, terminal = self.report('token', 'budget', '12junk', ok=False)
        self.assertEqual(invalid.returncode, 2)
        self.assertEqual(terminal['state'], 2)

    def test_store_flush_failure_never_claims_persistence(self):
        for command, operation, basename in (
                ('adopt', 'rewrite', 'adopt_mode'),
                ('agent-guard', 'strict', 'agent_guard_mode'),
                ('llm-invoice', 'begin', 'invoice_session.bin'),
                ('llm-invoice', 'reset', 'invoice.bin')):
            with self.subTest(command=command, operation=operation):
                self.env.update(LD_PRELOAD=str(self.fault), JASH_TEST_MODEL_FAIL_WRITE=basename)
                result, value, _, terminal = self.report(command, operation, ok=False)
                self.assertEqual(result.returncode, 1)
                self.assertFalse(value['ok'])
                self.assertFalse(value['persisted'])
                self.assertIsNone(value['changed'])
                self.assertTrue(value['effects_may_have_applied'])
                self.assertEqual(terminal['state'], 2)
                self.env.pop('JASH_TEST_MODEL_FAIL_WRITE')
                self.env.pop('LD_PRELOAD')

    def test_corrupt_invoice_is_rejected_without_rewriting(self):
        self.report('llm-invoice', 'begin', 'valid intent')
        path = self.home / '.jash/invoice_session.bin'
        original = path.read_bytes()
        corrupt = b'x' * 64 + original[64:]
        path.write_bytes(corrupt)
        result, value, _, terminal = self.report('llm-invoice', 'status', ok=False)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(value['error'], 'invoice_unavailable')
        self.assertEqual(terminal['state'], 2)
        self.assertEqual(path.read_bytes(), corrupt)

    def test_history_preserves_nul_and_non_utf8_and_rejects_excess(self):
        path = self.home / '.jash/token_usage.log'
        path.write_bytes(b'first\x00still first\ninvalid:\xff\n')
        _, value, native, _ = self.report('token', 'history')
        self.assertEqual(value['entries'][0], 'first\x00still first')
        self.assertEqual(native['entries'][1][1], (8, b'invalid:\xff'))
        path.write_bytes(b'x' * 4097 + b'\n')
        failed, value, _, terminal = self.report('token', 'history', ok=False)
        self.assertEqual(failed.returncode, 75)
        self.assertEqual(value['error'], 'history_limit_or_read_error')
        self.assertEqual(terminal['state'], 2)

    def test_prompt_has_actual_text_and_explicit_word_estimator(self):
        text = 'can you please help me understand git "quotes" ∞'
        _, value, native, _ = self.report('prompt', 'optimize', text)
        self.assertEqual(value['input'], text)
        self.assertEqual(value['output'], 'explain git "quotes" ∞')
        self.assertEqual(value['token_estimator'], 'whitespace-word-count')
        self.assertEqual(value['output_tokens_estimated'], 4)
        self.assertEqual(value['input_bytes'], len(text.encode()))
        self.assertEqual(native['input_bytes'][0], 4)
        self.assertFalse(value['semantic_preservation_verified'])
        _, analysis, _, _ = self.report('prompt', 'analyze', ' \t\n')
        self.assertEqual(analysis['input_tokens_estimated'], 0)
        self.assertIsNone(analysis['intent'])
        _, stats, _, _ = self.report('prompt', 'stats', mode='--json')
        self.assertGreater(stats['patterns_loaded'], 80)
        failed, _, _, terminal = self.report('prompt', 'compress', 'x' * 8192, ok=False)
        self.assertEqual(failed.returncode, 2)
        self.assertEqual(terminal['state'], 2)

    def test_forge_remains_unvalidated_scaffold_without_files(self):
        description = 'line\nwith "quotes", ∞ and ; touch unexpected'
        for mode in ('--json', '--data', '--data-v2'):
            _, value, _, _ = self.report('forge', '--', description, mode=mode)
            self.assertEqual(value['description'], description)
            self.assertEqual(value['status'], 'scaffold')
            self.assertFalse(value['validated'])
            self.assertFalse(value['implemented'])
            self.assertEqual(value['files_written'], 0)
        self.assertEqual(list(self.work.iterdir()), [])

    def test_brain_keyword_and_model_outputs_keep_estimate_scope(self):
        _, intent, _, _ = self.report('brain', 'intent', 'fix "quoted" bug')
        self.assertEqual(intent['intent_class'], 2)
        self.assertEqual(intent['engine'], 'keyword-classifier')
        self.assertFalse(intent['state_advanced'])
        _, status, native, _ = self.report('brain', 'status')
        self.assertEqual(status['model']['hidden_size'], 32)
        self.assertEqual(native['model'][1]['examples_seen'][0], 3)
        self.assertEqual(status['model']['accuracy_estimated']['schema'], 'jash.number.binary64.v1')
        _, impact, _, _ = self.report('brain', 'impact', 'file.c')
        self.assertEqual(impact['basis'], 'synthetic-one-mutation-feature-vector')
        self.assertFalse(impact['verified_blast_radius'])
        self.assertTrue(impact['state_advanced'])
        self.assertEqual(impact['risk_estimated']['schema'], 'jash.number.binary64.v1')

    def test_brain_learning_admission_and_exact_argument_validation(self):
        before = {p.name: p.read_bytes() for p in (self.home / '.jash').glob('liquid*') if p.is_file()}
        denied = self.runj([], code='brain --data learn git > /dev/full', ok=False)
        self.assertEqual(denied.returncode, 125)
        after = {p.name: p.read_bytes() for p in (self.home / '.jash').glob('liquid*') if p.is_file()}
        self.assertEqual(after, before)
        _, trained, _, _ = self.report('brain', 'learn', 'git status', 'rc=0', 'next=git diff', 'intent=explore', 'reward=1')
        self.assertEqual(trained['input'], 'git status')
        self.assertEqual(trained['model']['examples_seen'], 1)
        self.assertTrue(trained['persisted'])
        _, persisted, _, _ = self.report('brain', 'status')
        self.assertEqual(persisted['model']['examples_seen'], 1)
        failed, value, _, terminal = self.report('brain', 'learn', 'git', 'intent=999', ok=False)
        self.assertEqual(failed.returncode, 2)
        self.assertEqual(value['error'], 'invalid_intent_class')
        self.assertEqual(terminal['state'], 2)

    def test_brain_each_store_failure_is_reflected_in_native_terminal(self):
        for basename in ('liquid_weights.bin', 'liquid_stats.bin', 'liquid_cmd_hist.bin'):
            with self.subTest(file=basename):
                self.env.update(LD_PRELOAD=str(self.fault), JASH_TEST_MODEL_FAIL_WRITE=basename)
                failed, value, _, terminal = self.report('brain', 'learn', 'git', ok=False)
                self.assertEqual(failed.returncode, 1)
                self.assertFalse(value['ok'])
                self.assertFalse(value['persisted'])
                self.assertTrue(value['state_advanced'])
                self.assertEqual(terminal['state'], 2)
                self.env.pop('JASH_TEST_MODEL_FAIL_WRITE'); self.env.pop('LD_PRELOAD')

    def test_published_schemas_reject_missing_and_invented_fields(self):
        cases = [('adopt', 'status'), ('agent-guard', 'status'), ('stance', 'list'),
                 ('llm-invoice', 'status'), ('token', 'count'), ('brain', 'intent', 'fix'),
                 ('prompt', 'stats'), ('forge', 'description'), ('learn', 'list'),
                 ('preference', 'list'), ('mistake', 'list'), ('reflect', 'improve', 'apply', 'proposal'),
                 ('risk', 'assess'), ('predict', 'ls')]
        for command, *arguments in cases:
            with self.subTest(command=command):
                _, value, _, _ = self.report(command, *arguments)
                validator = semantic_validator(command)
                extra = dict(value, invented_qualification=True)
                self.assertTrue(list(validator.iter_errors(extra)))
                missing = dict(value); del missing['schema']
                self.assertTrue(list(validator.iter_errors(missing)))
                key = next(key for key in value if key != 'schema')
                missing = dict(value); del missing[key]
                self.assertTrue(list(validator.iter_errors(missing)))


if __name__ == '__main__':
    unittest.main()
