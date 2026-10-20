"""Direct recipe providers, complete admission and real per-step effects."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

import test_structured_models as models
from semantic_schema_fixture import semantic_validator, validate_semantic
from test_structured_pipe import native_rows


class StructuredRecipes(unittest.TestCase):
    setUp = models.StructuredModels.setUp
    runj = models.StructuredModels.runj

    @classmethod
    def setUpClass(cls):
        temporary = tempfile.TemporaryDirectory(prefix='jash-recipe-fault-')
        cls.addClassCleanup(temporary.cleanup)
        cls.fault = Path(temporary.name) / 'fault.so'
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        str(models.ROOT / 'tests/recipe_io_faults.c'), '-ldl', '-o', str(cls.fault)], check=True)

    def report(self, command, *arguments, mode='--data', status=0):
        result = self.runj([command, mode, *arguments], ok=False)
        self.assertEqual(result.returncode, status, (result.stdout[:300], result.stderr))
        if mode == '--json':
            values = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
            raw = terminal = None
        else:
            raw, terminal = native_rows(result.stdout)
            budget = self.env.pop('JASH_LLM_BUDGET', None)
            try: inspected = self.runj(['json', '--inspect-data'], data=result.stdout)
            finally:
                if budget is not None: self.env['JASH_LLM_BUDGET'] = budget
            values = json.loads(inspected.stdout)['items']
            self.assertEqual(terminal['producer_status'], status)
            self.assertEqual(terminal['state'], 0 if not status else 2)
        for value in values: validate_semantic(value)
        self.assertTrue(values)
        return values, raw, terminal, result

    def chain(self, entries):
        self.env['JASH_RECALL_GLOBAL'] = '1'
        data = bytearray()
        for sequence, timestamp, status, command in entries:
            prefix = f'{sequence} {timestamp}.123456789 ' + '0' * 64 + f' tag=- rc={status} cwd=/recorded repo=fixture dur=19 sess=fixture cmd='
            data.extend(prefix.encode() + command + b'\n')
        (self.home / '.jash/chain.log').write_bytes(data)

    def recipe(self, name, steps, params=(), session='fixture'):
        directory = self.home / '.jash/recipes'; directory.mkdir(exist_ok=True)
        path = directory / (name + '.jashrc')
        content = f'name {name}\ncreated 1700000000\nsession {session}\nrange 1 2\n'
        content += ''.join(f'param {p}\n' for p in params)
        content += ''.join(f'{kind} {command}\n' for kind, command in steps)
        path.write_text(content)
        return path

    def test_list_missing_and_show_exact_metadata_without_creation(self):
        for mode in ('--json', '--data', '--data-v2'):
            values, _, _, _ = self.report('recipe', mode=mode)
            self.assertTrue(values[0]['complete'])
            self.assertEqual(values[0]['recipes'], [])
        self.assertFalse((self.home / '.jash/recipes').exists())
        self.recipe('quoted', [('C', "echo 'literal ∞'"), ('A', 'expect file target')], ['TARGET'])
        values, raw, _, _ = self.report('recipe', 'show', 'quoted')
        self.assertEqual(values[0]['recipe']['steps'][0]['command'], "echo 'literal ∞'")
        self.assertEqual(values[0]['recipe']['parameters'], ['TARGET'])
        self.assertEqual(raw[0][0]['recipe'][1]['source'][1]['path'][0], 9)
        self.assertFalse((self.home / '.jash/recipes/.memory-lock').exists())

    def test_distill_exact_range_classification_and_atomic_candidate(self):
        first = 9007199254740993
        self.chain([(first, 1700000000, 1, b'touch target'),
                    (first + 1, 1700000001, 0, b'touch target'),
                    (first + 2, 1700000002, 0, b'expect file target'),
                    (first + 3, 1700000003, 0, b'echo observed')])
        values, raw, _, _ = self.report('distill', 'build', '--from', str(first), '--to', str(first + 3),
                                         '--param', 'TARGET=target', '--explain')
        value = values[0]
        self.assertEqual(value['candidate']['range'], {'from': first, 'to': first + 3})
        self.assertEqual([x['reason'] for x in value['decisions']], ['read-dropped', 'fix', 'assertion', 'read-dropped'])
        self.assertEqual([x['command'] for x in value['candidate']['steps']], ['touch ${TARGET}', 'expect file ${TARGET}'])
        self.assertFalse(value['classification']['causation_verified'])
        self.assertFalse(value['source']['cryptographically_verified'])
        self.assertTrue(value['explain_requested'])
        self.assertTrue(value['publication']['applied']); self.assertTrue(value['publication']['durable'])
        self.assertEqual(raw[0][0]['candidate'][1]['range'][1]['from'][0], 3)
        stored = (self.home / '.jash/recipes/build.jashrc').read_text()
        self.assertIn('C touch ${TARGET}\nA expect file ${TARGET}\n', stored)
        shown = self.report('recipe', 'show', 'build')[0][0]['recipe']
        self.assertEqual(shown['steps'], value['candidate']['steps'])

    def test_distill_input_failures_never_publish_truncated_recipe(self):
        self.chain([(1, 1700000000, 0, b'echo ' + b'x' * 1019)])
        self.report('distill', 'too-long', '--keep-reads', status=75)
        self.assertFalse((self.home / '.jash/recipes').exists())
        self.chain([(1, 1700000000, 0, b'expect file x')])
        with (self.home / '.jash/chain.log').open('ab') as file: file.write(b'not a record\n')
        self.report('distill', 'broken', status=1)
        self.chain([(1, 1700000000, 0, b'expect file ' + b'a' * 300)])
        self.report('distill', 'expanded', '--param', 'LONG_PARAMETER=a', status=75)
        self.assertFalse((self.home / '.jash/recipes').exists())
        self.chain([(2, 1700000001, 0, b'expect file x'), (1, 1700000000, 0, b'expect file y')])
        self.report('distill', 'backwards', status=1)
        self.report('distill', '..', status=2)

    def test_run_real_current_state_literal_bindings_and_output_destination(self):
        (self.work / 'sub').mkdir()
        self.recipe('current', [('C', 'export RECIPE_STATE=actual'), ('C', 'cd sub'),
                                ('C', 'touch ${TARGET}'), ('C', 'echo $RECIPE_STATE'),
                                ('C', '/usr/bin/printf external-output'), ('A', 'expect file ${TARGET}')], ['TARGET'])
        for mode in ('--json', '--data', '--data-v2'):
            values, _, _, result = self.report('recipe', 'run', 'current', "TARGET='quoted file'", 'UNDECLARED=ignored', mode=mode)
            self.assertEqual([x['row_kind'] for x in values], ['checkpoint'] + ['step'] * 6 + ['result'])
            self.assertTrue(values[0]['checkpoint']['applied']); self.assertTrue(values[0]['checkpoint']['durable'])
            self.assertTrue(values[-1]['completed'])
            self.assertEqual(values[-1]['ignored_binding_names'], ['UNDECLARED'])
            self.assertEqual(values[-1]['checkpoint_name'], 'recipe-current')
            self.assertEqual(values[-1]['output_destination'], 'stderr')
            self.assertIn(b'actual', result.stderr); self.assertIn(b'external-output', result.stderr)
            self.assertTrue((self.work / 'sub/quoted file').exists())

    def test_command_failure_continues_but_assertion_stops_with_actual_status(self):
        self.recipe('command-fail', [('C', 'false'), ('C', 'touch after-command')])
        values, _, _, _ = self.report('recipe', 'run', 'command-fail')
        self.assertEqual(values[1]['command_status'], 1)
        self.assertEqual(values[-1]['failed_commands'], 1)
        self.assertTrue(values[-1]['completed']); self.assertTrue((self.work / 'after-command').exists())
        self.recipe('assert-fail', [('C', 'touch before-assert'), ('A', 'false'), ('C', 'touch after-assert')])
        values, _, terminal, _ = self.report('recipe', 'run', 'assert-fail', status=1)
        self.assertEqual(values[-1]['assertion_failed_index'], 2)
        self.assertEqual(values[-1]['steps_completed'], 2)
        self.assertEqual(terminal['state'], 2)
        self.assertFalse((self.work / 'after-assert').exists())
        script = self.work / 'signal-status'; script.write_text('#!/bin/sh\nexit 143\n'); script.chmod(0o700)
        self.recipe('signal', [('A', './signal-status')])
        values, _, terminal, _ = self.report('recipe', 'run', 'signal', status=143)
        self.assertEqual(values[1]['command_status'], 143)
        self.assertEqual(terminal['state'], 2)  # observed status is not a cooperative cancellation

    def test_checkpoint_failure_stops_before_first_recipe_effect(self):
        self.recipe('check', [('C', 'touch must-not-run')])
        self.env.update(LD_PRELOAD=str(self.fault), JASH_TEST_RECIPE_FAULT='rename-checkpoint')
        values, _, _, _ = self.report('recipe', 'run', 'check', status=1)
        self.assertFalse(values[0]['checkpoint']['applied'])
        self.assertEqual(values[-1]['steps_completed'], 0)
        self.assertIsNone(values[-1]['checkpoint_name'])
        self.assertFalse((self.work / 'must-not-run').exists())
        self.env['JASH_TEST_RECIPE_FAULT'] = 'sync-checkpoint'
        values, _, _, _ = self.report('recipe', 'run', 'check', status=1)
        self.assertTrue(values[0]['checkpoint']['applied'])
        self.assertFalse(values[0]['checkpoint']['durable'])
        self.assertEqual(values[-1]['checkpoint_name'], 'recipe-check')
        self.assertFalse((self.work / 'must-not-run').exists())

    def test_store_rename_and_directory_sync_are_distinct_outcomes(self):
        self.chain([(1, 1700000000, 0, b'expect file target')])
        self.env.update(LD_PRELOAD=str(self.fault), JASH_TEST_RECIPE_FAULT='rename-recipe')
        values, _, _, _ = self.report('distill', 'publication', status=1)
        self.assertFalse(values[0]['publication']['applied'])
        self.assertFalse((self.home / '.jash/recipes/publication.jashrc').exists())
        self.env['JASH_TEST_RECIPE_FAULT'] = 'sync-recipe'
        values, _, _, _ = self.report('distill', 'publication', status=1)
        self.assertTrue(values[0]['publication']['applied']); self.assertFalse(values[0]['publication']['durable'])
        self.assertTrue((self.home / '.jash/recipes/publication.jashrc').exists())

    def test_sink_failure_between_steps_preserves_first_and_prevents_second(self):
        self.recipe('effects', [('C', 'touch first-effect'), ('C', 'touch second-effect')])
        self.env.update(LD_PRELOAD=str(self.fault), JASH_TEST_RECIPE_FAULT='sink-after-first',
                        JASH_TEST_RECIPE_FIRST_EFFECT=str(self.work / 'first-effect'))
        result = self.runj(['recipe', '--data', 'run', 'effects'], ok=False)
        self.assertEqual(result.returncode, 125)
        self.assertTrue((self.work / 'first-effect').exists())
        self.assertFalse((self.work / 'second-effect').exists())
        with self.assertRaises((ValueError, AssertionError, IndexError)): native_rows(result.stdout)

    def test_complete_input_admission_precedes_checkpoint_and_execution(self):
        path = self.recipe('invalid', [('C', 'touch early'), ('C', 'echo ${BIG}' * 70)], ['BIG'])
        self.report('recipe', 'run', 'invalid', 'BIG=' + 'x' * 255, status=75)
        self.assertFalse((self.home / '.jash/checkpoints.tsv').exists())
        self.assertFalse((self.work / 'early').exists())
        for broken in (b'name invalid\nname invalid\n', path.read_bytes() + b'unknown field\n', b'x' * (512 * 1024 + 1)):
            path.write_bytes(broken)
            self.report('recipe', 'run', 'invalid', status=75 if len(broken) > 512 * 1024 else 1)
            self.assertFalse((self.work / 'early').exists())

    def test_budget_and_sink_admission_precede_store_and_callback_effects(self):
        self.recipe('safe', [('C', 'touch safe-effect')])
        self.chain([(1, 1700000000, 0, b'expect file target')])
        for mode in ('--json', '--data', '--data-v2'):
            for argv in (['recipe', mode, 'run', 'safe'], ['distill', mode, 'new']):
                result = self.runj([], code=shlex.join(argv) + ' > /dev/full', ok=False)
                self.assertEqual(result.returncode, 125)
        self.assertFalse((self.work / 'safe-effect').exists())
        self.assertFalse((self.home / '.jash/recipes/new.jashrc').exists())
        self.env['JASH_LLM_BUDGET'] = '1'
        for argv in (['recipe', '--json', 'run', 'safe'], ['distill', '--json', 'new']):
            failed = self.runj(argv, ok=False)
            self.assertEqual(failed.returncode, 75)
            error = json.loads(failed.stdout)
            self.assertEqual(error['schema'], 'jash.builtin.error.v1')
            self.assertEqual(error['error']['code'], 'output_budget_exceeded')
            self.assertEqual(error['command_status'], 75)
        self.assertFalse((self.work / 'safe-effect').exists())
        self.report('recipe', 'run', 'safe')
        self.report('distill', 'new')

    def test_nested_redirects_use_registered_backups_and_restore_structured_stdout(self):
        self.recipe('redirects', [('C', 'echo file-content > nested-file'),
                                  ('C', 'echo screen-content'), ('A', 'expect file nested-file')])
        values, _, _, result = self.report('recipe', 'run', 'redirects')
        self.assertTrue(values[-1]['completed'])
        self.assertEqual((self.work / 'nested-file').read_bytes(), b'file-content\n')
        self.assertNotIn(b'file-content', result.stderr)
        self.assertIn(b'screen-content', result.stderr)

    def test_forget_real_store_and_untrusted_sources_fail_explicitly(self):
        path = self.recipe('remove', [('C', 'true')])
        values, _, _, _ = self.report('recipe', 'forget', 'remove')
        self.assertTrue(values[0]['publication']['applied']); self.assertFalse(path.exists())
        self.report('recipe', 'forget', 'remove', status=2)
        fifo = self.home / '.jash/recipes/pipe.jashrc'; os.mkfifo(fifo)
        self.report('recipe', 'show', 'pipe', status=1)
        values, _, _, _ = self.report('recipe', 'list', status=1)
        self.assertFalse(values[0]['complete']); self.assertEqual(values[0]['recipes'], [])

    def test_published_schemas_reject_missing_and_invented_fields(self):
        self.chain([(1, 1700000000, 0, b'expect file target')])
        value = self.report('distill', 'schema')[0][0]
        for family, original in [('distill', value), ('recipe', self.report('recipe', 'show', 'schema')[0][0])]:
            validator = semantic_validator(family)
            self.assertTrue(list(validator.iter_errors({**original, 'invented': True})))
            broken = dict(original); del broken['operation']
            self.assertTrue(list(validator.iter_errors(broken)))


if __name__ == '__main__':
    unittest.main()
