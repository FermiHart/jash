#!/usr/bin/env python3
"""Sigil receipts checked against actual ledger, artifacts and child effects."""
import copy
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

from jsonschema import ValidationError
import native_data_fixture as f
from contract_schema_fixture import projection, validate_contract


class SigilActions(f.NativeQueryCase):
    def setUp(self):
        super().setUp()
        module = f.ROOT / 'modules/sigil/sigil.modjash'
        self.assertTrue(module.is_file(), 'Build the real Sigil module before this gate')
        target = Path(self.env['HOME']) / '.jash/modules'
        target.mkdir(parents=True, mode=0o700)
        shutil.copyfile(module, target / 'sigil.modjash')
        (target / 'sigil.modjash').chmod(0o600)
        self.verifier = f.JASH.parent / 'jef_verify'
        self.assertTrue(self.verifier.is_file(), 'Build the independent jef_verify target')

    def report(self, operation, *, mode='--json', rc=0, strict=False):
        result = self.run_command(['sigil', mode, *operation], strict=strict,
                                  caps=['effect:read', 'effect:mutate', 'effect:exec'])
        self.assertEqual(result.returncode, rc, result.stderr)
        if mode == '--json':
            value = json.loads(result.stdout)
            validate_contract('sigil-replay' if operation[0] == 'replay' else 'sigil-action', value)
            return value
        stream = f.stream(result.stdout, credit_kind='none')
        self.assertEqual(stream.producer, rc)
        self.assertEqual(stream.state, 0 if rc == 0 else 2)
        values = [projection(item) for item in stream.items]
        for value in values:
            validate_contract('sigil-replay-row' if operation[0] == 'replay' else 'sigil-action', value)
        return values if operation[0] == 'replay' else values[0]

    def test_record_preserves_literal_bytes_and_real_chain_sequence(self):
        for index, mode in enumerate(('--json', '--data', '--data-v2')):
            label = 'label with spaces " \\ and newline\n' + str(index)
            result = self.report(['record', label], mode=mode)
            self.assertEqual(result['detail']['label'], label)
            self.assertTrue(result['applied']); self.assertTrue(result['durable'])
            chain = Path(result['detail']['chain'])
            lines = chain.read_text().splitlines()
            self.assertEqual(int(lines[-1].split()[0]), result['detail']['sequence'])
            self.assertIn('cmd=label with spaces " \\\\ and newline\\n' + str(index), lines[-1])
            self.assertTrue(result['detail']['chain_synced'])
            self.assertTrue(result['detail']['ratchet_published'])
        independent = subprocess.run([str(self.verifier), 'chain', str(chain)], capture_output=True)
        self.assertEqual(independent.returncode, 0, independent.stdout + independent.stderr)

    def test_proof_bytes_digest_signature_and_pinned_identity(self):
        self.report(['record', 'proof evidence'])
        for index, form in enumerate(('emit', 'dsse')):
            path = self.work / ('proof with spaces.' + form)
            result = self.report(['proof', form, str(path)], mode='--data' if index else '--json')
            detail = result['detail']; content = path.read_bytes()
            self.assertEqual(detail['path'], str(path))
            self.assertEqual(detail['bytes'], len(content))
            self.assertEqual(detail['sha256'], hashlib.sha256(content).hexdigest())
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            self.assertTrue(result['applied']); self.assertTrue(result['durable'])
            verified = subprocess.run([str(self.verifier), 'proof-pinned', detail['signer_key'], str(path)], capture_output=True)
            self.assertEqual(verified.returncode, 0, verified.stdout + verified.stderr)
            foreign = subprocess.run([str(self.verifier), 'proof-pinned', '0' * 64, str(path)], capture_output=True)
            self.assertNotEqual(foreign.returncode, 0)

    def test_proof_publication_failures_report_observed_stages(self):
        self.report(['record', 'fault evidence'])
        path = self.work / 'proof.dsse'
        for fault in ('write', 'fsync', 'close', 'rename', 'dir-fsync'):
            path.write_bytes(b'previous complete artifact')
            self.env['JASH_SIGIL_TEST_PROOF_FAIL'] = fault
            result = self.report(['proof', 'dsse', str(path)], rc=1, mode='--data')
            self.assertFalse(result['complete']); self.assertFalse(result['durable'])
            self.assertEqual(result['applied'], fault == 'dir-fsync')
            if fault == 'dir-fsync':
                self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), result['detail']['sha256'])
            else:
                self.assertEqual(path.read_bytes(), b'previous complete artifact')
            self.assertEqual(list(self.work.glob('proof.dsse.tmp.*')), [])
        self.env.pop('JASH_SIGIL_TEST_PROOF_FAIL')

    def test_export_receipt_matches_oci_blob_digests_and_exact_manifest(self):
        self.report(['record', 'export evidence'])
        manifest = self.work / 'caller manifest.json'
        manifest.write_bytes(b'{"literal":"$(touch forbidden)","unicode":"\\u221e"}\n')
        directory = self.work / 'layout with spaces'
        result = self.report(['export', str(directory), '--manifest', str(manifest)])
        detail = result['detail']
        self.assertTrue(detail['layout_written']); self.assertTrue(result['applied'])
        self.assertIsNone(result['durable'])
        blobs = directory / 'blobs/sha256'
        chain = (blobs / detail['chain_sha256']).read_bytes()
        self.assertEqual(len(chain), detail['chain_bytes'])
        self.assertEqual(hashlib.sha256(chain).hexdigest(), detail['chain_sha256'])
        for blob in blobs.iterdir():
            self.assertEqual(hashlib.sha256(blob.read_bytes()).hexdigest(), blob.name)
        self.assertTrue((blobs / detail['manifest_sha256']).is_file())
        self.assertFalse((self.work / 'forbidden').exists())

    def test_export_source_error_is_not_successful_absence(self):
        self.report(['record', 'source evidence'])
        manifest = self.work / 'fifo manifest'; os.mkfifo(manifest)
        result = self.report(['export', str(self.work / 'layout'), '--manifest', str(manifest)], rc=1)
        self.assertFalse(result['complete']); self.assertFalse(result['applied'])
        self.assertFalse((self.work / 'layout').exists())

    def test_push_uses_literal_child_argv_and_reports_actual_child_status(self):
        self.report(['record', 'local push evidence'])
        commands = self.work / 'local commands'; commands.mkdir()
        executable = commands / 'oras'
        executable.write_text('#!/bin/sh\nprintf "%s\\0" "$@" > "$ORAS_ARGV"\nprintf child-diagnostic\nexit "$ORAS_STATUS"\n')
        executable.chmod(0o700)
        arguments = self.work / 'actual argv'
        self.env.update(PATH=str(commands) + os.pathsep + self.env['PATH'],
                        ORAS_ARGV=str(arguments), ORAS_STATUS='17')
        result = self.report(['push', 'registry.invalid/team/object'], mode='--data', rc=17)
        detail = result['detail']
        self.assertTrue(detail['layout_written']); self.assertTrue(result['applied'])
        self.assertFalse(detail['remote_state_verified'])
        self.assertEqual(detail['child_exit_code'], 17); self.assertGreater(detail['child_pid'], 0)
        self.assertEqual(arguments.read_bytes().split(b'\0')[:-1],
                         [b'push', b'registry.invalid/team/object:latest', b'--layout', os.fsencode(detail['directory'])])
        arguments.unlink()
        self.report(['push', 'bad; touch injected'], rc=2)
        self.assertFalse(arguments.exists()); self.assertFalse((self.work / 'injected').exists())

    def test_replay_delivery_failure_stops_before_next_child(self):
        first = self.report(['record', 'touch first-effect'])['detail']['sequence']
        last = self.report(['record', 'touch second-effect'])['detail']['sequence']
        shim = self.work / 'delivery.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                        str(f.ROOT / 'tests/recipe_io_faults.c'), '-ldl', '-o', str(shim)], check=True)
        self.env.update(LD_PRELOAD=str(shim), JASH_TEST_RECIPE_FAULT='sink-after-first',
                        JASH_TEST_RECIPE_FIRST_EFFECT=str(self.work / 'first-effect'))
        result = self.run_command(['sigil', '--data', 'replay', '--exec', f'{first}..{last}'])
        self.assertEqual(result.returncode, 125, result.stderr)
        self.assertTrue((self.work / 'first-effect').exists())
        self.assertFalse((self.work / 'second-effect').exists())

    def test_replay_and_restore_observe_actual_records_without_implicit_execution(self):
        label = 'printf literal > replay-output'
        record = self.report(['record', label]); sequence = str(record['detail']['sequence'])
        restore = self.report(['restore', sequence], mode='--data')
        self.assertFalse(restore['detail']['restore_performed'])
        self.assertEqual(restore['detail']['entry']['command'], label)
        self.assertFalse((self.work / 'replay-output').exists())
        preview = self.report(['replay', sequence])
        self.assertFalse(preview['results'][0]['executed'])
        self.assertFalse((self.work / 'replay-output').exists())
        actual = self.report(['replay', '--exec', sequence], mode='--data')
        self.assertTrue(actual[0]['executed']); self.assertTrue(actual[0]['exit_code_matches'])
        self.assertEqual((self.work / 'replay-output').read_bytes(), b'literal')

    def test_replay_failed_comparison_prevents_next_child(self):
        first = self.report(['record', 'exit 7'])['detail']['sequence']
        last = self.report(['record', 'touch should-not-run'])['detail']['sequence']
        result = self.report(['replay', '--exec', f'{first}..{last}'], rc=1)
        self.assertFalse(result['complete']); self.assertEqual(len(result['results']), 1)
        self.assertEqual(result['results'][0]['actual_exit_code'], 7)
        self.assertFalse((self.work / 'should-not-run').exists())

    def test_data_ignores_presentation_budget_and_admission_precedes_effects(self):
        self.env['JASH_LLM_BUDGET'] = '1'
        self.report(['record', 'native budget'], mode='--data-v2')
        self.env.pop('JASH_LLM_BUDGET')
        for mode in ('--json', '--data', '--data-v2'):
            path = self.work / ('must-not-publish-' + mode)
            request = {'request_version': 2, 'argv': ['sigil', mode, 'proof', 'dsse', str(path)]}
            with open('/dev/full', 'wb') as sink:
                result = subprocess.run([str(f.JASH), '--exec-json', json.dumps(request)], env=self.env,
                                        cwd=self.work, stdout=sink, stderr=subprocess.PIPE, timeout=20)
            self.assertEqual(result.returncode, 125, result.stderr)
            self.assertFalse(path.exists())

    def test_agent_profile_retains_existing_module_boundary(self):
        result = self.run_command(['sigil', '--data', 'status'], strict=True,
                                  caps=['effect:read', 'effect:mutate', 'effect:exec'])
        self.assertEqual(result.returncode, 127)
        self.assertEqual(result.stdout, b'')

    def test_published_schema_rejects_missing_and_unknown_fields(self):
        record = self.report(['record', 'schema evidence'])
        for field in record:
            broken = copy.deepcopy(record); del broken[field]
            with self.assertRaises(ValidationError): validate_contract('sigil-action', broken)
        broken = copy.deepcopy(record); broken['detail']['invented'] = True
        with self.assertRaises(ValidationError): validate_contract('sigil-action', broken)


if __name__ == '__main__':
    import unittest
    unittest.main()
