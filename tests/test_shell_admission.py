#!/usr/bin/env python3
"""Real post-command state observes that failed response admission prevents effects."""
import json
import os
import shlex
import unittest
import test_shell_structured as shell

class ShellAdmission(unittest.TestCase):
    setUp = shell.ShellState.setUp
    runj = shell.ShellState.runj
    def test_shell_variables_reject_failed_admission(self):
        for selector in ('--json', '--data', '--data-v2'):
            for command in ('export', 'declare', 'typeset', 'readonly', 'array'):
                with self.subTest(selector=selector, command=command):
                    result = self.runj(f'{command} {selector} DENIED=value >/dev/full; declare --json -p DENIED', status=1)
                    self.assertIsNone(json.loads(result.stdout)['items'][0]['value'])
            result = self.runj(f'export DENIED=value; unset {selector} DENIED >/dev/full; declare --json -p DENIED')
            self.assertEqual(json.loads(result.stdout)['items'][0]['value'], 'value')

    def test_state_changes_reject_failed_admission(self):
        for selector in ('--json', '--data', '--data-v2'):
            with self.subTest(selector=selector):
                result = self.runj(f'shopt -u nullglob; shopt {selector} -s nullglob >/dev/full; shopt --json nullglob')
                self.assertFalse(json.loads(result.stdout)['items'][0]['enabled'])
                result = self.runj(f'umask 022; umask {selector} 077 >/dev/full; umask --json')
                self.assertEqual(json.loads(result.stdout)['items'][0]['mask'], 0o22)
                result = self.runj(f'trap {selector} ignored USR1 >/dev/full; trap --json -p USR1')
                self.assertIsNone(json.loads(result.stdout)['items'][0]['action'])

    def test_directories_reject_failed_admission(self):
        target = self.directory / 'target directory'
        target.mkdir()
        for selector in ('--json', '--data', '--data-v2'):
            for command in ('cd', 'pushd'):
                with self.subTest(selector=selector, command=command):
                    result = self.runj(f'{command} {selector} {shlex.quote(str(target))} >/dev/full; pwd --json')
                    self.assertEqual(json.loads(result.stdout)['cwd'], str(self.directory))
            result = self.runj(f'pushd {shlex.quote(str(target))} >/dev/null; popd {selector} >/dev/full; pwd --json')
            self.assertEqual(json.loads(result.stdout)['cwd'], str(target))

if __name__ == '__main__':
    unittest.main()
