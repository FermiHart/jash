"""B24 native semantic CLI providers; external effects stay explicit and checked."""
import base64
import copy
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

from test_structured_pipe import native_rows
from semantic_schema_fixture import semantic_validator, validate_semantic

ROOT = Path(__file__).resolve().parents[1]
JASH = Path(os.environ.get('JASH', ROOT / 'build/jash')).resolve()


class CLIAdapterContracts(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='jash-native-cli-')
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        self.work = root / 'work'; self.work.mkdir()
        self.home = root / 'home'; self.home.mkdir()
        self.bin = root / 'bin'; self.bin.mkdir()
        self.env = {key: value for key, value in os.environ.items() if not key.startswith('JASH_')}
        self.env.update(HOME=str(self.home), PATH=str(self.bin) + ':/usr/bin:/bin',
                        JASH_SIGIL='0', JASH_TIMELINE='0', JASH_MEMO='0', JASH_NUDGE='0',
                        JASH_ADOPT='0', JASH_BRIEF='0', JASH_LOOPGUARD='off', JASH_WORLD='0',
                        JASH_PREFETCH='0', GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL='/dev/null')

    def runj(self, argv, *, data=b'', ok=True, code=None, env=None):
        result = subprocess.run([str(JASH), '-c', code or shlex.join(argv)], cwd=self.work,
                                env=env or self.env, input=data, capture_output=True, timeout=15)
        if ok:
            self.assertEqual(result.returncode, 0, (argv, result.stdout[:400], result.stderr))
        return result

    def adapter(self, arguments, mode='--json', ok=True):
        result = self.runj(['json', mode, *arguments], ok=ok)
        if mode in ('--data', '--data-v2'):
            native_rows(result.stdout)
            projected = self.runj(['json', '--inspect-data'], data=result.stdout)
            document = json.loads(projected.stdout)
            semantic_validator('data-projection').validate(document)
            self.assertEqual(len(document['items']), 1)
            validate_semantic(document['items'][0])
            return result, document['items'][0], document['terminal']
        value = json.loads(result.stdout)
        validate_semantic(value)
        return result, value, None

    def fake(self, command, body):
        path = self.bin / command
        path.write_text('#!/usr/bin/python3\n' + body)
        path.chmod(0o700)

    def git(self, *args):
        result = subprocess.run(['/usr/bin/git', *args], cwd=self.work, env=self.env,
                                capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def repository(self):
        self.git('init', '--quiet')
        self.git('config', 'user.name', 'author\x01tail')
        self.git('config', 'user.email', 'fixture@example.invalid')

    def test_git_status_and_diff_preserve_raw_paths_and_native_units(self):
        self.repository()
        path = os.fsdecode(b'odd-\xff\nname.txt')
        (self.work / path).write_bytes(b'old\n')
        (self.work / 'binary').write_bytes(b'\0old')
        self.git('add', '--', path, 'binary')
        self.git('commit', '--quiet', '-m', 'subject\x01tail')
        (self.work / path).write_bytes(b'new\nextra\n')
        (self.work / 'binary').write_bytes(b'\0new')
        for command in ('status', 'diff'):
            _, expected, _ = self.adapter(['git', command])
            result, native, terminal = self.adapter(['git', command], '--data')
            self.assertEqual(native['files'], expected['files'])
            self.assertEqual(terminal['state'], 'complete')
            self.assertEqual(native['command_status'], 0)
            rows, _ = native_rows(result.stdout)
            self.assertEqual(rows[0][0]['output_bytes'][0], 4)
            file_values = rows[0][0]['files'][1]
            self.assertTrue(any(item[1]['path'] == (9, os.fsencode(path)) for item in file_values))
        binary = next(item for item in native['files'] if item['path'] == 'binary')
        self.assertTrue(binary['binary'])
        self.assertIsNone(binary['insertions'])
        self.assertIsNone(binary['deletions'])
        self.assertEqual(native['total']['binary_files'], 1)
        self.assertTrue(native['total']['text_files_only'])

    def test_git_log_uses_nul_framing_and_exact_argument_grammar(self):
        self.repository()
        (self.work / 'file').write_text('one')
        self.git('add', 'file')
        self.git('commit', '--quiet', '-m', 'subject\x01tail')
        for mode in ('--json', '--data-v2'):
            _, value, _ = self.adapter(['git', 'log', '1'], mode)
            self.assertEqual(len(value['commits']), 1)
            self.assertEqual(value['commits'][0]['message'], 'subject\x01tail')
            self.assertEqual(value['commits'][0]['author'], 'author\x01tail')
            self.assertEqual(len(value['commits'][0]['hash']), 40)
        for invalid in ('0', '-1', '2junk', '129'):
            self.assertNotEqual(self.runj(['json', '--data', 'git', 'log', invalid], ok=False).returncode, 0)

    def test_build_argv_and_child_status_are_preserved(self):
        self.fake('make', "import sys,json,pathlib\npathlib.Path('called').write_text(json.dumps(sys.argv[1:]))\nprint('file name.c:7:4: error: real failure')\nsys.exit(7)\n")
        operands = ['target with space', '$(touch forbidden)', '--data']
        for mode in ('--json', '--data'):
            result, value, terminal = self.adapter(['make', *operands], mode, ok=False)
            self.assertEqual(result.returncode, 7)
            self.assertEqual(json.loads((self.work / 'called').read_text()), operands)
            self.assertFalse((self.work / 'forbidden').exists())
            self.assertEqual(value['argv'], ['make', *operands])
            self.assertEqual(value['command_status'], 7)
            self.assertEqual(value['adapter_status'], 0)
            self.assertEqual(value['diagnostics'][0]['file'], 'file name.c')
            self.assertEqual(value['diagnostics'][0]['line'], 7)
            self.assertEqual(value['diagnostics'][0]['column'], 4)
            self.assertEqual(value['diagnostics'][0]['message'], 'real failure')
            if terminal:
                self.assertEqual(terminal['state'], 'failed')
                self.assertEqual(terminal['producer_status'], 7)
                self.assertEqual(terminal['transform_status'], 0)

    def test_npm_estimates_keep_unknowns_and_exact_large_counts(self):
        self.fake('npm', "print('Tests: 9007199254740993 passed, 2 failed, 9007199254740995 total')\n")
        result, value, _ = self.adapter(['npm', 'test'], '--data')
        self.assertEqual(value['tests'], {'passed': 9007199254740993, 'failed': 2, 'total': 9007199254740995})
        self.assertEqual(value['counter_source'], 'best-effort-text')
        rows, _ = native_rows(result.stdout)
        self.assertEqual(rows[0][0]['tests'][1]['passed'], (3, 9007199254740993))
        self.fake('npm', "print('custom runner finished; no standard summary')\n")
        _, unknown, _ = self.adapter(['npm'])
        self.assertEqual(unknown['tests'], {'passed': None, 'failed': None, 'total': None})

    def test_combined_capture_preserves_nul_and_invalid_utf8(self):
        raw = b'prefix\0raw-\xff\n'
        self.fake('cmake', 'import os\nos.write(1,' + repr(raw) + ')\n')
        for mode in ('--json', '--data'):
            _, value, _ = self.adapter(['cmake', '--build', 'directory with spaces'], mode)
            field = value['raw']
            if isinstance(field, str):
                observed = field.encode()
            else:
                observed = base64.b64decode(field['data'])
            self.assertEqual(observed, raw)
            self.assertEqual(value['output_bytes'], len(raw))
            self.assertFalse(value['diagnostics_complete'])

    def test_capture_limit_is_explicit_and_does_not_hide_command_status(self):
        self.fake('make', "import os\nos.write(1,b'x'*300000)\n")
        result, value, terminal = self.adapter(['make'], '--data', ok=False)
        self.assertEqual(result.returncode, 75)
        self.assertEqual(value['command_status'], 0)
        self.assertEqual(value['adapter_status'], 75)
        self.assertEqual(value['output_bytes'], 300000)
        self.assertEqual(len(value['raw']), 256 * 1024)
        self.assertTrue(value['truncated'])
        self.assertTrue(terminal['truncated'])
        self.assertEqual(terminal['state'], 'failed')
        self.assertEqual(terminal['producer_status'], 0)
        self.assertEqual(terminal['transform_status'], 75)

    def test_sink_and_budget_admission_precede_external_effects(self):
        self.fake('make', "import pathlib\npathlib.Path('effect').touch()\nprint('done')\n")
        for mode in ('--json', '--data', '--data-v2'):
            result = self.runj([], code=f'json {mode} make > /dev/full', ok=False)
            self.assertEqual(result.returncode, 125)
            self.assertFalse((self.work / 'effect').exists())
            constrained = dict(self.env, JASH_LLM_BUDGET='1')
            if mode == '--json':
                result = self.runj(['json', mode, 'make'], env=constrained, ok=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse((self.work / 'effect').exists())
            else:
                result = self.runj(['json', mode, 'make'], env=constrained)
                _, terminal = native_rows(result.stdout)
                self.assertEqual(terminal['state'], 0)
                self.assertTrue((self.work / 'effect').exists())
                (self.work / 'effect').unlink()
        bad = self.runj(['json', '--data', '--json', 'make'], ok=False)
        self.assertNotEqual(bad.returncode, 0)
        self.assertFalse((self.work / 'effect').exists())

    def test_machine_format_negative_controls_and_u64_counts(self):
        self.fake('git', "import os\nos.write(1,b'9007199254740993\\t2\\tpath\\x00')\n")
        _, value, _ = self.adapter(['git', 'diff'], '--data')
        self.assertEqual(value['files'][0]['insertions'], 9007199254740993)
        self.fake('git', "import os\nos.write(1,b'## main...origin/main [ahead 9007199254740993, behind 2]\\x00')\n")
        _, value, _ = self.adapter(['git', 'status'], '--data')
        self.assertEqual(value['git']['ahead'], 9007199254740993)
        for invalid in (b'-\t2\tpath\0', b'1\t2\tunterminated',
                        b'18446744073709551615\t0\ta\0' + b'1\t0\tb\0'):
            self.fake('git', 'import os\nos.write(1,' + repr(invalid) + ')\n')
            result, value, terminal = self.adapter(['git', 'diff'], '--data', ok=False)
            self.assertEqual(result.returncode, 75)
            self.assertEqual(value['error'], 'capture_or_parse_failed')
            self.assertEqual(terminal['transform_status'], 75)

    def test_git_observation_disables_configured_fsmonitor_and_signature_commands(self):
        self.repository()
        (self.work / 'tracked').write_bytes(b'fixture\n')
        self.git('add', '--', 'tracked')
        self.git('commit', '--quiet', '-m', 'fixture')
        monitor = self.bin / 'configured-monitor'
        marker = self.work / 'monitor-ran'
        monitor.write_text('#!/bin/sh\n: > ' + shlex.quote(str(marker)) + '\n')
        monitor.chmod(0o700)
        self.git('config', 'core.fsmonitor', str(monitor))
        self.git('status', '--porcelain=v1')
        self.assertTrue(marker.exists(), 'negative control must invoke configured monitor')
        marker.unlink()
        self.git('config', 'log.showSignature', 'true')
        for mode in ('--json', '--data-v2'):
            self.adapter(['git', 'status'], mode)
            self.adapter(['git', 'log', '1'], mode)
            self.adapter(['git', 'diff'], mode)
            self.assertFalse(marker.exists())
        # An argv-recording plumbing fixture checks the signature flag even
        # without requiring signing keys or a signature-verification program.
        self.fake('git', "import pathlib, sys\npathlib.Path('git-argv').write_text('\\n'.join(sys.argv[1:]))\n")
        self.adapter(['git', 'log', '1'])
        arguments = (self.work / 'git-argv').read_text().splitlines()
        self.assertIn('--no-show-signature', arguments)
        self.assertIn('core.fsmonitor=false', arguments)
        self.assertIn('--no-optional-locks', arguments)

    def test_published_adapter_schemas_reject_false_shapes_and_missing_payload(self):
        self.fake('git', "import os\nos.write(1,b'## main\\x00')\n")
        _, value, _ = self.adapter(['git', 'status'])
        validator = semantic_validator('git-status')
        invalid = copy.deepcopy(value); invalid['git']['dirty'] = 0
        self.assertFalse(validator.is_valid(invalid))
        invalid = copy.deepcopy(value); invalid['git']['ahead'] = -1
        self.assertFalse(validator.is_valid(invalid))
        invalid = copy.deepcopy(value); del invalid['git']
        self.assertFalse(validator.is_valid(invalid))
        invalid = copy.deepcopy(value); invalid['command_status'] = '0'
        self.assertFalse(validator.is_valid(invalid))
        self.fake('make', "print('a.c:1:2: error: failure')\n")
        _, value, _ = self.adapter(['make'])
        invalid = copy.deepcopy(value); invalid['diagnostics'][0]['severity'] = 'success'
        self.assertFalse(semantic_validator('build-result').is_valid(invalid))


if __name__ == '__main__':
    unittest.main()
