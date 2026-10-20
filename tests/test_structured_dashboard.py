"""Dashboard counters follow real complete sources and preserve unknown values."""
import os
import unittest

import test_structured_models as models
from semantic_schema_fixture import semantic_validator


class StructuredDashboard(unittest.TestCase):
    setUp = models.StructuredModels.setUp
    runj = models.StructuredModels.runj
    report = models.StructuredModels.report

    def chain(self, entries):
        self.env['JASH_RECALL_GLOBAL'] = '1'
        data = bytearray()
        for sequence, timestamp, status, command in entries:
            prefix = f'{sequence} {timestamp}.123456789 ' + '0' * 64 + f' tag=- rc={status} cwd=/recorded_dir repo=fixture dur=19 sess=fixture cmd='
            data.extend(prefix.encode() + command + b'\n')
        (self.home / '.jash/chain.log').write_bytes(data)

    def test_missing_sources_are_empty_and_rates_remain_unknown(self):
        for mode in ('--json', '--data', '--data-v2'):
            _, value, native, _ = self.report('dashboard', mode=mode)
            self.assertTrue(value['complete'])
            self.assertEqual(value['commands'], 0)
            self.assertEqual(value['fleet'], 0)
            self.assertEqual(value['checkpoints'], 0)
            self.assertIsNone(value['fail_pct'])
            self.assertIsNone(value['adoption']['pct'])
            self.assertIsNone(value['uptime_s'])
            self.assertFalse(value['sources']['chain']['present'])
            self.assertFalse(value['sources']['mesh']['present'])
            self.assertFalse(value['sources']['checkpoints']['present'])
            if native:
                self.assertEqual(native['commands'][0], 3)
                self.assertEqual(native['fail_pct'][0], 0)
        self.assertFalse((self.home / '.jash/mesh').exists())
        self.assertFalse((self.home / '.jash/checkpoints.tsv').exists())

    def test_actual_chain_observations_are_not_attestation_claims(self):
        self.chain([(9007199254740993, 1700000000, 0, b"eval 'recall recent'"),
                    (9007199254740994, 1700000030, 7, b'unknown ' + b'x' * 5000),
                    (9007199254740995, 1700000060, 0, b"' ")])
        (self.home / '.jash/checkpoints.tsv').write_bytes(b'one\ntwo\n')
        for mode in ('--json', '--data-v2'):
            _, value, _, _ = self.report('dashboard', mode=mode)
            self.assertEqual(value['commands'], 3)
            self.assertEqual(value['failed'], 1)
            self.assertEqual(value['fail_pct'], 33)
            self.assertEqual(value['adoption']['features'], {'recall': 1})
            self.assertEqual(value['uptime_s'], 60)
            self.assertEqual(value['checkpoints'], 2)
            self.assertEqual(value['sources']['checkpoints']['basis'], 'recorded-metadata-lines')
            self.assertFalse(value['sources']['chain']['cryptographically_verified'])
            self.assertEqual(value['consistency'], 'independent-source-observations')

    def test_invalid_chain_does_not_count_only_the_valid_prefix(self):
        self.chain([(1, 1700000000, 0, b'valid')])
        with (self.home / '.jash/chain.log').open('ab') as file:
            file.write(b'2 malformed row\n')
        failed, value, _, terminal = self.report('dashboard', ok=False)
        self.assertEqual(failed.returncode, 1)
        self.assertFalse(value['complete'])
        self.assertFalse(value['sources']['chain']['read_complete'])
        self.assertIsNone(value['commands'])
        self.assertIsNone(value['failed'])
        self.assertIsNone(value['adoption']['features'])
        self.assertEqual(terminal['state'], 2)

    def test_backwards_timestamps_are_not_a_fabricated_uptime(self):
        self.chain([(1, 1700000060, 0, b'one'), (2, 1700000000, 0, b'two')])
        _, value, _, _ = self.report('dashboard')
        self.assertTrue(value['complete'])
        self.assertEqual(value['commands'], 2)
        self.assertIsNone(value['uptime_s'])

    def test_mesh_counts_come_from_native_snapshot_and_keep_unknown_pid(self):
        self.runj(['mesh', '--data', 'register', '--id', 'worker', '--model', 'fixture'])
        self.runj(['mesh', '--data', 'add', 'actual task'])
        _, value, native, _ = self.report('dashboard')
        self.assertEqual(value['fleet'], 1)
        self.assertEqual(value['fleet_observation']['unknown'], 1)
        self.assertEqual(value['fleet_observation']['live'], 0)
        self.assertEqual(value['board']['total'], 1)
        self.assertEqual(value['board']['ready'], 1)
        self.assertEqual(native['fleet'][0], 3)
        self.assertTrue(value['sources']['mesh']['complete'])
        path = self.home / '.jash/mesh/workers/worker.tsv'
        path.write_bytes(b'broken\n')
        failed, value, _, terminal = self.report('dashboard', ok=False)
        self.assertNotEqual(failed.returncode, 0)
        self.assertFalse(value['sources']['mesh']['complete'])
        self.assertIsNone(value['fleet'])
        self.assertIsNone(value['board']['total'])
        self.assertEqual(terminal['state'], 2)

    def test_nonregular_checkpoint_source_fails_without_waiting(self):
        os.mkfifo(self.home / '.jash/checkpoints.tsv')
        failed, value, _, terminal = self.report('dashboard', ok=False)
        self.assertEqual(failed.returncode, 1)
        self.assertFalse(value['sources']['checkpoints']['complete'])
        self.assertIsNone(value['checkpoints'])
        self.assertEqual(terminal['state'], 2)

    def test_arguments_and_schema_negative_controls(self):
        failed, value, _, terminal = self.report('dashboard', '--unknown', ok=False)
        self.assertEqual(failed.returncode, 2)
        self.assertEqual(value['error'], 'invalid_arguments')
        self.assertEqual(terminal['state'], 2)
        _, good, _, _ = self.report('dashboard')
        validator = semantic_validator('dashboard')
        self.assertTrue(list(validator.iter_errors({**good, 'invented': True})))
        del good['sources']
        self.assertTrue(list(validator.iter_errors(good)))


if __name__ == '__main__':
    unittest.main()
