#!/usr/bin/env python3
"""Sigil recovery receipts compared with the actual ledger and witness files."""
import copy
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

from jsonschema import ValidationError
import native_data_fixture as f
from contract_schema_fixture import projection, validate_contract


FAULT_SOURCE = r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

ssize_t write(int descriptor, const void* bytes, size_t length) {
    static ssize_t (*real_write)(int, const void*, size_t);
    if (!real_write) *(void**)&real_write = dlsym(RTLD_NEXT, "write");
    const char* selected = getenv("SIGIL_TRANSITION_SINK_FAULT");
    const char* chain = getenv("SIGIL_TRANSITION_CHAIN");
    if (descriptor == STDOUT_FILENO && selected && chain) {
        struct stat metadata;
        int found = stat(chain, &metadata) == 0;
        int failed = !strcmp(selected, "after-quarantine") && !found && errno == ENOENT;
        const char* original = getenv("SIGIL_TRANSITION_CHAIN_BYTES");
        if (!strcmp(selected, "after-boundary") && found && original &&
            metadata.st_size > strtoll(original, NULL, 10)) failed = 1;
        if (failed) { errno = EPIPE; return -1; }
    }
    return real_write(descriptor, bytes, length);
}

int unlink(const char* path) {
    static int (*real_unlink)(const char*);
    if (!real_unlink) *(void**)&real_unlink = dlsym(RTLD_NEXT, "unlink");
    const char* witness = getenv("SIGIL_TRANSITION_FAIL_WITNESS_CLEANUP");
    if (witness && !strcmp(witness, path)) { errno = EACCES; return -1; }
    return real_unlink(path);
}
'''


class SigilTransitions(f.NativeQueryCase):
    @classmethod
    def setUpClass(cls):
        temporary = tempfile.TemporaryDirectory(prefix='jash-sigil-sink-fault-')
        cls.addClassCleanup(temporary.cleanup)
        source = Path(temporary.name) / 'fault.c'
        source.write_text(FAULT_SOURCE)
        cls.fault_library = Path(temporary.name) / 'fault.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                        str(source), '-o', str(cls.fault_library), '-ldl'], check=True)

    def setUp(self):
        super().setUp()
        self.isolate('initial')

    def isolate(self, name):
        self.work = self.root / name / 'repo'
        self.work.mkdir(parents=True)
        subprocess.run(['git', 'init', '-q', str(self.work)], check=True)
        home = self.root / name / 'home'
        target = home / '.jash/modules'
        target.mkdir(parents=True, mode=0o700)
        module = f.ROOT / 'modules/sigil/sigil.modjash'
        self.assertTrue(module.is_file(), 'Build the module under review')
        shutil.copyfile(module, target / 'sigil.modjash')
        (target / 'sigil.modjash').chmod(0o600)
        self.env['HOME'] = str(home)
        self.env['JASH_SIGIL'] = '0'
        for key in tuple(self.env):
            if key.startswith(('JASH_SIGIL_TEST_', 'SIGIL_TRANSITION_')) or key == 'LD_PRELOAD':
                self.env.pop(key)

    def seed(self, label='recovery evidence'):
        response = self.run_command(['sigil', '--json', 'record', label])
        self.assertEqual(response.returncode, 0, response.stderr)
        detail = json.loads(response.stdout)['detail']
        chain = Path(detail['chain'])
        witness = next(chain.parent.glob('ratchet-*.key'))
        return chain, witness

    def query(self, operation, rc=0, mode='--json'):
        response = self.run_command(['sigil', mode, *operation])
        self.assertEqual(response.returncode, rc, response.stderr + response.stdout[:256])
        if mode == '--json':
            value = json.loads(response.stdout)
        else:
            stream = f.stream(response.stdout, credit_kind='none')
            self.assertEqual(stream.producer, rc)
            self.assertEqual(stream.state, 2 if rc else 0)
            self.assertEqual(len(stream.items), 1)
            value = projection(stream.items[0])
        validate_contract('sigil-query', value)
        return value

    def transition(self, operation, rc=0, mode='--json'):
        response = self.run_command(['sigil', mode, *operation])
        self.assertEqual(response.returncode, rc, response.stderr + response.stdout[:512])
        return self.transition_report(response.stdout, rc=rc, mode=mode)

    def transition_report(self, payload, rc=0, mode='--json'):
        if mode == '--json':
            value = json.loads(payload)
            validate_contract('sigil-transition-report', value)
        else:
            stream = f.stream(payload, credit_kind='none')
            self.assertEqual(stream.producer, rc)
            self.assertEqual(stream.state, 2 if rc else 0)
            rows = [projection(item) for item in stream.items]
            for row in rows:
                validate_contract('sigil-transition', row)
            self.assertTrue(rows)
            self.assertEqual(rows[-1]['row_kind'], 'result')
            self.assertTrue(all(row['row_kind'] == 'receipt' for row in rows[:-1]))
            value = {'receipts': rows[:-1], 'result': rows[-1]}
        self.assertEqual(value['result']['exit_code'], rc)
        self.assertEqual(value['result']['complete'], not rc)
        return value

    def test_status_and_tail_salvage_preserve_prefix_and_manifest_bytes(self):
        for index, mode in enumerate(('--json', '--data', '--data-v2')):
            with self.subTest(mode=mode):
                self.isolate('tail-' + str(index))
                chain, witness = self.seed()
                prefix, witness_before = chain.read_bytes(), witness.read_bytes()
                suffix = b'legacy continuation " \xff\n'
                chain.write_bytes(prefix + suffix)
                status = self.query(['quarantine', 'status'], mode=mode)['result']
                self.assertEqual(status['active_reason'], 'corrupted_newline_fragment')
                self.assertEqual(status['scan']['fragments'], 1)
                self.assertEqual(chain.read_bytes(), prefix + suffix)
                report = self.transition(['quarantine', 'apply'], mode=mode)
                self.assertEqual(len(report['receipts']), 1)
                receipt = report['receipts'][0]
                self.assertEqual(receipt['stage'], 'quarantine-tail')
                self.assertTrue(receipt['applied']); self.assertTrue(receipt['durable'])
                self.assertEqual(receipt['retained_offset'], len(prefix))
                manifest = json.loads(Path(receipt['manifest']).read_bytes())
                self.assertEqual(Path(manifest['quarantined']).read_bytes(), suffix)
                self.assertEqual(manifest['sha256'], hashlib.sha256(suffix).hexdigest())
                self.assertEqual(manifest['original_sha256'], hashlib.sha256(prefix + suffix).hexdigest())
                self.assertEqual(chain.read_bytes(), prefix)
                self.assertEqual(witness.read_bytes(), witness_before)
                self.assertTrue(report['result']['tail_salvaged'])
                self.assertFalse(report['result']['active_reset'])
                self.assertTrue(self.query(['verify'])['result']['valid'])

    def test_hash_recovery_reports_pair_then_fresh_pair_and_preserves_evidence(self):
        chain, witness = self.seed()
        corrupted = chain.read_bytes().replace(b'cmd=recovery', b'cmd=modified', 1)
        old_witness = witness.read_bytes(); chain.write_bytes(corrupted)
        report = self.transition(['quarantine', 'apply'], mode='--data-v2')
        self.assertEqual([row['stage'] for row in report['receipts']],
                         ['quarantine-active-pair', 'create-fresh-pair'])
        manifest = json.loads(Path(report['receipts'][0]['manifest']).read_bytes())
        self.assertEqual(Path(manifest['quarantined']).read_bytes(), corrupted)
        self.assertEqual(Path(manifest['ratchet_quarantined']).read_bytes(), old_witness)
        self.assertEqual(manifest['sha256'], hashlib.sha256(corrupted).hexdigest())
        self.assertEqual(chain.read_bytes(), b'')
        self.assertNotEqual(witness.read_bytes(), old_witness)
        self.assertEqual(report['result']['applied_groups'], 2)
        self.assertTrue(report['result']['active_reset'])
        status = self.query(['quarantine', 'status'])['result']
        self.assertIsNone(status['active_reason'])
        self.assertEqual(status['witness']['entries'], 0)
        self.assertEqual(status['witness']['ratchet_advances'], 0)

    def test_manifest_failure_rolls_back_pair_before_false_applied_receipt(self):
        for fault in ('write', 'close'):
            with self.subTest(fault=fault):
                self.isolate('manifest-' + fault)
                chain, witness = self.seed()
                chain.write_bytes(chain.read_bytes().replace(b'cmd=recovery', b'cmd=modified', 1))
                before, old_witness = chain.read_bytes(), witness.read_bytes()
                self.env['JASH_SIGIL_TEST_MANIFEST_FAIL'] = fault
                report = self.transition(['quarantine', 'apply'], rc=1, mode='--data')
                self.assertEqual(len(report['receipts']), 1)
                self.assertFalse(report['receipts'][0]['applied'])
                self.assertFalse(report['result']['active_reset'])
                self.assertEqual(chain.read_bytes(), before)
                self.assertEqual(witness.read_bytes(), old_witness)
                self.assertEqual(list((chain.parent / 'quarantine').iterdir()), [])

    def test_suffix_durability_failure_keeps_only_copy_and_terminal_failure(self):
        chain, witness = self.seed()
        prefix, old_witness = chain.read_bytes(), witness.read_bytes()
        suffix = b'fragment that must survive\n'; chain.write_bytes(prefix + suffix)
        self.env['JASH_SIGIL_TEST_QUARANTINE_FAIL'] = 'suffix-fsync'
        report = self.transition(['quarantine', 'apply'], rc=1, mode='--data-v2')
        receipt = report['receipts'][0]
        self.assertTrue(receipt['applied']); self.assertFalse(receipt['durable'])
        self.assertEqual(receipt['outcome'], 1)
        manifest = json.loads(Path(receipt['manifest']).read_bytes())
        self.assertEqual(Path(manifest['quarantined']).read_bytes(), suffix)
        self.assertEqual(chain.read_bytes(), prefix)
        self.assertEqual(witness.read_bytes(), old_witness)
        self.assertEqual(report['result']['error'], 'recovery_group_not_durable')

    def test_partial_tail_refresh_allows_same_process_append(self):
        for index, mode in enumerate(('--json', '--data', '--data-v2')):
            with self.subTest(mode=mode):
                self.isolate('partial-tail-process-' + str(index))
                chain, witness = self.seed()
                prefix = chain.read_bytes()
                suffix = b'partial-tail-recovery-bytes\n'
                chain.write_bytes(prefix + suffix)
                receipt_file = self.work / 'recovery.receipt'
                marker = 'same-process-after-partial-tail'
                self.env['JASH_SIGIL_TEST_QUARANTINE_FAIL'] = 'suffix-fsync'
                command = (f'sigil {mode} quarantine apply > {shlex.quote(str(receipt_file))}; '
                           f'echo {marker}')
                response = subprocess.run([str(f.JASH), '-c', command], cwd=self.work,
                    env=dict(self.env, JASH_SIGIL='2'), capture_output=True, timeout=20)
                self.assertEqual(response.returncode, 0, response.stderr)
                self.assertEqual(response.stdout, (marker + '\n').encode())
                report = self.transition_report(receipt_file.read_bytes(), rc=1, mode=mode)
                receipt = report['receipts'][0]
                self.assertEqual(receipt['outcome'], 1)
                self.assertTrue(receipt['applied']); self.assertFalse(receipt['durable'])
                self.assertEqual(report['result']['failed_groups'], 1)
                self.assertEqual(report['result']['error'], 'recovery_group_not_durable')
                manifest = json.loads(Path(receipt['manifest']).read_bytes())
                self.assertEqual(Path(manifest['quarantined']).read_bytes(), suffix)
                after = chain.read_bytes()
                self.assertTrue(after.startswith(prefix))
                # A -c invocation records the complete top-level command, not
                # a separate echo entry. Require exactly that new attestation.
                appended = after[len(prefix):].splitlines()
                self.assertEqual(len(appended), 1)
                self.assertEqual(appended[0].split(b' ', 1)[0], b'1')
                self.assertEqual(appended[0].split(b' cmd=', 1)[1], command.encode())
                self.assertIn(b' rc=0 ', appended[0])
                self.assertEqual(int(witness.read_text().split()[1]), 2)
                verified = self.query(['verify'])['result']
                self.assertTrue(verified['valid'])
                self.assertEqual(verified['entries_verified'], 2)

    def test_partial_pair_repair_preserves_failure_and_quarantined_bytes(self):
        for mode_index, mode in enumerate(('--json', '--data', '--data-v2')):
            for fault in ('full-rollback', 'rollback-chain', 'rollback-ratchet'):
                with self.subTest(mode=mode, fault=fault):
                    self.isolate('partial-pair-' + str(mode_index) + '-' + fault)
                    chain, witness = self.seed()
                    corrupted = chain.read_bytes().replace(b'cmd=recovery', b'cmd=modified', 1)
                    old_witness = witness.read_bytes(); chain.write_bytes(corrupted)
                    self.assertEqual(self.query(['verify'], rc=1)['result']['failure'], 'hash_mismatch')
                    self.env.update(JASH_SIGIL_TEST_QUARANTINE_FAIL=fault,
                                    JASH_SIGIL_TEST_MANIFEST_FAIL='write')
                    report = self.transition(['quarantine', 'apply'], rc=1, mode=mode)
                    self.assertEqual([row['stage'] for row in report['receipts']],
                                     ['quarantine-active-pair', 'create-fresh-pair'])
                    moved, created = report['receipts']
                    self.assertTrue(moved['applied']); self.assertFalse(moved['durable'])
                    self.assertEqual(moved['outcome'], 1)
                    self.assertTrue(created['applied']); self.assertTrue(created['durable'])
                    self.assertEqual(created['outcome'], 0)
                    result = report['result']
                    self.assertTrue(result['active_reset'])
                    self.assertEqual((result['applied_groups'], result['failed_groups']), (2, 1))
                    self.assertEqual(result['error'], 'recovery_group_not_durable')
                    quarantined = chain.parent / 'quarantine'
                    saved_chains = list(quarantined.glob('hash_mismatch-*chain-*.log'))
                    saved_witnesses = list(quarantined.glob('hash_mismatch-*ratchet-*.key'))
                    self.assertEqual(len(saved_chains), 1); self.assertEqual(len(saved_witnesses), 1)
                    self.assertEqual(saved_chains[0].read_bytes(), corrupted)
                    self.assertEqual(saved_witnesses[0].read_bytes(), old_witness)
                    self.assertEqual(chain.read_bytes(), b'')
                    self.assertNotEqual(witness.read_bytes(), old_witness)
                    self.assertEqual(int(witness.read_text().split()[1]), 0)
                    self.assertTrue(self.query(['verify'])['result']['valid'])

    def test_partial_active_repair_does_not_start_independent_migration(self):
        chain, witness = self.seed()
        original = chain.read_bytes()
        migration = Path(str(chain) + '.v1'); migration.write_bytes(original)
        chain.write_bytes(original + b'0 invalid invalid tag=invalid rc=0 cmd=malformed\n')
        self.env.update(JASH_SIGIL_TEST_QUARANTINE_FAIL='full-rollback',
                        JASH_SIGIL_TEST_MANIFEST_FAIL='write')
        report = self.transition(['quarantine', 'apply'], rc=1, mode='--data-v2')
        self.assertEqual([row['stage'] for row in report['receipts']],
                         ['quarantine-active-pair', 'create-fresh-pair'])
        self.assertTrue(report['result']['active_reset'])
        self.assertEqual(report['result']['failed_groups'], 1)
        self.assertEqual(migration.read_bytes(), original)
        self.assertEqual(chain.read_bytes(), b'')
        self.assertEqual(int(witness.read_text().split()[1]), 0)

    def test_fresh_pair_failure_retains_quarantined_pair_and_stops(self):
        chain, witness = self.seed()
        corrupted = chain.read_bytes().replace(b'cmd=recovery', b'cmd=modified', 1)
        chain.write_bytes(corrupted); old_witness = witness.read_bytes()
        self.env['JASH_SIGIL_TEST_QUARANTINE_FAIL'] = 'fresh-chain'
        report = self.transition(['quarantine', 'apply'], rc=1)
        self.assertEqual(len(report['receipts']), 2)
        self.assertTrue(report['receipts'][0]['applied'])
        self.assertFalse(report['receipts'][1]['applied'])
        manifest = json.loads(Path(report['receipts'][0]['manifest']).read_bytes())
        self.assertEqual(Path(manifest['quarantined']).read_bytes(), corrupted)
        self.assertEqual(Path(manifest['ratchet_quarantined']).read_bytes(), old_witness)
        self.assertFalse(chain.exists()); self.assertFalse(witness.exists())
        self.assertEqual(report['result']['error'], 'fresh_pair_failed')

    def test_migration_artifact_quarantine_does_not_reset_active_pair(self):
        chain, witness = self.seed()
        original, old_witness = chain.read_bytes(), witness.read_bytes()
        artifact = Path(str(chain) + '.v1'); artifact.write_bytes(original)
        status = self.query(['quarantine', 'status'])['result']
        self.assertEqual(status['invalid_migration']['path'], str(artifact))
        report = self.transition(['quarantine', 'apply'], mode='--data')
        self.assertEqual([row['stage'] for row in report['receipts']], ['quarantine-migration'])
        self.assertFalse(artifact.exists()); self.assertFalse(report['result']['active_reset'])
        self.assertEqual(chain.read_bytes(), original)
        self.assertEqual(witness.read_bytes(), old_witness)

    def test_fresh_pair_cleanup_failure_cannot_claim_no_effect(self):
        chain, witness = self.seed()
        chain.write_bytes(chain.read_bytes().replace(b'cmd=recovery', b'cmd=modified', 1))
        self.env.update(JASH_SIGIL_TEST_QUARANTINE_FAIL='fresh-chain',
                        LD_PRELOAD=str(self.fault_library),
                        SIGIL_TRANSITION_FAIL_WITNESS_CLEANUP=str(witness))
        report = self.transition(['quarantine', 'apply'], rc=1, mode='--data')
        receipt = report['receipts'][-1]
        self.assertEqual(receipt['stage'], 'create-fresh-pair')
        self.assertIsNone(receipt['applied']); self.assertFalse(receipt['durable'])
        self.assertEqual(receipt['outcome'], -2)
        self.assertFalse(chain.exists()); self.assertTrue(witness.exists())
        self.assertEqual(int(witness.read_text().split()[1]), 0)
        self.assertTrue(Path(report['receipts'][0]['manifest']).exists())

    def test_rekey_records_real_boundary_retired_key_and_current_segment(self):
        for index, mode in enumerate(('--json', '--data', '--data-v2')):
            with self.subTest(mode=mode):
                self.isolate('rekey-' + str(index))
                chain, witness = self.seed()
                original = chain.read_bytes()
                report = self.transition(['ratchet', 'rekey'], mode=mode)
                result = report['result']
                self.assertEqual([row['stage'] for row in report['receipts']],
                    ['seal-boundary-and-advance-witness', 'preserve-retired-witness',
                     'publish-replacement-witness', 'append-retirement-record'])
                self.assertTrue(all(row['applied'] and row['durable'] for row in report['receipts']))
                self.assertTrue(chain.read_bytes().startswith(original))
                self.assertIn(b'#sigil-boundary v1 mode=rotate', chain.read_bytes())
                self.assertEqual(result['boundary_sequence'], 1)
                self.assertEqual(result['previous_entries'], 1)
                self.assertEqual(result['previous_advances'], 1)
                self.assertFalse(result['past_entries_promoted_to_verified'])
                self.assertNotEqual(Path(result['retired_witness']).read_bytes(), witness.read_bytes())
                sidecar = json.loads(Path(result['sidecar']).read_text().splitlines()[-1])
                self.assertEqual(sidecar['boundary_seq'], result['boundary_sequence'])
                self.assertEqual(sidecar['retired_witness_path'], result['retired_witness'])
                self.assertEqual(sidecar['segment_entries'], 1)
                self.assertEqual(sidecar['segment_does_not_claim'], 'position-witness')
                verified = self.query(['verify'])['result']
                self.assertTrue(verified['valid']); self.assertEqual(verified['witness_scope'], 'current_segment')
                self.assertEqual(verified['entries_verified'], 2)
                self.assertEqual(verified['witness_entries'], 0)
                self.assertEqual(verified['ratchet_advances'], 0)

    def test_adopt_shared_witness_explicitly_preserves_weaker_lineage(self):
        chain, witness = self.seed()
        global_witness = witness.parent / 'ratchet.key'
        witness.rename(global_witness)
        original = global_witness.read_bytes()
        report = self.transition(['ratchet', 'adopt'], mode='--data')
        result = report['result']
        self.assertTrue(result['from_shared']); self.assertEqual(result['mode'], 'adopt')
        self.assertEqual(result['previous_advances_scope'], 'shared-aggregate-across-repos')
        self.assertIn('weaker', result['exclusivity'])
        self.assertFalse(result['past_entries_promoted_to_verified'])
        self.assertTrue(witness.exists())
        self.assertNotEqual(global_witness.read_bytes(), original, 'Boundary advances the outgoing shared witness')
        self.assertEqual(Path(result['retired_witness']).read_bytes(), global_witness.read_bytes())
        self.assertIn(b'lineage=derived-from-shared', chain.read_bytes())
        self.assertTrue(self.query(['verify'])['result']['valid'])
        before = chain.read_bytes(), witness.read_bytes()
        rejected = self.transition(['ratchet', 'adopt'], rc=1)
        self.assertEqual(rejected['result']['error'], 'already_repo_scoped')
        self.assertEqual(rejected['receipts'], [])
        self.assertEqual((chain.read_bytes(), witness.read_bytes()), before)

    def test_ratchet_failure_receipts_stop_at_the_failed_publication(self):
        expected_stages = {'outgoing': 1, 'preserve': 2, 'replacement': 3,
                           'sidecar-open': 4, 'sidecar-fsync': 4}
        for fault, count in expected_stages.items():
            with self.subTest(fault=fault):
                self.isolate('ratchet-' + fault)
                chain, witness = self.seed()
                before, old_witness = chain.read_bytes(), witness.read_bytes()
                self.env['JASH_SIGIL_TEST_RATCHET_FAIL'] = fault
                report = self.transition(['ratchet', 'rekey'], rc=1, mode='--data-v2')
                result, receipts = report['result'], report['receipts']
                self.assertEqual(len(receipts), count)
                self.assertNotEqual(receipts[-1]['outcome'], 0)
                self.assertFalse(receipts[-1]['durable'])
                self.assertTrue(chain.read_bytes().startswith(before))
                self.assertTrue(result['boundary_sealed'])
                self.assertEqual(result['replacement_published'], fault.startswith('sidecar-'))
                self.assertFalse(result['sidecar_written'])
                if fault == 'outgoing':
                    self.assertEqual(witness.read_bytes(), old_witness)
                    self.assertFalse(Path(result['retired_witness']).exists())
                if fault in ('preserve', 'replacement'):
                    self.assertFalse(receipts[-1]['applied'])
                if fault != 'sidecar-fsync':
                    self.assertFalse(Path(result['sidecar']).exists())
                else:
                    self.assertIsNone(receipts[-1]['applied'])
                    self.assertEqual(json.loads(Path(result['sidecar']).read_text())['boundary_seq'], 1)

    def test_admission_failure_precedes_any_recovery_effect(self):
        for index, mode in enumerate(('--json', '--data-v2')):
            with self.subTest(mode=mode):
                self.isolate('admission-' + str(index))
                chain, witness = self.seed()
                chain.write_bytes(chain.read_bytes().replace(b'cmd=recovery', b'cmd=modified', 1))
                before, old_witness = chain.read_bytes(), witness.read_bytes()
                request = {'request_version': 2, 'argv': ['sigil', mode, 'quarantine', 'apply'], 'cwd': str(self.work)}
                with open('/dev/full', 'wb') as sink:
                    response = subprocess.run([str(f.JASH), '--exec-json', json.dumps(request)],
                        cwd=self.work, env=self.env, stdout=sink, stderr=subprocess.PIPE, timeout=20)
                self.assertNotEqual(response.returncode, 0)
                self.assertEqual(chain.read_bytes(), before)
                self.assertEqual(witness.read_bytes(), old_witness)
                self.assertFalse((chain.parent / 'quarantine').exists())

    def test_receipt_failure_stops_before_next_recovery_group(self):
        for index, mode in enumerate(('--json', '--data-v2')):
            with self.subTest(mode=mode):
                self.isolate('sink-quarantine-' + str(index))
                chain, witness = self.seed()
                original = chain.read_bytes().replace(b'cmd=recovery', b'cmd=modified', 1)
                chain.write_bytes(original); old_witness = witness.read_bytes()
                self.env.update(LD_PRELOAD=str(self.fault_library), SIGIL_TRANSITION_SINK_FAULT='after-quarantine',
                                SIGIL_TRANSITION_CHAIN=str(chain))
                response = self.run_command(['sigil', mode, 'quarantine', 'apply'])
                self.assertEqual(response.returncode, 125, response.stderr)
                self.assertFalse(chain.exists(), 'The failed first receipt must prevent fresh pair creation')
                self.assertFalse(witness.exists())
                manifests = list((chain.parent / 'quarantine').glob('*.manifest.json'))
                self.assertEqual(len(manifests), 1)
                manifest = json.loads(manifests[0].read_bytes())
                self.assertEqual(Path(manifest['quarantined']).read_bytes(), original)
                self.assertEqual(Path(manifest['ratchet_quarantined']).read_bytes(), old_witness)
                if mode == '--data-v2':
                    self.assertFalse(any(kind == b'T' for kind, _ in f.frames(response.stdout, b'JTV2')))

    def test_receipt_failure_after_boundary_prevents_replacement_and_retirement(self):
        chain, witness = self.seed()
        before = chain.read_bytes()
        self.env.update(LD_PRELOAD=str(self.fault_library), SIGIL_TRANSITION_SINK_FAULT='after-boundary',
                        SIGIL_TRANSITION_CHAIN=str(chain), SIGIL_TRANSITION_CHAIN_BYTES=str(len(before)))
        response = self.run_command(['sigil', '--data-v2', 'ratchet', 'rekey'])
        self.assertEqual(response.returncode, 125, response.stderr)
        self.assertTrue(chain.read_bytes().startswith(before))
        self.assertIn(b'#sigil-boundary v1', chain.read_bytes())
        self.assertEqual(list(chain.parent.glob('retired-ratchet-*.key')), [])
        self.assertEqual(list(chain.parent.glob('retired-*.jsonl')), [])
        self.assertTrue(witness.exists())
        self.assertFalse(any(kind == b'T' for kind, _ in f.frames(response.stdout, b'JTV2')))
        self.env.pop('LD_PRELOAD')
        self.assertEqual(int(witness.read_text().split()[1]), 2)
        verified = self.query(['verify'], rc=1)['result']
        self.assertFalse(verified['valid'])
        self.assertEqual(verified['failure'], 'ratchet_replacement_incomplete')

    def test_transition_schema_rejects_wrong_types_missing_outcome_and_claims(self):
        self.seed()
        report = self.transition(['ratchet', 'rekey'])
        for mutate in (
                lambda value: value['receipts'][0].__setitem__('applied', 'yes'),
                lambda value: value['receipts'][0].pop('outcome'),
                lambda value: value['result'].__setitem__('past_entries_promoted_to_verified', True)):
            invalid = copy.deepcopy(report); mutate(invalid)
            with self.assertRaises(ValidationError):
                validate_contract('sigil-transition-report', invalid)


if __name__ == '__main__':
    import unittest
    unittest.main()
