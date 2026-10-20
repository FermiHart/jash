#!/usr/bin/env python3
"""Actual memory inference, checked persistence, and scoped verification."""
import copy
import base64
import fcntl
import json
import os
from pathlib import Path
import subprocess
import select
import socket
import tempfile
import unittest
import time

from jsonschema import Draft202012Validator
import test_management_builtins as management
from test_discovery_builtins import native_stream, project


class MemNative(unittest.TestCase):
    run_cli = management.ManagementBuiltins.run_cli
    execute = management.ManagementBuiltins.execute

    @classmethod
    def setUpClass(cls):
        temporary = tempfile.TemporaryDirectory(prefix='jash-mem-faults-')
        cls.addClassCleanup(temporary.cleanup)
        cls.fault_library = str(Path(temporary.name) / 'faults.so')
        subprocess.run(['cc', '-std=gnu11', '-shared', '-fPIC', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                        str(management.ROOT / 'tests/memory_io_faults.c'), '-ldl', '-o', cls.fault_library],
                       check=True, capture_output=True, timeout=25)

    def setUp(self):
        management.ManagementBuiltins.setUp(self)
        (self.directory / '.jash').mkdir(mode=0o700)
        (self.directory / 'tmp').mkdir(mode=0o700)
        self.env['TMPDIR'] = str(self.directory / 'tmp')
        self.store = self.directory / '.jash/mem.tsv'
        self.chain_path = self.directory / '.jash/chain.log'
        schema = json.loads((management.ROOT / 'schemas/builtin-mem-v1.schema.json').read_text())
        Draft202012Validator.check_schema(schema)
        self.validator = Draft202012Validator(schema)

    def chain(self, entries):
        data = bytearray()
        for sequence, timestamp, status, session, command in entries:
            prefix = f'{sequence} {timestamp}.000000001 ' + '0' * 64 + f' tag=fixture rc={status} cwd={self.directory} repo=- dur=1 sess={session} cmd='
            data.extend(prefix.encode() + command + b'\n')
        self.chain_path.write_bytes(data)

    def seed(self):
        (self.directory / 'source.c').write_text('synthetic fixture\n')
        self.chain([(1, 100, 1, 'alpha', b'cat source.c'),
                    (2, 101, 0, 'alpha', b'cat source.c'),
                    (3, 110, 0, 'beta', b'ls source.c'),
                    (4, 120, 0, 'gamma', b'touch source.c')])

    def report(self, *arguments, status=0):
        value = json.loads(self.execute(['mem', '--json', *arguments], status=status).stdout)
        self.validator.validate(value)
        return value

    def native(self, *arguments, status=0, selector='--data-v2'):
        rows, terminal = native_stream(self.execute(['mem', selector, *arguments], status=status).stdout)
        self.assertEqual(len(rows), 1)
        self.assertEqual(terminal['status'], status)
        self.assertEqual(terminal['state'], 0 if status == 0 else 2)
        value = project(rows[0]); self.validator.validate(value)
        return value, terminal, rows[0]

    def test_distill_uses_real_engine_and_checked_publication(self):
        self.seed()
        result, terminal, raw = self.native('distill')
        self.assertTrue(result['applied']); self.assertTrue(result['durable'])
        self.assertTrue(result['changed']); self.assertTrue(result['attestation_requested'])
        self.assertEqual({k: result['distillation'][k] for k in ('cards', 'observations', 'bugfix', 'discovery', 'change')},
                         dict(cards=3, observations=4, bugfix=1, discovery=2, change=1))
        self.assertEqual(result['distillation']['chain_records'], 4)
        self.assertFalse(result['chain_source']['cryptographically_verified'])
        self.assertFalse(result['store']['present'])
        self.assertEqual(raw[1]['distillation'][1]['cards'][0], 3)
        stored = self.store.read_text().splitlines()
        self.assertEqual(sum(line.startswith('C\t') for line in stored), 3)
        self.assertEqual(sum(line.startswith('O\t') for line in stored), 4)
        before = self.store.read_bytes(); stamp = self.store.stat().st_mtime_ns
        again = self.report('distill')
        self.assertFalse(again['applied']); self.assertFalse(again['changed'])
        self.assertFalse(again['attestation_requested']); self.assertIsNone(again['durable'])
        self.assertEqual(self.store.read_bytes(), before); self.assertEqual(self.store.stat().st_mtime_ns, stamp)

    def test_zero_based_ledger_distills_reloads_and_verifies(self):
        self.chain([(0, 100, 1, 'zero', b'cat source.c'),
                    (1, 101, 0, 'zero', b'cat source.c')])
        result = self.report('distill')
        self.assertTrue(result['applied'])
        self.assertEqual(result['distillation']['chain_records'], 2)
        timeline, terminal, raw = self.native('timeline')
        self.assertEqual(terminal['state'], 0)
        card = timeline['results'][0]['card']
        self.assertEqual((card['first_sequence'], card['last_sequence']), (0, 1))
        self.assertTrue(any(row['sequence_a'] == 0 for row in card['observations']))
        verified = self.report('verify', card['id'])['verification']
        self.assertTrue(verified['consistent'])
        before = self.store.read_bytes()
        for rows in ([(0, 100, 1, 'zero', b'cat source.c'),
                      (0, 101, 0, 'zero', b'cat source.c')],
                     [(-1, 100, 0, 'zero', b'cat source.c')]):
            self.chain(rows)
            failed = self.report('distill', status=1)
            self.assertFalse(failed['applied'])
            self.assertIsNotNone(failed['error'])
            self.assertEqual(self.store.read_bytes(), before)

    def test_self_records_are_excluded_and_session_gap_is_shared(self):
        self.chain([(1, 100, 0, 'same', b'ls'), (2, 101, 0, 'same', b'mem --json distill'),
                    (3, 102, 0, 'other', b'mem-distill: synthetic attest'),
                    (4, 103, 0, 'same', b'pwd'), (5, 2000, 0, 'same', b'ls')])
        result = self.report('distill')
        self.assertEqual(result['distillation']['chain_records'], 3)
        self.assertEqual(result['distillation']['skipped_self_records'], 2)
        self.assertEqual(result['distillation']['cards'], 2)
        cards = self.report('timeline')['results']
        self.assertEqual([row['card']['commands'] for row in cards], [2, 1])
        for row in cards:
            verified = self.report('verify', row['card']['id'])
            self.assertTrue(verified['verification']['consistent'])

    def test_queries_page_and_preserve_native_integer_and_bytes(self):
        sequence = 9007199254740993
        self.chain([(sequence, 100, 0, 'exact', b'cat file\xff')])
        self.report('distill')
        result, _, raw = self.native('timeline', '--limit', '1')
        card = result['results'][0]['card']
        self.assertEqual(card['first_sequence'], sequence)
        self.assertEqual(raw[1]['results'][1][0][1]['card'][1]['first_sequence'][0], 3)
        self.assertFalse(card['files_inventory_complete'])
        detail = card['observations'][0]['detail_preview']
        self.assertEqual(detail, 'read-only session: 1 reads')
        self.assertEqual(self.report('show', card['id'])['results'][0]['card'], card)
        self.seed(); self.report('distill')
        page = self.report('timeline', '--limit', '1')
        self.assertEqual(page['matched'], 3); self.assertEqual(page['next_offset'], 1)
        tail = self.report('timeline', '--offset', '1', '--limit', '2')
        self.assertEqual(len(tail['results']), 2); self.assertFalse(tail['truncated'])
        self.assertEqual(self.report('timeline', '--offset', '100')['results'], [])
        missing, _, _ = self.native('show', 'does-not-exist', status=1)
        self.assertEqual(missing['error'], 'card_not_found')

    def test_search_literal_query_and_outcome_filters(self):
        self.seed(); self.report('distill')
        result = self.report('search', 'cat source.c')
        observations = [r for r in result['results'] if r['kind'] == 'observation']
        self.assertTrue(any(r['observation']['type'] == 'bugfix' for r in observations))
        failed = self.report('search', 'cat', '--failed')
        self.assertEqual({r['observation']['type'] for r in failed['results']}, {'bugfix'})
        changes = self.report('search', 'session', '--type', 'change')
        self.assertEqual(len(changes['results']), 1)
        self.assertEqual(changes['results'][0]['observation']['type'], 'change')
        self.assertEqual(self.report('search', '--', '--json')['results'], [])

    def test_verify_checks_counts_timestamps_sessions_without_crypto_claim(self):
        self.seed(); self.report('distill')
        card = self.report('timeline')['results'][0]['card']
        valid = self.report('verify', card['id'])['verification']
        self.assertTrue(valid['consistent'])
        self.assertFalse(valid['cryptographically_verified'])
        self.assertFalse(valid['heuristic_truth_verified'])
        original = self.store.read_text()
        lines = original.splitlines(); fields = lines[0].split('\t'); fields[8] = '0'
        lines[0] = '\t'.join(fields); self.store.write_text('\n'.join(lines) + '\n')
        drift = self.report('verify', card['id'], status=1)
        self.assertFalse(drift['verification']['consistent'])
        self.assertEqual(drift['error'], 'record_drift')
        self.store.write_text(original)
        raw = self.chain_path.read_bytes().replace(b'2 101.000000001', b'2 102.000000001')
        self.chain_path.write_bytes(raw)
        self.assertFalse(self.native('verify', card['id'], status=1)[0]['verification']['consistent'])

    def test_brief_uses_real_gap_and_mesh_collectors(self):
        self.seed(); self.report('distill')
        (self.directory / '.jash/gaps.tsv').write_text('aaaaaaaaaaaaaaaa\topen\t2\t1\t2\tfixture\t\t\nbbbbbbbbbbbbbbbb\tfixed\t1\t1\t2\tfixed fixture\t\t\n')
        (self.directory / '.jash/graph.tsv').write_bytes(b'only metadata observed, not parsed\n')
        brief = self.report('brief')['brief']
        self.assertEqual(brief['last_card']['session'], 'gamma')
        self.assertEqual(brief['gaps']['open'], 1); self.assertEqual(brief['gaps']['fixed'], 1)
        self.assertFalse(brief['mesh_tasks']['present']); self.assertEqual(brief['mesh_tasks']['open'], 0)
        self.assertTrue(brief['graph']['present']); self.assertFalse(brief['graph']['content_verified'])
        (self.directory / '.jash/gaps.tsv').write_bytes(b'malformed\n')
        broken = self.report('brief', status=1)
        self.assertFalse(broken['brief']['gaps']['source']['read_complete'])
        self.assertIsNone(broken['brief']['gaps']['open'])
        self.assertIsNotNone(broken['error'])

    def test_missing_queries_are_explicit_and_do_not_create_store(self):
        for arguments in ((), ('brief',), ('timeline',), ('search', 'nothing')):
            value = self.report(*arguments)
            self.assertFalse(value['store']['present'])
            self.assertEqual(value['results'], [])
        failed = self.report('distill', status=1)
        self.assertEqual(failed['error'], 'chain_not_recorded')
        self.assertFalse(self.store.exists())
        self.assertFalse((self.directory / '.jash/.memory-lock').exists())

    def test_malformed_unsafe_and_inconsistent_store_cannot_be_empty_success(self):
        self.seed(); self.report('distill'); original = self.store.read_bytes()
        first = original.splitlines(keepends=True)[0]
        for malformed in (b'bad\n', first + first, first.rstrip(b'\n'), b'O\tmissing\tbugfix\tx\t1\t2\n'):
            self.store.write_bytes(malformed)
            value = self.report('timeline', status=1)
            self.assertFalse(value['store']['read_complete'])
            self.assertIsNone(value['store']['cards']); self.assertIsNone(value['matched'])
            self.assertEqual(value['results'], [])
        self.store.unlink(); outside = self.directory / 'outside'; outside.write_bytes(original)
        self.store.symlink_to(outside)
        failed = self.report('distill', status=1)
        self.assertFalse(failed['applied']); self.assertEqual(outside.read_bytes(), original)

    def test_engine_limits_fail_before_publishing_partial_inferences(self):
        self.seed(); self.report('distill'); before = self.store.read_bytes()
        self.chain([(1, 100, 0, 'overlong', b'x' * 1200)])
        result = self.report('distill', status=1)
        self.assertEqual(result['error'], 'engine_input_limit'); self.assertEqual(self.store.read_bytes(), before)
        self.chain([(n + 1, 100 + n, 0, str(n), b'ls') for n in range(1025)])
        result = self.report('distill', status=1)
        self.assertEqual(result['error'], 'inference_collection_failed'); self.assertEqual(self.store.read_bytes(), before)
        self.chain([(2, 100, 0, 's', b'ls'), (1, 101, 0, 's', b'ls')])
        self.assertEqual(self.report('distill', status=1)['error'], 'invalid_chain_sequence_order')

    def test_bad_arguments_and_failed_admission_have_no_publication(self):
        self.seed()
        for arguments in (('show',), ('search', ''), ('timeline', '--limit', '0'), ('brief', '--offset', '1'),
                          ('distill', 'unexpected'), ('search', 'x', '--type', 'invented'), ('--data', 'timeline')):
            self.report(*arguments, status=2)
            self.assertFalse(self.store.exists())
        for selector in ('--json', '--data', '--data-v2'):
            with open('/dev/full', 'wb') as sink:
                self.execute(['mem', selector, 'distill'], status=125, stdout=sink)
            self.assertFalse(self.store.exists())
        limited = self.report('distill', '--budget', '1', status=75)
        self.assertEqual(limited['error'], 'receipt_admission_failed')
        self.assertFalse(self.store.exists())
        self.assertFalse((self.directory / '.jash/.memory-lock').exists())

    def test_publication_faults_report_actual_applied_and_durable_phases(self):
        self.seed(); self.report('distill'); before = self.store.read_bytes()
        self.chain([(1, 100, 0, 'replacement', b'ls')])
        for fault in ('file_sync', 'file_close', 'rename', 'directory_sync'):
            with self.subTest(fault=fault):
                self.store.write_bytes(before)
                self.env.update(LD_PRELOAD=self.fault_library, JASH_TEST_MEMORY_ROOT=str(self.directory),
                                JASH_TEST_MEMORY_FILE='mem.tsv', JASH_TEST_MEMORY_FAULT=fault)
                try: value, _, _ = self.native('distill', status=1)
                finally:
                    for key in ('LD_PRELOAD', 'JASH_TEST_MEMORY_ROOT', 'JASH_TEST_MEMORY_FILE', 'JASH_TEST_MEMORY_FAULT'): self.env.pop(key, None)
                self.assertEqual(value['applied'], fault == 'directory_sync')
                self.assertFalse(value['durable']); self.assertFalse(value['attestation_requested'])
                self.assertIsNotNone(value['error'])
                self.assertEqual(self.store.read_bytes() == before, fault != 'directory_sync')
                self.assertFalse(list(self.store.parent.glob('.memory-*.tmp')))

    def test_all_query_modes_and_both_data_aliases_ignore_presentation_budget(self):
        self.seed(); self.report('distill')
        card = self.report('timeline')['results'][0]['card']['id']
        for arguments in (('timeline',), ('show', card), ('search', 'cat source.c'), ('verify', card), ('brief',)):
            with self.subTest(arguments=arguments):
                json_value = self.report(*arguments)
                for selector in ('--data', '--data-v2'):
                    self.env['JASH_LLM_BUDGET'] = '1'
                    try: data_value, _, _ = self.native(*arguments, selector=selector)
                    finally: self.env.pop('JASH_LLM_BUDGET')
                    if data_value['brief']:
                        data_value['brief']['mesh_tasks']['observed_at'] = json_value['brief']['mesh_tasks']['observed_at']
                    self.assertEqual(data_value, json_value)
        self.env['JASH_LLM_BUDGET'] = '1'
        self.native('distill', '--budget', '1', selector='--data')

    def test_schema_rejects_missing_fields_false_proof_and_unknown_success(self):
        self.seed(); self.report('distill')
        value = self.report('timeline')
        absent = copy.deepcopy(value); del absent['store']['read_complete']
        self.assertFalse(self.validator.is_valid(absent))
        invented = dict(value, verified_everything=True)
        self.assertFalse(self.validator.is_valid(invented))
        preview = copy.deepcopy(value); preview['results'][0]['card']['files_inventory_complete'] = True
        self.assertFalse(self.validator.is_valid(preview))
        card = value['results'][0]['card']['id']
        verified = self.report('verify', card)
        verified['verification']['cryptographically_verified'] = True
        self.assertFalse(self.validator.is_valid(verified))

    def test_strict_cancel_at_held_publication_lock_preserves_store_and_session(self):
        self.seed()
        control, child = socket.socketpair()
        env = dict(self.env, JASH_AGENT_CANCEL_FD=str(child.fileno()))
        agent = management.BUILD / 'jash-agentd'
        proc = subprocess.Popen([str(agent)], cwd=self.directory, env=env, pass_fds=(child.fileno(),),
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        child.close(); pending = {}

        def line(stream):
            descriptor = stream.fileno(); data = pending.pop(descriptor, b'')
            deadline = time.monotonic() + 10
            while b'\n' not in data:
                self.assertTrue(select.select([descriptor], [], [], max(0, deadline - time.monotonic()))[0], ('timeout', data))
                chunk = os.read(descriptor, 65536); self.assertTrue(chunk, ('EOF', data)); data += chunk
            body, _, rest = data.partition(b'\n'); pending[descriptor] = rest
            return json.loads(body)

        try:
            self.assertTrue(line(proc.stdout)['cancel']['enabled'])
            lock_path = self.directory / '.jash/.memory-lock'
            with lock_path.open('wb') as lock:
                lock_path.chmod(0o600); fcntl.lockf(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                request = dict(id='mem-held-lock', protocol_version=2,
                               argv=['mem', '--data-v2', 'distill'], caps=['effect:mutate'], cancelable=True)
                proc.stdin.write((json.dumps(request) + '\n').encode())
                start = line(proc.stdout); self.assertEqual(start.get('event'), 'start', start)
                chunks = []
                # A delivered D/G prefix proves admission happened while the
                # independent process still owns the publication lock.
                while not chunks:
                    event = line(proc.stdout); self.assertEqual(event.get('event'), 'chunk', event)
                    if event['stream'] == 'stdout': chunks.append(base64.b64decode(event['data_b64']))
                self.assertFalse(self.store.exists())
                control.sendall(b'{"cancel":"mem-held-lock"}\n')
                self.assertEqual(line(control), {'cancel':'mem-held-lock','accepted':True})
                while True:
                    event = line(proc.stdout)
                    if event.get('event') != 'chunk': break
                    if event['stream'] == 'stdout': chunks.append(base64.b64decode(event['data_b64']))
                self.assertEqual(event['rc'], 130, event); self.assertTrue(event['cancelled'])
                values, terminal = native_stream(b''.join(chunks))
                self.assertEqual((terminal['state'], terminal['status']), (1,130))
                self.assertEqual(len(values), 1)
                value = project(values[0]); self.validator.validate(value)
                self.assertFalse(value['applied']); self.assertEqual(value['error'], 'cancelled')
                self.assertFalse(self.store.exists())
            proc.stdin.write((json.dumps(dict(id='mem-after', protocol_version=2,
                                argv=['mem','--json','timeline'], caps=['effect:read'])) + '\n').encode())
            self.assertEqual(line(proc.stdout).get('event'), 'start')
            while True:
                event = line(proc.stdout)
                if event.get('event') != 'chunk': break
            self.assertEqual(event['rc'], 0, event)
        finally:
            control.close()
            if proc.poll() is None: proc.kill()
            proc.communicate(timeout=10)


if __name__ == '__main__':
    unittest.main(verbosity=2)
