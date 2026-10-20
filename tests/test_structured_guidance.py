"""Actual guide metadata and child observations, not parsed human reports."""
import json
from decimal import Decimal
import os
import shlex
import sys
import unittest

import test_structured_models as models


class StructuredGuidance(unittest.TestCase):
    setUp = models.StructuredModels.setUp
    runj = models.StructuredModels.runj
    report = models.StructuredModels.report

    def test_guide_uses_compiled_registry_and_env_documentation(self):
        for mode in ('--json', '--data', '--data-v2'):
            _, value, native, _ = self.report('agent-guide', mode=mode)
            self.assertTrue(value['metadata_only'])
            self.assertEqual(value['qualification_source'], '--contract-json')
            entries = {item['name']: item for item in value['builtins']}
            self.assertGreater(len(entries), 100)
            self.assertTrue(entries['echo']['argv_handler'])
            self.assertIn('JASH_SIGIL', [item['name'] for item in value['environment']])
            self.assertIsNone(value['artifact'])
            self.assertIsNone(value['document'])
            self.assertFalse(value['effects_may_have_applied'])
            if native:
                self.assertEqual(native['builtins'][0], 11)

    def test_guide_save_receipt_matches_exact_published_artifact(self):
        expected = self.runj(['agent-guide', '--markdown']).stdout
        _, saved, native, _ = self.report('agent-guide', '--save', '--markdown')
        artifact = saved['artifact']
        self.assertTrue(artifact['published'])
        self.assertTrue(artifact['durable'])
        self.assertEqual(artifact['bytes'], len(expected))
        self.assertEqual(saved['document'].encode(), expected)
        self.assertEqual((self.home / '.jash/agent_guide.md').read_bytes(), expected)
        self.assertEqual(native['artifact'][1]['bytes'][0], 4)
        self.assertEqual(native['artifact'][1]['path'][0], 9)

    def test_guide_rejects_unsafe_target_without_claiming_publication(self):
        victim = self.work / 'outside'; victim.write_bytes(b'keep')
        (self.home / '.jash/agent_guide.md').symlink_to(victim)
        failed, value, _, terminal = self.report('agent-guide', '--save', ok=False)
        self.assertEqual(failed.returncode, 1)
        self.assertEqual(terminal['state'], 2)
        self.assertFalse(value['artifact']['published'])
        self.assertFalse(value['artifact']['durable'])
        self.assertEqual(victim.read_bytes(), b'keep')

    def test_cost_dry_run_preserves_literal_argv_and_is_only_lower_bound(self):
        args = [sys.executable, '-c', 'from pathlib import Path; Path("unexpected").touch()', 'quoted " ∞']
        for mode in ('--json', '--data', '--data-v2'):
            _, value, native, _ = self.report('cost', '--dry-run', '--', *args, mode=mode)
            self.assertEqual(value['argv'], args)
            self.assertFalse(value['executed'])
            self.assertEqual(value['basis'], 'argv-bytes-lower-bound')
            self.assertEqual(value['bytes'], sum(len(arg.encode()) + 1 for arg in args))
            self.assertFalse(value['price_policy']['current_market_price'])
            self.assertIsNone(value['command_status'])
            if native:
                self.assertEqual(native['bytes'][0], 4)
                self.assertEqual(native['duration_ns'][0], 6)
                self.assertEqual(value['total_usd']['coefficient'], 90)
        self.assertFalse((self.work / 'unexpected').exists())

    def test_cost_exec_observes_combined_bytes_and_child_failure(self):
        code = 'import os; os.write(1, b"a\\x00\\xffb"); os.write(2, b"cdef"); raise SystemExit(7)'
        for mode in ('--json', '--data'):
            failed, value, _, terminal = self.report('cost', sys.executable, '-c', code, mode=mode, ok=False)
            self.assertEqual(failed.returncode, 7)
            self.assertTrue(value['executed'])
            self.assertEqual(value['bytes'], 8)
            self.assertEqual(value['tokens_estimated'], 2)
            self.assertEqual(value['command_status'], 7)
            self.assertEqual(value['adapter_status'], 0)
            if terminal:
                self.assertEqual(terminal['state'], 2)
                self.assertEqual(value['input_usd']['value'], 2)
            else:
                exact = json.loads(failed.stdout, parse_float=Decimal)
                self.assertEqual(exact['input_usd'], Decimal('0.000060'))
                self.assertEqual(exact['total_usd'], Decimal('0.000180'))

    def test_cost_signalled_child_is_failed_not_cooperative_cancelled(self):
        code = 'import os, signal; os.kill(os.getpid(), signal.SIGTERM)'
        failed, value, _, terminal = self.report('cost', sys.executable, '-c', code, ok=False)
        self.assertEqual(failed.returncode, 143)
        self.assertEqual(value['command_status'], 143)
        self.assertEqual(terminal['state'], 2)

    def test_sink_admission_prevents_guide_save_and_child_execution(self):
        path = self.home / '.jash/agent_guide.md'; path.write_bytes(b'prior guide')
        command = [sys.executable, '-c', 'from pathlib import Path; Path("unexpected").touch()']
        for mode in ('--json', '--data'):
            for argv in (['agent-guide', mode, '--save'], ['cost', mode, *command]):
                failed = self.runj([], code=shlex.join(argv) + ' > /dev/full', ok=False)
                self.assertEqual(failed.returncode, 125)
                self.assertEqual(path.read_bytes(), b'prior guide')
                self.assertFalse((self.work / 'unexpected').exists())

    def test_presentation_preflight_status_is_75_without_effects(self):
        self.env['JASH_LLM_BUDGET'] = '1'
        command = [sys.executable, '-c', 'from pathlib import Path; Path("unexpected").touch()']
        for argv in (['agent-guide', '--json', '--save'], ['cost', '--json', *command]):
            failed = self.runj(argv, ok=False)
            self.assertEqual(failed.returncode, 75)
            self.assertEqual(json.loads(failed.stdout)['command_status'], 75)
        self.assertFalse((self.home / '.jash/agent_guide.md').exists())
        self.assertFalse((self.work / 'unexpected').exists())


if __name__ == '__main__':
    unittest.main()
