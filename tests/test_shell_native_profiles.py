#!/usr/bin/env python3
"""Single-command native state boundaries in general and strict execution."""
import json
import os
import unittest
import native_data_fixture as fixture


class StateProfiles(fixture.NativeQueryCase):
    def native(self, argv, *, rc=0, **kwargs):
        result = self.run_command(argv, **kwargs)
        self.assertEqual(result.returncode, rc, result.stderr)
        value = fixture.stream(result.stdout, credit_kind="none")
        self.assertEqual(value.producer, rc)
        return value

    def test_scalar_state_providers_have_native_observations_in_both_profiles(self):
        cases = [
            (['export', 'A=one two'], 'value', 'string', 'one two'),
            (['readonly', 'A=keep'], 'readonly', 'bool', True),
            (['declare', '-i', 'A=6*7'], 'value', 'string', '42'),
            (['typeset', 'A=literal'], 'value', 'string', 'literal'),
            (['array', 'A[9]=space value'], 'value', 'string', 'space value'),
            (['umask', '077'], 'mask', 'u64', 0o77),
            (['ulimit', '-Sn', '512'], 'soft', 'u64', 512),
            (['shopt', '-s', 'nullglob'], 'enabled', 'bool', True),
            (['trap', 'echo harmless', 'USR1'], 'action', 'string', 'echo harmless'),
        ]
        for strict in (False, True):
            for case, key, kind, expected in cases:
                for flag in ('--data', '--data-v2'):
                    with self.subTest(strict=strict, case=case, flag=flag):
                        result = self.native(case[:1] + [flag] + case[1:], strict=strict,
                                             caps=['effect:read', 'effect:mutate'])
                        self.assertEqual((result.count, result.state), (1, 0))
                        self.scalar(result.items[0], key, kind, expected)

    def test_native_directory_paths_and_state_snapshot(self):
        target = self.work / 'space directory'
        target.mkdir()
        for strict in (False, True):
            with self.subTest(strict=strict):
                result = self.native(['cd', '--data', str(target)], strict=strict,
                                     caps=['effect:read', 'effect:mutate'])
                self.scalar(result.items[0], 'cwd', 'path', os.fsencode(target))
                result = self.native(['pushd', '--data-v2', str(target)], strict=strict,
                                     caps=['effect:read', 'effect:mutate'])
                self.assertEqual(result.count, 2)
                self.scalar(result.items[0], 'path', 'path', os.fsencode(target))
                result = self.native(['state', '--data', '--no-git'], strict=strict, caps=['effect:read'])
                state = result.items[0].value['state']
                self.scalar(state, 'cwd', 'path', os.fsencode(self.work))
                self.scalar(state, 'timeline_total', 'u64', 0)
                self.scalar(state.value['git'], 'preview', 'bool', True)

    def test_native_shell_options_and_environment(self):
        for strict in (False, True):
            with self.subTest(strict=strict):
                result = self.native(['set', '--data', '-o', 'pipefail'], strict=strict,
                                     caps=['effect:read', 'effect:mutate'])
                row = next(r for r in result.items if r.value['name'].value == 'pipefail')
                self.scalar(row, 'enabled', 'bool', True)
                result = self.native(['env', '--data'], strict=strict)
                row = next(r for r in result.items if r.value['name'].value == 'HOME')
                self.scalar(row, 'value', 'string', self.env['HOME'])
                result = self.native(['times', '--data'], strict=strict)
                self.assertGreater(self.scalar(result.items[0], 'ticks_per_second', 'u64'), 0)

    def test_invalid_native_state_modes_have_failed_terminals(self):
        for strict in (False, True):
            for command in ('set', 'shopt', 'trap', 'ulimit', 'umask', 'declare', 'cd', 'state'):
                with self.subTest(strict=strict, command=command):
                    result = self.native([command, '--data', '--bad'], strict=strict, rc=2,
                                         caps=['effect:read', 'effect:mutate', 'effect:exec'])
                    self.assertEqual((result.state, result.count), (2, 1 if command == 'state' else 0))
                    if command == 'state': self.scalar(result.items[0], 'error', 'symbol', 'invalid_arguments')


if __name__ == '__main__':
    unittest.main()
