#!/usr/bin/env python3
"""Observe real Git state and the running binary; no receipt-derived oracle."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import unittest
import native_data_fixture as f
from contract_schema_fixture import validate_contract


class RecoveryExtra(f.NativeQueryCase):
    def git(self, *args, check=True):
        return subprocess.run(['git', *args], cwd=self.work, env=self.env,
                              capture_output=True, check=check)

    def repository(self):
        self.git('init', '-q')
        self.git('config', 'user.name', 'Fixture')
        self.git('config', 'user.email', 'fixture@example.invalid')
        (self.work / 'original file').write_bytes(b'initial\n')
        self.git('add', '--', 'original file')
        self.git('commit', '-qm', 'initial')

    def report(self, argv, rc=0, strict=False):
        result = self.run_command(argv, strict=strict, caps=['effect:read', 'effect:exec'])
        self.assertEqual(result.returncode, rc, result.stderr)
        value = json.loads(result.stdout)
        validate_contract(argv[0], value)
        return value

    def test_revert_git_add_changes_real_index_in_both_profiles(self):
        self.repository()
        for strict in (False, True):
            (self.work / 'original file').write_bytes(b'changed\n')
            self.git('add', '--', 'original file')
            self.assertNotEqual(self.git('diff', '--cached', '--quiet', check=False).returncode, 0)
            report = self.report(['revert', '--json', 'git', 'add'], strict=strict)
            self.assertEqual(self.git('diff', '--cached', '--quiet', check=False).returncode, 0)
            self.assertEqual(report['items'][0]['argv'], ['git', 'reset'])
            self.assertTrue(report['items'][0]['started'])
            self.assertFalse(report['items'][0]['domain_state_verified'])

    def test_revert_preserves_literal_paths_and_multi_step_order(self):
        self.repository()
        moved = 'moved " name ∞'
        self.git('mv', '--', 'original file', moved)
        result = self.run_command(['revert', '--data', 'git', 'mv', 'original file', moved])
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = f.stream(result.stdout, credit_kind='none').items
        self.assertEqual([x.value for x in rows[0].value['argv'].value],
                         ['git', 'mv', '--', moved, 'original file'])
        self.assertTrue((self.work / 'original file').exists())
        self.assertFalse((self.work / moved).exists())
        self.git('rm', '--', 'original file')
        report = self.report(['revert', '--json', 'git', 'rm', 'original file'])
        self.assertEqual([x['step'] for x in report['items']], [1, 2])
        self.assertEqual((self.work / 'original file').read_bytes(), b'initial\n')
        self.assertEqual(self.git('status', '--porcelain').stdout, b'')

    def test_revert_commit_updates_head_and_preserves_index(self):
        self.repository()
        before = self.git('rev-parse', 'HEAD').stdout
        (self.work / 'new file').write_bytes(b'new\n')
        self.git('add', '--', 'new file'); self.git('commit', '-qm', 'second')
        self.report(['revert', '--json', 'git', 'commit'])
        self.assertEqual(self.git('rev-parse', 'HEAD').stdout, before)
        self.assertIn(b'new file', self.git('diff', '--cached', '--name-only').stdout)

    def test_failed_child_prevents_next_inverse_and_sink_failure_prevents_first(self):
        bin_dir = self.work / 'bin'; bin_dir.mkdir()
        docker = bin_dir / 'docker'
        docker.write_text('#!/bin/sh\nprintf "%s\\n" "$1" >> calls\nexit 9\n')
        docker.chmod(0o700); self.env['PATH'] = str(bin_dir) + ':' + self.env['PATH']
        report = self.report(['revert', '--json', 'docker', 'literal id'], 9)
        self.assertEqual(len(report['items']), 1)
        self.assertEqual((self.work / 'calls').read_text(), 'stop\n')
        (self.work / 'calls').unlink()
        for mode in ('--json', '--data'):
            request = {'request_version': 2, 'argv': ['revert', mode, 'docker', 'id']}
            with open('/dev/full', 'wb') as full:
                result = subprocess.run([str(f.JASH), '--exec-json', json.dumps(request)],
                    env=self.env, cwd=self.work, stdout=full, stderr=subprocess.PIPE)
            self.assertEqual(result.returncode, 125, result.stderr)
            self.assertFalse((self.work / 'calls').exists())

    def test_revert_delivery_failure_stops_between_steps(self):
        bin_dir = self.work / 'bin'; bin_dir.mkdir()
        docker = bin_dir / 'docker'
        docker.write_text('#!/bin/sh\nprintf "%s\\n" "$1" >> calls\n')
        docker.chmod(0o700)
        shim = self.work / 'delivery.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                        str(f.ROOT / 'tests/serve_delivery_faults.c'), '-ldl', '-o', str(shim)], check=True)
        self.env.update(PATH=str(bin_dir) + ':' + self.env['PATH'], LD_PRELOAD=str(shim),
                        JASH_TEST_DELIVERY='after_first_write')
        result = self.run_command(['revert', '--json', 'docker', 'id'])
        self.assertEqual(result.returncode, 125, result.stderr)
        self.assertEqual((self.work / 'calls').read_text(), 'stop\n')

    def test_selfcheck_hash_matches_independent_digest_in_both_profiles(self):
        baseline = Path(self.env['HOME']) / '.jash/binary_hash'
        for strict, binary in ((False, f.JASH), (True, f.AGENT)):
            digest = hashlib.sha256(binary.read_bytes()).hexdigest()
            report = self.report(['selfcheck', '--json', 'verify'], strict=strict)
            self.assertEqual(report['current_hash'], digest)
            self.assertEqual(report['binary_size'], binary.stat().st_size)
            self.assertFalse(report['baseline_available_before'])
            self.assertIsNone(report['verified'])
            self.assertFalse(baseline.exists())

    def test_selfcheck_match_mismatch_and_corrupt_baseline_are_distinct(self):
        baseline = Path(self.env['HOME']) / '.jash/binary_hash'
        baseline.parent.mkdir(exist_ok=True)
        digest = hashlib.sha256(f.JASH.read_bytes()).hexdigest()
        baseline.write_text(digest + '\n')
        result = self.run_command(['selfcheck', '--data', 'verify'])
        self.assertEqual(result.returncode, 0, result.stderr)
        row = f.stream(result.stdout, credit_kind='none').items[0]
        self.scalar(row, 'verified', 'bool', True)
        self.scalar(row, 'binary_size', 'filesize', f.JASH.stat().st_size)
        self.scalar(row, 'baseline', 'path', os.fsencode(baseline))
        baseline.write_text('0' * 64 + '\n')
        report = self.report(['selfcheck', '--json'], 1)
        self.assertTrue(report['complete']); self.assertFalse(report['verified'])
        baseline.write_bytes(b'broken\n')
        report = self.report(['selfcheck', '--json'], 1)
        self.assertFalse(report['complete']); self.assertIsNone(report['verified'])
        self.assertEqual(report['error'], 'baseline_invalid_or_unreadable')
        self.assertEqual(baseline.read_bytes(), b'broken\n')

    def test_selfcheck_does_not_block_on_fifo_or_follow_baseline_symlink(self):
        baseline = Path(self.env['HOME']) / '.jash/binary_hash'
        baseline.parent.mkdir(exist_ok=True)
        os.mkfifo(baseline)
        self.report(['selfcheck', '--json'], 1)
        baseline.unlink()
        target = self.work / 'target'; target.write_text('0' * 64 + '\n')
        baseline.symlink_to(target)
        self.report(['selfcheck', '--json'], 1)
        self.assertTrue(baseline.is_symlink())

    def test_baseline_store_keeps_existing_human_approval_boundary(self):
        for operation in ('store', 'force'):
            for mode in ('--json', '--data'):
                result = self.run_command(['selfcheck', mode, operation])
                self.assertEqual(result.returncode, 1, result.stderr)
                report = json.loads(result.stdout) if mode == '--json' else None
                if report: self.assertEqual(report['error'], 'interactive_terminal_required')
                else:
                    stream = f.stream(result.stdout, credit_kind='none')
                    self.assertEqual((stream.state, stream.producer), (2, 1))
                self.assertFalse((Path(self.env['HOME']) / '.jash/binary_hash').exists())


if __name__ == '__main__':
    unittest.main()
