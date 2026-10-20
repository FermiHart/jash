#!/usr/bin/env python3
"""Context facts versus actual source, Git and recorded session metadata."""
import base64
import json
import os
from pathlib import Path
import subprocess
import unittest
from jsonschema import ValidationError
import native_data_fixture as f
from contract_schema_fixture import projection, validate_contract


class ContextNative(f.NativeQueryCase):
    def report(self, *arguments, mode='--json', status=0, strict=False):
        caps = ['effect:exec'] if not arguments or arguments[0] == 'bundle' else ['effect:read']
        result = self.run_command(['context', mode, *arguments], strict=strict, caps=caps)
        self.assertEqual(result.returncode, status, result.stderr)
        if mode == '--json': value = json.loads(result.stdout)
        else:
            stream = f.stream(result.stdout, credit_kind='none')
            self.assertEqual((stream.producer, stream.state, stream.errors),
                             (status, 0 if status == 0 else 2, 0 if status == 0 else 1))
            self.assertEqual(len(stream.items), 1)
            value = projection(stream.items[0])
        validate_contract('context', value)
        return value

    def git(self, *arguments):
        result = subprocess.run(['git', *arguments], cwd=self.work, env=self.env,
                                capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result

    def git_repo(self):
        self.git('init', '-q')
        self.git('config', 'user.name', 'Context Fixture')
        self.git('config', 'user.email', 'context@example.invalid')
        (self.work / 'tracked.c').write_text('int original(void) { return 1; }\n')
        self.git('add', '--', 'tracked.c')
        self.git('commit', '-qm', 'fixture')

    def private(self):
        directory = Path(self.env['HOME']) / '.jash'
        directory.mkdir(mode=0o700, exist_ok=True)
        return directory

    def test_map_direct_declarations_counts_and_both_profiles(self):
        text = '#include <stdio.h>\nint function(void) {\n  return 1;\n}\nstruct point {\n  int x;\n};\n'
        (self.work / 'a.c').write_text(text)
        (self.work / 'notes.txt').write_text('not a recognized source')
        (self.work / 'build').mkdir()
        (self.work / 'build' / 'excluded.c').write_text('int excluded(void);\n')
        for mode, strict in [('--json', False), ('--data', False), ('--data-v2', True)]:
            result = self.report('map', mode=mode, strict=strict)['result']
            self.assertEqual(result['source_files'], 1)
            row = result['files'][0]
            self.assertEqual((row['path'], row['bytes'], row['lines']), ('a.c', len(text), 7))
            self.assertEqual(row['signatures'], [{'line': 2, 'text': 'int function(void)'},
                                                 {'line': 5, 'text': 'struct point'}])
            self.assertEqual((row['signatures_total'], row['signatures_omitted']), (2, 0))
            self.assertEqual(result['snapshot'], 'individually-stable-files-not-an-atomic-directory-snapshot')

    def test_paths_and_long_declarations_keep_exact_bytes(self):
        path = b'quote " $ tab\t line\n raw\xff.c'
        signature = b'int ' + b'x' * 4100 + b'(void);'
        fd = os.open(os.fsencode(self.work) + b'/' + path, os.O_WRONLY | os.O_CREAT, 0o600)
        os.write(fd, signature + b'\n'); os.close(fd)
        for mode in ('--json', '--data-v2'):
            row = self.report('map', mode=mode)['result']['files'][0]
            self.assertEqual(base64.b64decode(row['path']['data']), path)
            self.assertEqual(row['signatures'][0]['text'], signature.decode())
        folded = self.report('fold', path, mode='--data')['result']
        self.assertEqual(base64.b64decode(folded['path']['data']), path)
        self.assertEqual(folded['source_lines'], 1)

    def test_map_projection_limits_are_counted_not_silent(self):
        (self.work / 'many.c').write_text(''.join(f'int f{i}(void);\n' for i in range(65)))
        row = self.report('map', mode='--data')['result']['files'][0]
        self.assertEqual((len(row['signatures']), row['signatures_total'], row['signatures_omitted']), (64, 65, 1))

    def test_fold_uses_shared_brace_heuristic_and_real_line_positions(self):
        text = 'int f(void) {\n  const char *s = "}";\n  return 1;\n}\nint g(void);\n'
        (self.work / 'a.c').write_text(text)
        value = self.report('fold', 'a.c', mode='--data', strict=True)['result']
        self.assertEqual((value['source_lines'], value['bodies_collapsed'], value['lines_suppressed']), (5, 1, 3))
        self.assertEqual(value['lines'], [{'line': 1, 'text': 'int f(void) {\n', 'body_collapsed_after': True},
                                         {'line': 5, 'text': 'int g(void);\n', 'body_collapsed_after': False}])
        self.assertEqual(value['remaining_brace_depth'], 0)
        # An escaped terminal quote/backslash is a bounded byte observation.
        (self.work / 'tail.c').write_bytes(b'const char *s = "tail\\')
        self.assertEqual(self.report('fold', 'tail.c')['result']['lines'][0]['text'], 'const char *s = "tail\\')

    def test_ask_ranking_uses_actual_terms_signatures_and_intent(self):
        (self.work / 'target.c').write_text('int target(void) {\n    return 1;\n}\n')
        (self.work / 'unrelated.c').write_text('int other(void) { return 0; }\n')
        self.env['JASH_INTENT'] = 'finish target'
        result = self.report('ask', 'target', mode='--data')['result']
        self.assertEqual((result['source_files'], result['candidates'], result['deeply_analyzed']), (2, 1, 1))
        row = result['files'][0]
        self.assertEqual((row['path'], row['score'], row['lines']), ('target.c', 26, 3))
        self.assertEqual(row['signatures'], [{'line': 1, 'text': 'int target(void)'}])
        self.assertEqual(row['snippets'], [{'line': 1, 'text': 'int target(void) {\n'}])
        self.assertEqual(result['intent']['origin'], 'environment')
        self.assertFalse(result['has_more'])

    def test_ask_phase_bounds_and_empty_search_are_truthful(self):
        (self.work / 'late.c').write_text('/* unrelated */\n' * 50 + 'int needle(void);\n')
        empty = self.report('ask', 'needle', mode='--data-v2', strict=True)
        self.assertTrue(empty['complete']); self.assertEqual(empty['result']['files'], [])
        for index in range(40): (self.work / f'needle{index:02}.c').write_text('int needle(void);\n')
        result = self.report('ask', 'needle')['result']
        self.assertEqual((result['candidates'], result['deeply_analyzed'], len(result['files'])), (40, 32, 10))
        self.assertTrue(result['has_more'])
        self.report('ask', 'x', status=2)
        self.report('ask', 'x' * 48, status=2, mode='--data')
        self.report('ask', ' '.join(f'term{i}' for i in range(17)), status=2)

    def test_bundle_reads_git_diff_and_exact_last_ten_records(self):
        self.git_repo()
        (self.work / 'tracked.c').write_text('int changed(void) { return 2; }\nint added(void);\n')
        directory = self.private()
        (directory / 'intent').write_text('intent from store\nignored second line\n')
        records = [{'ts': 1700000000 + i, 'exit': i % 3, 'dur_ms': 2**53 + i,
                    'cmd': ('x' * 3500 if i == 11 else '') + f'command {i} "quoted"\t'} for i in range(12)]
        for mode, strict in [('--json', False), ('--data-v2', True)]:
            (directory / 'session_meta.jsonl').write_text(''.join(json.dumps(row) + '\n' for row in records))
            result = self.report('bundle', mode=mode, strict=strict)['result']
            self.assertEqual(result['intent']['text'], 'intent from store')
            self.assertEqual(result['recent']['total_records'], 12)
            self.assertTrue(result['recent']['has_more'])
            self.assertEqual([row['command_recorded'] for row in result['recent']['records']], [row['cmd'] for row in records[-10:]])
            self.assertEqual(result['recent']['records'][-1]['duration_ms'], 2**53 + 11)
            self.assertFalse(result['recent']['cryptographically_verified'])
            self.assertTrue(result['git']['status']['payload']['git']['dirty'])
            self.assertEqual(result['diff_stat']['payload']['total']['insertions'], 2)
            self.assertEqual(result['diff_stat']['payload']['total']['deletions'], 1)
            self.assertEqual(result['diff_stat']['payload']['files'][0]['path'], 'tracked.c')

    def test_bundle_missing_git_is_unavailable_not_clean(self):
        value = self.report('bundle', status=1, mode='--data')
        self.assertFalse(value['complete'])
        result = value['result']
        self.assertFalse(result['git']['status']['available'])
        self.assertNotEqual(result['git']['status']['command_status'], 0)
        self.assertIsNone(result['git']['status']['payload'])
        self.assertIsNone(result['diff_stat'])
        self.assertFalse(result['recent']['present'])

    def test_git_execution_requires_authority_and_actual_sink(self):
        commands = self.root / 'bin'; commands.mkdir()
        marker = self.root / 'started'
        script = commands / 'git'
        script.write_text('#!/bin/sh\nprintf started > "$CONTEXT_MARKER"\nexit 17\n')
        script.chmod(0o700)
        self.env['PATH'] = str(commands) + ':' + self.env['PATH']
        self.env['CONTEXT_MARKER'] = str(marker)
        denied = self.run_command(['context', '--data', 'bundle'], strict=True, caps=['effect:read'])
        self.assertNotEqual(denied.returncode, 0); self.assertFalse(marker.exists())
        sink = subprocess.run([str(f.JASH), '-c', 'context --data bundle > /dev/full'],
                              cwd=self.work, env=self.env, capture_output=True, timeout=10)
        self.assertEqual(sink.returncode, 125, sink.stderr); self.assertFalse(marker.exists())
        self.env['JASH_LLM_BUDGET'] = '1'
        budget = self.run_command(['context', '--json', 'bundle'])
        self.assertEqual(budget.returncode, 75, budget.stderr); self.assertFalse(marker.exists())
        self.env.pop('JASH_LLM_BUDGET')
        observed = self.report('bundle', status=1, strict=True)['result']['git']['status']
        self.assertEqual(observed['command_status'], 17); self.assertTrue(marker.exists())

    def test_corrupt_or_unterminated_metadata_is_an_error(self):
        self.git_repo(); target = self.private() / 'session_meta.jsonl'
        for content in (b'{"ts":1,"ts":2,"exit":0,"dur_ms":1,"cmd":"x"}\n',
                        b'{"ts":1,"exit":0,"dur_ms":1,"cmd":"x"}',
                        b'{"ts":1,"exit":0,"dur_ms":-1,"cmd":"x"}\n', b'bad\n'):
            target.write_bytes(content)
            result = self.report('bundle', mode='--data', status=1)['result']['recent']
            self.assertFalse(result['read_complete']); self.assertIsNone(result['total_records'])
            self.assertEqual(result['records'], [])

    def test_source_limits_symlink_fifo_and_missing_are_not_empty_success(self):
        (self.work / 'real.c').write_text('int real(void);\n')
        (self.work / 'alias.c').symlink_to('real.c')
        self.assertEqual(self.report('map')['result']['source_files'], 1)
        self.report('fold', 'alias.c', mode='--data', status=1)
        os.mkfifo(self.work / 'fifo.c')
        self.report('fold', 'fifo.c', status=1)
        self.report('map', 'missing', mode='--data', status=1)
        self.report('fold', 'missing', status=1)
        (self.work / 'nul.c').write_bytes(b'int x;\0tail')
        self.report('map', mode='--data', status=1)
        (self.work / 'nul.c').unlink()
        (self.work / 'large.c').write_bytes(b' ' * (1024 * 1024 + 1))
        self.report('map', mode='--data', status=75)

    def test_json_budget_is_not_a_data_presentation_cutoff(self):
        (self.work / 'a.c').write_text('int a(void);\n')
        self.env['JASH_LLM_BUDGET'] = '1'
        failed = self.run_command(['context', '--json', 'map'])
        self.assertEqual(failed.returncode, 75, failed.stderr)
        validate_contract('context', json.loads(failed.stdout))
        self.assertEqual(self.report('map', mode='--data')['result']['source_files'], 1)
        self.env.pop('JASH_LLM_BUDGET')
        failed = subprocess.run([str(f.JASH), '-c', 'context --data map > /dev/full'],
                                cwd=self.work, env=self.env, capture_output=True, timeout=10)
        self.assertEqual(failed.returncode, 125, failed.stderr)

    def test_schema_rejects_unknown_and_missing_fields_and_arguments(self):
        value = self.report('help', mode='--data')
        for bad in ({**value, 'invented': 1}, {key: item for key, item in value.items() if key != 'complete'}):
            with self.assertRaises(ValidationError): validate_contract('context', bad)
        for arguments in (('unknown',), ('map', 'a', 'b'), ('fold',), ('bundle', 'unexpected'), ('help', 'x')):
            self.report(*arguments, status=2)


if __name__ == '__main__': unittest.main()
