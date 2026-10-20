#!/usr/bin/env python3
"""Sigil native queries compared with actual ledger bytes and independent JEF."""
import base64
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


class SigilQueries(f.NativeQueryCase):
    def setUp(self):
        super().setUp()
        module = f.ROOT / 'modules/sigil/sigil.modjash'
        self.assertTrue(module.is_file(), 'Build the Sigil module under review')
        target = Path(self.env['HOME']) / '.jash/modules'
        target.mkdir(parents=True, mode=0o700)
        shutil.copyfile(module, target / 'sigil.modjash')
        (target / 'sigil.modjash').chmod(0o600)

    def record(self, label):
        result = self.run_command(['sigil', '--json', 'record', label])
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)['detail']

    def query(self, args, *, mode='--json', rc=0, incomplete=False):
        result = self.run_command(['sigil', mode, *args])
        self.assertEqual(result.returncode, rc, result.stderr + result.stdout[:512])
        if mode == '--json':
            value = json.loads(result.stdout)
        else:
            decoded = f.stream(result.stdout, credit_kind='none')
            self.assertEqual(decoded.producer, rc)
            self.assertEqual(decoded.state, 2 if rc else 3 if incomplete else 0)
            self.assertEqual(decoded.truncated, incomplete)
            self.assertEqual(len(decoded.items), 1)
            value = projection(decoded.items[0])
        validate_contract('sigil-query', value)
        return value

    def test_full_command_bytes_source_counts_and_typed_pages(self):
        labels = ['space " quote \\ newline\nend', 'a' * 3000, b'raw-\xff-byte']
        details = [self.record(label) for label in labels]
        chain = Path(details[0]['chain'])
        original = chain.read_bytes()
        for mode in ('--json', '--data', '--data-v2'):
            report = self.query(['list'], mode=mode)
            rows = report['result']['records']
            self.assertEqual([r['sequence'] for r in rows], [d['sequence'] for d in details])
            self.assertEqual(rows[0]['command'], labels[0])
            self.assertEqual(rows[1]['command'], labels[1])
            self.assertEqual(base64.b64decode(rows[2]['command']['data']), labels[2])
            self.assertTrue(all(row['attested_record'] and not row['verified'] for row in rows))
            self.assertEqual(report['result']['source_bytes'], len(original))
            page = self.query(['list', '--limit', '1', '--offset', '1'], mode=mode, incomplete=True)
            self.assertFalse(page['complete']); self.assertEqual(page['result']['next_offset'], 2)
            self.assertEqual(page['result']['records'][0]['command'], labels[1])
        self.assertEqual(chain.read_bytes(), original, 'Queries must not append or rewrite ledger bytes')

    def test_history_blame_diff_and_first_failure_use_recorded_semantics(self):
        first = self.record('--literal quoted " field')
        second = self.record('different command')
        tail = self.query(['history', '--tail', '1'], mode='--data')['result']
        self.assertEqual([r['command'] for r in tail['records']], ['different command'])
        blame = self.query(['blame', '--', '--literal'], mode='--data-v2')['result']
        self.assertEqual(len(blame['records']), 1)
        self.assertEqual(blame['records'][0]['command'], '--literal quoted " field')
        diff = self.query(['diff', str(first['sequence']), str(second['sequence'])])['result']
        self.assertFalse(diff['command_equal']); self.assertTrue(diff['exit_code_equal'])
        self.assertIn('no filesystem', diff['comparison_scope'])
        self.env['JASH_SIGIL'] = '2'
        actual = subprocess.run([str(f.JASH), '-c', 'false'], cwd=self.work, env=self.env, capture_output=True)
        self.assertEqual(actual.returncode, 1, actual.stderr)
        self.env['JASH_SIGIL'] = '0'
        failure = self.query(['bisect'], mode='--data')['result']
        self.assertEqual(failure['first_failure']['stored_exit_code'], 1)
        self.assertEqual(failure['first_failure']['command'], 'false')
        self.assertFalse(failure['executed'])

    def test_verify_shared_verdict_detects_tampering_and_witness_truncation(self):
        detail = self.record('verified first')
        self.record('verified second')
        chain = Path(detail['chain']); original = chain.read_bytes()
        for mode in ('--json', '--data', '--data-v2'):
            report = self.query(['chain', 'verify'], mode=mode)['result']
            self.assertTrue(report['valid']); self.assertEqual(report['entries_verified'], 2)
            self.assertEqual(report['checks']['hash_chain'], 'pass')
            self.assertEqual(report['checks']['per_entry_tags'], 'not-verified')
            selected = self.query(['verify', '0..0'], mode=mode)['result']
            self.assertEqual(selected['range_verified'], 1)
            self.assertEqual(selected['entries_verified'], 2, 'Range still declares whole-chain verification')
        chain.write_bytes(original.replace(b'cmd=verified first', b'cmd=modified first', 1))
        broken = self.query(['verify'], mode='--data', rc=1)['result']
        self.assertEqual(broken['failure'], 'hash_mismatch'); self.assertFalse(broken['valid'])
        self.assertEqual(broken['checks']['hash_chain'], 'fail')
        chain.write_bytes(original.splitlines(keepends=True)[0])
        truncated = self.query(['verify'], rc=1)['result']
        self.assertEqual(truncated['failure'], 'log_truncated')
        self.assertEqual(truncated['missing'], 1)
        self.assertEqual(truncated['checks']['hash_chain'], 'pass')
        self.assertEqual(truncated['checks']['ratchet_position'], 'fail')
        chain.write_bytes(original)

    def test_proof_show_and_verification_bind_one_source_snapshot(self):
        self.record('proof query evidence')
        for form in ('emit', 'dsse'):
            path = self.work / ('proof with spaces.' + form)
            emitted = self.run_command(['sigil', '--json', 'proof', form, str(path)])
            self.assertEqual(emitted.returncode, 0, emitted.stderr)
            key = json.loads(emitted.stdout)['detail']['signer_key']
            shown = self.query(['proof', 'show', str(path)], mode='--data')['result']
            self.assertEqual(base64.b64decode(shown['content']['data']), path.read_bytes())
            self.assertEqual(shown['sha256'], hashlib.sha256(path.read_bytes()).hexdigest())
            self.assertFalse(shown['verified'])
            for mode in ('--json', '--data-v2'):
                proof = self.query(['proof', 'verify', '--pubkey', key, str(path)], mode=mode)['result']
                self.assertTrue(proof['signature_valid']); self.assertTrue(proof['identity_authenticated'])
                self.assertTrue(proof['chain_binding_available']); self.assertEqual(proof['live_chain_status'], 'matches')
                self.assertEqual(proof['chain_entries'], 1)
            local = self.query(['proof', 'verify', str(path)])['result']
            self.assertTrue(local['identity_authenticated']); self.assertEqual(local['trust_source'], 'local-identity')
            wrong = self.query(['proof', 'verify', '--pubkey', '0' * 64, str(path)], mode='--data', rc=1)
            self.assertEqual(wrong['error'], 'unexpected_identity')
            self.assertTrue(wrong['result']['signature_valid']); self.assertFalse(wrong['result']['identity_authenticated'])
        self.record('chain grew')
        old = self.query(['proof', 'verify', str(path)], rc=3)
        self.assertEqual(old['error'], 'proof_obsolete')

    def test_identity_and_loaded_capabilities_are_native_engine_values(self):
        identity = self.query(['proof', 'identity'], mode='--data')['result']
        self.assertTrue(identity['available']); self.assertEqual(identity['algorithm'], 'ed25519')
        self.assertEqual(len(identity['key_fingerprint']), 64)
        caps = self.query(['caps'])['result']
        module = next(row for row in caps['modules'] if row['name'] == 'sigil')
        self.assertEqual(module['undeclared_mask'], module['used_mask'] & ~module['declared_mask'])
        self.assertIn('crypto', module['used'])
        self.assertEqual(caps['count'], len(caps['modules']))

    def test_missing_corrupt_nonregular_and_unknown_flags_remain_explicit(self):
        empty = self.query(['list'], mode='--data')['result']
        self.assertEqual(empty['records'], [])
        detail = self.record('stable source'); chain = Path(detail['chain'])
        original = chain.read_bytes(); chain.write_bytes(original + b'fragment without LF')
        malformed = self.query(['list'], mode='--data', rc=1)
        self.assertEqual(malformed['error'], 'malformed_chain')
        chain.write_bytes(original)
        for arguments in (['list', '--unknown'], ['history', '--tail', '257'], ['history', '--offset', '1'],
                          ['blame', '--unknown'], ['diff', '0..1junk'], ['verify', '2..1'],
                          ['proof', 'verify', '--pubkey', 'x' * 64], ['proof', 'show', '--unknown']):
            result = self.query(arguments, mode='--data-v2', rc=2)
            self.assertFalse(result['ok'])
        fifo = self.work / 'proof fifo'; os.mkfifo(fifo)
        result = self.query(['proof', 'show', str(fifo)], rc=1)
        self.assertEqual(result['error'], 'proof_read_failed')
        self.assertEqual(chain.read_bytes(), original)

    def test_strict_profile_keeps_the_module_boundary(self):
        result = self.run_command(['sigil', '--data', 'list'], strict=True,
                                  caps=['effect:read', 'effect:mutate', 'effect:exec'])
        self.assertEqual(result.returncode, 127, result.stderr)

    def test_raw_shell_query_does_not_attest_itself_and_next_command_still_records(self):
        detail = self.record('prior evidence')
        chain = Path(detail['chain']); original = chain.read_bytes()
        self.env['JASH_SIGIL'] = '2'
        response = subprocess.run([str(f.JASH), '-c', 'sigil --json list'],
                                  cwd=self.work, env=self.env, capture_output=True, timeout=20)
        self.assertEqual(response.returncode, 0, response.stderr)
        validate_contract('sigil-query', json.loads(response.stdout))
        self.assertEqual(chain.read_bytes(), original)
        response = subprocess.run([str(f.JASH), '-c', 'sigil --json list; echo after-query'],
                                  cwd=self.work, env=self.env, capture_output=True, timeout=20)
        self.assertEqual(response.returncode, 0, response.stderr)
        self.assertIn(b'echo after-query', chain.read_bytes())
        self.assertNotEqual(chain.read_bytes(), original)

    def test_query_schema_rejects_wrong_record_types_and_unearned_verification(self):
        self.record('schema evidence')
        report = self.query(['list'])
        for mutate in (
                lambda value: value['result']['records'][0].__setitem__('sequence', '0'),
                lambda value: value['result']['records'][0].__setitem__('verified', True),
                lambda value: value['result'].pop('source_bytes')):
            invalid = copy.deepcopy(report); mutate(invalid)
            with self.assertRaises(ValidationError):
                validate_contract('sigil-query', invalid)


if __name__ == '__main__':
    import unittest
    unittest.main()
