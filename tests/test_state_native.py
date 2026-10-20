#!/usr/bin/env python3
"""Shell facts, explicit Git observations and pre-execution admission."""
import base64
import copy
import json
import os
from pathlib import Path
import subprocess
import unittest
from jsonschema import ValidationError
import native_data_fixture as f
from contract_schema_fixture import projection, validate_contract


class StateNative(f.NativeQueryCase):
    def report(self, *args, mode='--json', strict=False, status=0, complete=True):
        caps = ['effect:read'] if args == ('--no-git',) else ['effect:exec']
        result = self.run_command(['state', mode, *args], strict=strict, caps=caps)
        self.assertEqual(result.returncode, status, result.stderr)
        if mode == '--json':
            value = json.loads(result.stdout)
        else:
            wire = f.stream(result.stdout, credit_kind='none')
            self.assertEqual(wire.producer, status)
            self.assertEqual(wire.state, 2 if status else 0 if complete else 3)
            self.assertEqual(wire.count, 1)
            value = projection(wire.items[0])
        validate_contract('state', value)
        return value

    def git(self, *args):
        result = subprocess.run(['git', *args], cwd=self.work, env=self.env,
                                capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def repository(self):
        self.git('init', '-q')
        self.git('config', 'user.name', 'State Fixture')
        self.git('config', 'user.email', 'state@example.invalid')
        (self.work/'tracked').write_text('first\n')
        self.git('add', '--', 'tracked')
        self.git('commit', '-qm', 'known commit')

    def test_local_snapshot_in_both_profiles_has_actual_typed_values(self):
        self.env['JASH_INTENT'] = 'known intent'
        self.env['JASH_BYTES'] = os.fsdecode(b'value-\xff')
        for strict in (False, True):
            for mode in ('--json', '--data', '--data-v2'):
                with self.subTest(strict=strict, mode=mode):
                    state = self.report('--no-git', mode=mode, strict=strict)['state']
                    self.assertEqual(state['cwd'], str(self.work))
                    self.assertEqual(state['intent'], 'known intent')
                    self.assertEqual(base64.b64decode(state['env']['JASH_BYTES']['data']), b'value-\xff')
                    self.assertEqual(state['timeline_total'], 0)
                    self.assertTrue(state['complete'])
                    self.assertFalse(state['git']['requested'])
                    self.assertIsNone(state['git']['present'])
                    self.assertIsNone(state['git']['dirty'])

    def test_git_facts_and_diff_are_real_and_shared_by_json_and_native(self):
        self.repository()
        (self.work/'tracked').write_text('first\nsecond\n')
        (self.work/'space name').write_bytes(b'\x00')
        for mode, strict in (('--json', False), ('--data', False), ('--data-v2', True)):
            result = self.report(mode=mode, strict=strict)['state']
            git = result['git']
            self.assertTrue(git['present']); self.assertTrue(git['dirty'])
            self.assertTrue(result['complete'])
            self.assertEqual(git['branch'], self.git('branch', '--show-current').stdout.decode().strip())
            files = git['observation']['status']['payload']['files']
            self.assertEqual({row['path'] for row in files}, {'tracked', 'space name'})
            latest = git['observation']['latest_commit']['payload']['commits']
            self.assertEqual(latest[0]['hash'], self.git('rev-parse', 'HEAD').stdout.decode().strip())
            self.assertEqual(latest[0]['message'], 'known commit')
            self.assertEqual(git['diff_observation']['payload']['total']['insertions'], 1)
            self.assertIn('known commit', git['recent_commits'])

    def test_git_callbacks_are_not_executed(self):
        self.repository()
        marker = self.work/'callback-marker'
        helper = self.work/'hook'
        helper.write_text('#!/bin/sh\ntouch '+str(marker)+'\n')
        helper.chmod(0o700)
        self.git('config', 'core.fsmonitor', str(helper))
        self.git('config', 'diff.external', str(helper))
        (self.work/'tracked').write_text('changed\n')
        self.report()
        self.assertFalse(marker.exists())

    def test_git_previews_declare_limits_while_retaining_structured_facts(self):
        self.repository()
        message = 'x'*5000
        self.git('commit', '--allow-empty', '-qm', message)
        for i in range(25): (self.work/f'untracked-{i}').write_text('data')
        git = self.report()['state']['git']
        self.assertTrue(git['status_preview_truncated'])
        self.assertTrue(git['recent_preview_truncated'])
        self.assertLessEqual(len(git['recent_commits'].encode()),4096)
        self.assertEqual(len(git['observation']['status']['payload']['files']),25)
        self.assertEqual(git['observation']['latest_commit']['payload']['commits'][0]['message'],message)

    def test_failed_git_does_not_become_clean_or_empty_success(self):
        fake = self.work/'bin'; fake.mkdir()
        git = fake/'git'; git.write_text('#!/bin/sh\nprintf "observed failure" >&2\nexit 17\n'); git.chmod(0o700)
        self.env['PATH'] = str(fake)+':'+os.defpath
        for mode in ('--json', '--data-v2'):
            state = self.report(mode=mode, complete=False)['state']
            self.assertFalse(state['complete'])
            self.assertIsNone(state['git']['dirty']); self.assertIsNone(state['git']['present'])
            observed = state['git']['observation']['status']
            self.assertEqual(observed['command_status'], 17)
            self.assertFalse(observed['available'])
            self.assertIsNone(observed['payload'])

    def test_missing_head_is_explicit_and_diff_still_observed(self):
        self.git('init', '-q')
        result = self.report(mode='--data-v2', complete=False)['state']['git']
        self.assertTrue(result['present'])
        self.assertTrue(result['observation']['status']['available'])
        self.assertFalse(result['observation']['latest_commit']['available'])
        self.assertTrue(result['diff_observation']['available'])

    def test_budget_and_failed_sink_prevent_git_execution(self):
        fake = self.work/'bin'; fake.mkdir()
        marker = self.work/'git-started'
        git = fake/'git'; git.write_text('#!/bin/sh\ntouch '+str(marker)+'\nexit 17\n'); git.chmod(0o700)
        self.env['PATH'] = str(fake)+':'+os.defpath
        self.env['JASH_LLM_BUDGET'] = '1'
        result = self.run_command(['state', '--json'], caps=['effect:exec'])
        self.assertEqual(result.returncode, 75, result.stderr)
        self.assertFalse(marker.exists())
        self.env.pop('JASH_LLM_BUDGET')
        with open('/dev/full', 'wb') as output:
            result = subprocess.run([str(f.JASH), '--exec-json', json.dumps({'argv':['state','--data-v2']})],
                                    cwd=self.work, env=self.env, stdout=output, stderr=subprocess.PIPE, timeout=10)
        self.assertEqual(result.returncode, 125, result.stderr)
        self.assertFalse(marker.exists())

    def test_native_ignores_presentation_budget_and_keeps_own_limits(self):
        self.env['JASH_LLM_BUDGET'] = '1'
        self.report('--no-git', mode='--data-v2')
        self.env.pop('JASH_LLM_BUDGET')
        for i in range(100): self.env[f'JASH_LARGE_{i}'] = 'x'*12000
        result = self.run_command(['state','--data-v2','--no-git'], caps=['effect:read'])
        self.assertEqual(result.returncode, 125, result.stderr)
        wire = f.stream(result.stdout, credit_kind='none')
        self.assertEqual((wire.state,wire.producer), (2,125))

    def test_exact_argv_and_exec_authority(self):
        for args in (('--bad',), ('--no-git','extra'), ('--json',), ('--no-git --extra',)):
            self.report(*args, status=2)
        result = self.run_command(['state','--data-v2'], strict=True, caps=['effect:read'])
        self.assertEqual(result.returncode, 125, result.stderr)
        self.report('--no-git', mode='--data-v2', strict=True)

    def test_schemas_reject_unknown_or_missing_facts(self):
        value = self.report('--no-git')
        for action in ('unknown','missing'):
            broken=copy.deepcopy(value)
            if action=='unknown': broken['state']['made_up']=True
            else: del broken['state']['git']['requested']
            with self.assertRaises(ValidationError): validate_contract('state',broken)


if __name__ == '__main__':
    unittest.main()
