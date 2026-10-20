#!/usr/bin/env python3
"""Digest semantic results against real project bytes, stores and Git state."""
import base64
import copy
import fcntl
import json
import os
from pathlib import Path
import subprocess
import time

from jsonschema import ValidationError
import native_data_fixture as f
from contract_schema_fixture import projection, validate_contract


class DigestNative(f.NativeQueryCase):
    def report(self, *operands, mode='--json', rc=0, strict=False):
        result = self.run_command(['digest', mode, *operands], strict=strict,
                                  caps=['effect:read', 'effect:mutate'])
        self.assertEqual(result.returncode, rc, result.stderr)
        if mode == '--json':
            value = json.loads(result.stdout)
        else:
            stream = f.stream(result.stdout, credit_kind='none')
            self.assertEqual((stream.producer, stream.state), (rc, 0 if rc == 0 else 2))
            self.assertEqual(len(stream.items), 1)
            value = projection(stream.items[0])
        validate_contract('digest', value)
        return value

    def seed(self):
        (self.work / 'README.md').write_bytes(b'abc')
        (self.work / 'main.py').write_bytes(b'print("hello")\n')
        (self.work / 'empty').mkdir()
        (self.work / 'opaque.bin').write_bytes(b'not part of the recognized inventory')
        (self.work / 'linked.py').symlink_to('main.py')

    def test_manifest_and_fingerprint_use_actual_recognized_inventory(self):
        self.seed()
        first = self.report('--manifest', strict=True)
        rows = {x['path']: x for x in first['entries']}
        self.assertEqual(set(rows), {'README.md', 'main.py', 'empty'})
        self.assertEqual(rows['README.md']['bytes'], 3)
        self.assertEqual(rows['README.md']['blake3'], '6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85')
        self.assertEqual(rows['main.py']['language'], 'Python')
        self.assertEqual(rows['empty'], {'path': 'empty', 'kind': 'directory', 'language': None})
        self.assertEqual(first['coverage']['excluded_files'], 1)
        self.assertEqual(first['coverage']['excluded_special'], 1)
        for mode in ('--data', '--data-v2'):
            self.assertEqual(self.report('--manifest', mode=mode, strict=True), first)
        fingerprint = self.report('--fingerprint', mode='--data')
        self.assertEqual(first['fingerprint'], fingerprint['fingerprint'])
        self.assertFalse((Path(self.env['HOME']) / '.jash/digest').exists())

    def test_cached_project_records_publication_cas_and_real_ledger(self):
        self.seed()
        first = self.report('--brief')
        self.assertTrue(first['cache']['published']); self.assertTrue(first['cache']['durable'])
        self.assertFalse(first['cache']['hit'])
        self.assertTrue(first['cache']['cas_complete'])
        self.assertEqual(first['cache']['cas_verified_objects'], 2)
        self.assertTrue(first['cache']['ledger_written']); self.assertTrue(first['cache']['ledger_durable'])
        second = self.report('--brief', mode='--data-v2')
        self.assertTrue(second['cache']['hit']); self.assertFalse(second['cache']['published'])
        root = Path(self.env['HOME']) / '.jash/digest'
        self.assertTrue((root / (first['cache']['key'] + '.md')).is_file())
        ledger = self.report('--ledger', '2', strict=True)
        self.assertEqual(ledger['total_rows'], 2)
        self.assertEqual([r['served_from_cache'] for r in ledger['receipts']], [False, True])
        self.assertEqual([r['fingerprint'] for r in ledger['receipts']], [first['fingerprint']] * 2)
        cache = self.report('--cache-status', mode='--data', strict=True)
        self.assertEqual(cache['entries'], 1); self.assertGreater(cache['bytes'], 0)

    def test_fetch_verifies_content_hash_and_repairs_corruption_on_refresh(self):
        self.seed(); report = self.report('--brief')
        hash_value = next(row['blake3'] for row in report['entries'] if row['path'] == 'README.md')
        content = self.report('--fetch', hash_value, mode='--data', strict=True)
        self.assertEqual(base64.b64decode(content['content']['data']), b'abc')
        path = Path(self.env['HOME']) / '.jash/digest/cas' / hash_value[:2] / hash_value[2:]
        path.write_bytes(b'bad')
        self.report('--fetch', hash_value, rc=1)
        self.report('--brief', '--refresh')
        self.assertEqual(path.read_bytes(), b'abc')

    def test_ledger_rejects_wrong_types_and_unknown_fields(self):
        self.seed(); self.report('--brief')
        path = Path(self.env['HOME']) / '.jash/digest/ledger.jsonl'
        original = json.loads(path.read_bytes())
        for key, value in [('files', '3'), ('ts', -1), ('bytes_out', 2**64),
                           ('fingerprint', 'x'*64), ('level', 'imagined'),
                           ('served_from_cache', 1), ('root', {'encoding': 'base64', 'data': '???='})]:
            changed = dict(original); changed[key] = value
            path.write_text(json.dumps(changed) + '\n')
            result = self.report('--ledger', mode='--data', rc=1)
            self.assertEqual(result['error'], 'ledger_invalid_or_over_limit')
        changed = dict(original); changed['made_up'] = True
        path.write_text(json.dumps(changed) + '\n')
        self.report('--ledger', rc=1)

    def test_ledger_lock_deadline_reports_prior_effects(self):
        self.seed()
        directory = Path(self.env['HOME']) / '.jash/digest'
        directory.mkdir(parents=True, mode=0o700)
        with (directory / 'ledger.jsonl').open('wb') as ledger:
            os.chmod(ledger.name, 0o600)
            fcntl.flock(ledger, fcntl.LOCK_EX | fcntl.LOCK_NB)
            started = time.monotonic()
            result = self.report('--brief', mode='--data', rc=1)
            elapsed = time.monotonic() - started
        self.assertGreaterEqual(elapsed, 4.8); self.assertLess(elapsed, 10)
        self.assertEqual(result['error'], 'ledger_append_failed')
        self.assertTrue(result['cache']['published'])
        self.assertFalse(result['cache']['ledger_written'])

    def test_cas_quota_counts_existing_bytes_before_adding_content(self):
        self.seed()
        shard = Path(self.env['HOME']) / '.jash/digest/cas/ff'
        shard.mkdir(parents=True, mode=0o700)
        with (shard / ('f'*62)).open('wb') as object_file:
            object_file.truncate(512 * 1024 * 1024 - 2)
        os.chmod(object_file.name, 0o600)
        result = self.report('--brief', rc=1)
        self.assertEqual(result['error'], 'content_store_incomplete')
        self.assertFalse(result['cache']['cas_complete'])
        self.assertFalse(result['cache']['ledger_written'])
        self.assertEqual(sum(p.stat().st_size for p in shard.parent.glob('*/*') if p.is_file()),
                         512 * 1024 * 1024 - 2)

    def test_manifest_output_is_usable_as_diff_base_and_native_output_decodes(self):
        self.seed(); manifest = self.root / 'base manifest.json'
        receipt = self.report('--manifest', '--output', str(manifest))
        self.assertTrue(receipt['applied']); self.assertTrue(receipt['durable'])
        original = json.loads(manifest.read_bytes()); validate_contract('digest', original)
        (self.work / 'README.md').write_bytes(b'modified\n')
        (self.work / 'main.py').unlink(); (self.work / 'added.c').write_bytes(b'int x;\n')
        diff = self.report('--diff-base', str(manifest), mode='--data', strict=True)
        self.assertEqual([x['path'] for x in diff['added']], ['added.c'])
        self.assertEqual([x['path'] for x in diff['modified']], ['README.md'])
        self.assertEqual([x['path'] for x in diff['removed']], ['main.py'])
        output = self.root / 'snapshot.jtv2'
        self.report('--manifest', '--output', str(output), mode='--data-v2')
        stream = f.stream(output.read_bytes(), credit_kind='none')
        self.assertEqual(stream.state, 0); validate_contract('digest', projection(stream.items[0]))

    def test_literal_raw_filename_and_control_bytes_roundtrip(self):
        name = b'raw\xff\n.py'
        path = os.fsencode(self.work) + b'/' + name
        fd = os.open(path, os.O_CREAT | os.O_WRONLY, 0o600)
        os.write(fd, b'x\0\xff'); os.close(fd)
        manifest = self.report('--manifest', mode='--data')
        row = manifest['entries'][0]
        self.assertEqual(base64.b64decode(row['path']['data']), name)
        self.assertEqual(row['bytes'], 3)
        self.assertEqual(self.report('--manifest'), manifest)

    def test_readonly_queries_do_not_create_cache_and_fifo_fails_promptly(self):
        self.assertEqual(self.report('--ledger', strict=True)['receipts'], [])
        self.assertEqual(self.report('--cache-status', strict=True)['entries'], 0)
        directory = Path(self.env['HOME']) / '.jash/digest'
        self.assertFalse(directory.exists())
        directory.mkdir(parents=True, mode=0o700)
        os.mkfifo(directory / 'ledger.jsonl')
        self.report('--ledger', mode='--data', rc=1)
        os.mkfifo(self.work / 'bad-base')
        self.report('--diff-base', str(self.work / 'bad-base'), rc=1)

    def test_sink_and_json_budget_rejection_precede_cache_side_effects(self):
        self.seed(); cache = Path(self.env['HOME']) / '.jash/digest'
        for mode in ('--json', '--data', '--data-v2'):
            with open('/dev/full', 'wb') as sink:
                result = subprocess.run([str(f.JASH), '--exec-json', json.dumps({'request_version': 2, 'argv': ['digest', mode, '--brief']})],
                    env=self.env, cwd=self.work, stdout=sink, stderr=subprocess.PIPE, timeout=20)
            self.assertEqual(result.returncode, 125, result.stderr)
            self.assertFalse(cache.exists())
        self.env['JASH_LLM_BUDGET'] = '1'
        result = self.run_command(['digest', '--json', '--brief'])
        self.assertEqual(result.returncode, 75); self.assertFalse(cache.exists())
        self.report('--brief', mode='--data')
        self.assertTrue(cache.exists())

    def test_file_and_value_bounds_fail_before_publication(self):
        with (self.work / 'too-big.py').open('wb') as file: file.truncate(8388609)
        result = self.report('--brief', rc=1)
        self.assertFalse(result['ok'])
        self.assertFalse((Path(self.env['HOME']) / '.jash/digest').exists())
        for operands in [('--jobs', '33'), ('--manifest', '--fingerprint'), ('--session', '--jobs', '1'), ('--ledger', '0')]:
            self.report(*operands, rc=2)

    def test_session_uses_recorded_bytes_and_explicit_git_availability(self):
        directory = Path(self.env['HOME']) / '.jash'; directory.mkdir(exist_ok=True, mode=0o700)
        text = ('first command', 'a very long ' + 'x'*1500)
        chain = ''.join(f'{i+1} 1700000000.000000001 {"0"*64} tag=- rc={i} cmd={command}\n'
                        for i, command in enumerate(text))
        (directory / 'chain.log').write_text(chain)
        result = self.report('--session', '--no-git', '--last', '2', mode='--data', strict=True)
        self.assertEqual([x['command_recorded'] for x in result['commands']], list(text))
        self.assertEqual(result['failed_commands'], 1)
        self.assertFalse(result['source']['cryptographically_verified'])
        self.assertFalse(result['git_requested']); self.assertIsNone(result['git'])
        failed = self.report('--session', rc=128)
        self.assertFalse(failed['complete'])
        self.assertFalse(failed['git']['status']['available'])

    def test_schema_positive_and_negative_controls(self):
        self.seed(); value = self.report('--no-cache')
        for field in value:
            broken = copy.deepcopy(value); del broken[field]
            with self.assertRaises(ValidationError): validate_contract('digest', broken)
        broken = copy.deepcopy(value); broken['cache']['imagined'] = True
        with self.assertRaises(ValidationError): validate_contract('digest', broken)
        self.report('--help', mode='--data')


if __name__ == '__main__':
    import unittest
    unittest.main()
