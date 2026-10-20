"""Gap observations, actual proofs and checked per-effect receipts."""
import fcntl
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import time
import unittest

import test_structured_models as models
from semantic_schema_fixture import semantic_validator, validate_semantic
from test_structured_pipe import native_rows


class StructuredGap(unittest.TestCase):
    setUp = models.StructuredModels.setUp
    runj = models.StructuredModels.runj

    @classmethod
    def setUpClass(cls):
        temporary = tempfile.TemporaryDirectory(prefix='jash-gap-fault-')
        cls.addClassCleanup(temporary.cleanup)
        cls.fault = Path(temporary.name) / 'fault.so'
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        str(models.ROOT / 'tests/gap_io_faults.c'), '-ldl', '-o', str(cls.fault)], check=True)

    def report(self, *arguments, mode='--data', status=0):
        result = self.runj(['gap', mode, *arguments], ok=False)
        self.assertEqual(result.returncode, status, result.stderr)
        if mode == '--json':
            values = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
            raw = terminal = None
        else:
            raw, terminal = native_rows(result.stdout)
            env = self.env.pop('JASH_LLM_BUDGET', None)
            try:
                inspected = self.runj(['json', '--inspect-data'], data=result.stdout)
            finally:
                if env is not None: self.env['JASH_LLM_BUDGET'] = env
            values = json.loads(inspected.stdout)['items']
            self.assertEqual(terminal['producer_status'], status)
            self.assertEqual(terminal['state'], 0 if not status else 2)
        for value in values: validate_semantic(value)
        self.assertTrue(values)
        return values, raw, terminal

    def test_missing_store_query_never_creates_or_invents_gaps(self):
        for mode in ('--json', '--data', '--data-v2'):
            values, raw, _ = self.report('list', mode=mode)
            self.assertEqual(values[0]['entries'], [])
            self.assertTrue(values[0]['complete'])
            self.assertFalse(values[0]['sources']['local']['present'])
            if raw: self.assertEqual(raw[0][0]['entries'][0], 11)
        self.assertFalse((self.home / '.jash/gaps.tsv').exists())
        self.assertFalse((self.home / '.jash/gaps.lock').exists())
        self.assertFalse((self.home / '.jash/.memory-lock').exists())

    def test_report_dedup_close_and_reopen_match_actual_store(self):
        for mode in ('--json', '--data', '--data-v2'):
            values, raw, _ = self.report('report', 'literal " issue ∞', '--expected', 'expected " text', '--prove', 'file missing', mode=mode)
            result = values[-1]
            self.assertEqual(result['entry']['tried'], 'literal " issue ∞')
            self.assertEqual(values[0]['action'], 'publish-local')
            self.assertTrue(values[0]['applied'])
            self.assertTrue(values[0]['durable'])
            self.assertEqual(values[1]['action'], 'attest')
            self.assertIsNone(values[1]['persistence_verified'])
            if raw: self.assertEqual(raw[-1][0]['entry'][1]['count'][0], 3)
        sig = result['entry']['sig']
        self.assertEqual(result['entry']['count'], 3)
        closed, _, _ = self.report('close', sig)
        self.assertEqual(closed[-1]['entry']['status'], 'fixed')
        self.assertEqual([row['action'] for row in closed[:-1]], ['publish-local'])
        self.assertEqual(self.report('list')[0][0]['entries'], [])
        reopened, _, _ = self.report('literal " issue ∞', '--expected', 'expected " text')
        self.assertEqual(reopened[-1]['entry']['status'], 'open')
        self.assertEqual(reopened[-1]['entry']['count'], 4)
        self.assertEqual(reopened[-1]['entry']['prove'], 'file missing')

    def test_exact_u64_count_and_ambiguous_legacy_fields(self):
        values, _, _ = self.report('one')
        sig = values[-1]['entry']['sig']
        path = self.home / '.jash/gaps.tsv'
        path.write_text(f'{sig}\topen\t9007199254740993\t1\t2\tone\t\t\n')
        listed, raw, _ = self.report('list')
        self.assertEqual(listed[0]['entries'][0]['count'], 9007199254740993)
        self.assertEqual(raw[0][0]['entries'][1][0][1]['count'][0], 3)
        for text in ('ambiguous\tfield', 'ambiguous\nfield', 'x' * 512):
            before = path.read_bytes()
            invalid, _, _ = self.report('report', text, status=2)
            self.assertEqual(invalid[0]['error'], 'invalid_arguments')
            self.assertEqual(path.read_bytes(), before)
        with path.open('ab') as file: file.write(path.read_bytes())
        failed, _, _ = self.report('list', status=1)
        self.assertFalse(failed[0]['complete'])
        self.assertEqual(failed[0]['entries'], [])
        self.assertIsNone(failed[0]['sources']['local']['records'])

    def test_fleet_publication_and_partial_failure_have_independent_receipts(self):
        (self.home / '.jash/mesh').mkdir()
        values, _, _ = self.report('shared issue')
        self.assertEqual([row['action'] for row in values[:-1]], ['publish-local', 'publish-fleet', 'attest'])
        self.assertEqual(values[-1]['entry']['fleet'], 1)
        self.env.update(LD_PRELOAD=str(self.fault), JASH_TEST_GAP_FAULT='rename-fleet')
        values, _, _ = self.report('shared issue', status=1)
        self.assertTrue(values[0]['applied'])
        self.assertFalse(values[1]['applied'])
        self.assertEqual(values[-1]['entry']['count'], 2)
        self.assertEqual(values[-1]['entry']['fleet'], 1)
        self.assertEqual([row['action'] for row in values[:-1]], ['publish-local', 'publish-fleet'])

    def test_rename_and_durability_failures_do_not_claim_success(self):
        self.env.update(LD_PRELOAD=str(self.fault), JASH_TEST_GAP_FAULT='rename-local')
        values, _, _ = self.report('unpublished', status=1)
        self.assertFalse(values[0]['applied'])
        self.assertIsNone(values[-1]['entry'])
        self.assertEqual(values[-1]['still_open'], 0)
        self.assertFalse((self.home / '.jash/gaps.tsv').exists())
        self.env['JASH_TEST_GAP_FAULT'] = 'directory-sync-local'
        values, _, _ = self.report('published but not durable', status=1)
        self.assertTrue(values[0]['applied'])
        self.assertFalse(values[0]['durable'])
        self.assertEqual(values[-1]['entry']['tried'], 'published but not durable')
        self.assertTrue((self.home / '.jash/gaps.tsv').exists())

    def test_verify_executes_real_proofs_and_only_publishes_passing_ones(self):
        self.report('first', '--prove', 'file present')
        self.report('second', '--prove', 'file missing')
        (self.work / 'present').write_bytes(b'real')
        values, _, _ = self.report('verify', status=1)
        self.assertEqual([row['command_status'] for row in values if row.get('action') == 'expect'], [0, 1])
        self.assertEqual(values[-1]['with_proof'], 2)
        self.assertEqual(values[-1]['fixed_now'], 1)
        self.assertEqual(values[-1]['still_open'], 1)
        observed = self.report('list', '--all')[0][0]['entries']
        self.assertEqual({row['tried']: row['status'] for row in observed}, {'first': 'fixed', 'second': 'open'})

    def test_sink_failure_after_first_callback_stops_before_second(self):
        self.report('first', '--prove', 'cmd "touch first-effect" rc 0')
        self.report('second', '--prove', 'cmd "touch second-effect" rc 0')
        self.env.update(LD_PRELOAD=str(self.fault), JASH_TEST_GAP_FAULT='sink-after-proof',
                        JASH_TEST_GAP_FIRST_PROOF=str(self.work / 'first-effect'))
        failed = self.runj(['gap', '--data', 'verify'], ok=False)
        self.assertEqual(failed.returncode, 125)
        self.assertTrue((self.work / 'first-effect').exists())
        self.assertFalse((self.work / 'second-effect').exists())
        self.assertNotIn(b'\tfixed\t', (self.home / '.jash/gaps.tsv').read_bytes())

    def test_sink_and_json_budget_admission_precede_mutation(self):
        for mode in ('--json', '--data'):
            failed = self.runj([], code=shlex.join(['gap', mode, 'report', 'not recorded']) + ' > /dev/full', ok=False)
            self.assertEqual(failed.returncode, 125)
            self.assertFalse((self.home / '.jash/gaps.tsv').exists())
            self.assertFalse((self.home / '.jash/gaps.lock').exists())
        self.env['JASH_LLM_BUDGET'] = '1'
        failed = self.runj(['gap', '--json', 'not recorded'], ok=False)
        self.assertEqual(failed.returncode, 75)
        self.assertEqual(json.loads(failed.stdout)['command_status'], 75)
        self.assertFalse((self.home / '.jash/gaps.tsv').exists())
        self.assertTrue(self.report('native recorded')[0][-1]['ok'])

    def test_real_legacy_writer_lock_serializes_without_overwriting(self):
        path = self.home / '.jash/gaps.lock'
        with path.open('w') as owner:
            fcntl.flock(owner, fcntl.LOCK_EX)
            child = subprocess.Popen([str(models.JASH), '-c', 'gap --data locked'], cwd=self.work,
                                     env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                time.sleep(0.1)
                self.assertIsNone(child.poll())
                self.assertFalse((self.home / '.jash/gaps.tsv').exists())
                fcntl.flock(owner, fcntl.LOCK_UN)
                output, error = child.communicate(timeout=10)
                self.assertEqual(child.returncode, 0, error)
                self.assertEqual(native_rows(output)[1]['state'], 0)
            finally:
                if child.poll() is None: child.kill(); child.communicate()

    def test_scan_uses_complete_chain_and_labels_observed_heuristic(self):
        self.env['JASH_RECALL_GLOBAL'] = '1'
        path = self.home / '.jash/chain.log'
        def row(sequence, command):
            return f'{sequence} 1700000000.000000001 '.encode() + b'0' * 64 + b' tag=- rc=2 cwd=/ repo=fixture dur=1 sess=fixture cmd=' + command + b'\n'
        path.write_bytes(row(1, b'git unknown first') + row(2, b'git unknown second'))
        values, _, _ = self.report('scan')
        self.assertEqual(values[0]['clusters'], [{'misuse': 'git unknown', 'count': 2}])
        self.assertFalse(values[0]['capability_gap_confirmed'])
        path.write_bytes(row(1, b'x' * 64))
        failed, _, _ = self.report('scan', status=75)
        self.assertFalse(failed[0]['complete'])
        self.assertEqual(failed[0]['clusters'], [])
        self.assertIsNone(failed[0]['total_status_two'])

    def test_nonregular_input_and_schema_negative_controls(self):
        os.mkfifo(self.home / '.jash/gaps.tsv')
        values, _, _ = self.report('list', status=1)
        self.assertFalse(values[0]['complete'])
        validator = semantic_validator('gap')
        self.assertTrue(list(validator.iter_errors({**values[0], 'invented': True})))
        broken = dict(values[0]); del broken['sources']
        self.assertTrue(list(validator.iter_errors(broken)))


if __name__ == '__main__':
    unittest.main()
