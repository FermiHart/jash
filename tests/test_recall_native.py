#!/usr/bin/env python3
"""Recall reconstructs recorded facts without promoting them to execution proof."""
import base64
import json
import os
from pathlib import Path
import unittest

import test_management_builtins as management
import test_history_native as history
from test_discovery_builtins import native_stream, project
from jsonschema import Draft202012Validator


class RecallNative(unittest.TestCase):
    run_cli = management.ManagementBuiltins.run_cli
    execute = management.ManagementBuiltins.execute
    chain = history.HistoryNative.chain

    def setUp(self):
        management.ManagementBuiltins.setUp(self)
        (self.directory / '.jash').mkdir(mode=0o700)
        self.validator = Draft202012Validator(json.loads((management.ROOT / 'schemas/builtin-recall-v1.schema.json').read_text()))

    def report(self, *args, status=0):
        value = json.loads(self.execute(['recall', '--json', *args], status=status).stdout)
        self.validator.validate(value)
        return value

    def native(self, *args, status=0):
        rows, terminal = native_stream(self.execute(['recall', '--data-v2', *args], status=status).stdout)
        self.assertEqual(len(rows), 1)
        value = project(rows[0]); self.validator.validate(value)
        self.assertEqual(terminal['status'], status)
        return value, terminal, rows[0]

    def capture(self, name, content):
        directory = self.directory / '.jash/overflow'
        directory.mkdir(mode=0o700, exist_ok=True)
        file = directory / name; file.write_bytes(content)
        return file

    def decoded(self, value):
        return base64.b64decode(value['data'])

    def test_commands_preserve_bytes_sequence_and_filter_pages(self):
        huge = b'build target with spaces ' + b'x' * 3000 + b'\xff'
        self.chain([(9007199254740993, 100, 0, huge, 'build'),
                    (9007199254740994, 101, 4, b'build target with spaces fail', 'err:4'),
                    (9007199254740995, 102, 0, b'unrelated', '-')])
        first = self.report('cmds', 'build target with spaces', '--limit', '1')
        self.assertEqual(first['matched'], 2)
        self.assertEqual(first['results'][0]['sequence'], 9007199254740994)
        self.assertEqual(first['next_offset'], 1)
        second, terminal, raw = self.native('did', 'build target with spaces', '--limit', '1', '--offset', '1')
        entry = second['results'][0]
        self.assertEqual(self.decoded(entry['command_recorded']), huge)
        self.assertEqual(entry['sequence'], 9007199254740993)
        self.assertFalse(second['source']['cryptographically_verified'])
        self.assertEqual(terminal['state'], 0)
        self.assertEqual(raw[1]['results'][1][0][1]['sequence'][0], 3)
        failed = self.report('failed', '--session', 'synthetic', '--repo=fixture', '--since=101', '--until', '101')
        self.assertEqual([row['status'] for row in failed['results']], [4])

    def test_word_filter_and_literal_option_pattern(self):
        self.chain([(1, 100, 0, b'foobar', '-'), (2, 101, 0, b'foo x', '-'), (3, 102, 0, b'echo --json', '-')])
        self.assertEqual(self.report('history', 'foo')['matched'], 2)
        self.assertEqual(self.report('history', 'foo', '--word')['matched'], 1)
        self.assertEqual(self.report('cmds', '--', '--json')['results'][0]['command_recorded'], 'echo --json')

    def test_stats_keep_long_words_and_exact_counts(self):
        one, two = b'x' * 80 + b'a', b'x' * 80 + b'b'
        self.chain([(1, 100, 0, one + b' arg', '-'), (2, 101, 5, one + b' other', '-'), (3, 102, 0, two, '-')])
        value, _, _ = self.native('stats')
        self.assertEqual(value['analysis']['commands'], 3)
        self.assertEqual(value['analysis']['failed'], 1)
        self.assertEqual(value['results'], [{'first_word_recorded': one.decode(), 'runs': 2, 'failed': 1}, {'first_word_recorded': two.decode(), 'runs': 1, 'failed': 0}])

    def test_adoption_labels_heuristic_and_counts_actual_records(self):
        self.chain([(1, 100, 0, b"setopt x && eval 'checkpoint begin'", '-'),
                    (2, 101, 0, b'echo checkpoint', '-'), (3, 102, 0, b'recall cmds; checkpoint later', '-')])
        value = self.report('adoption')
        self.assertEqual(value['analysis']['commands'], 3)
        self.assertEqual(value['analysis']['differentiator_uses'], 2)
        self.assertEqual(value['analysis']['adoption_percent_rounded'], 67)
        self.assertEqual({r['feature']: r['commands'] for r in value['results']}, {'recall': 1, 'checkpoint': 1})
        self.assertEqual(value['analysis']['classifier'], 'first_recognized_differentiator_heuristic')

    def test_why_temporal_comparison_and_missing_baseline(self):
        self.chain([(1, 100, 1, b'build a', '-'), (2, 110, 0, b'build a', '-'), (3, 120, 2, b'build a', '-'), (4, 130, 3, b'build a', '-')])
        (self.directory / '.jash/fsjournal.tsv').write_bytes(b'105\tsynthetic\t+\tbefore.c\n115\tsynthetic\t~\twith spaces.c\n125\tsynthetic\t-\tafter.c\n')
        value = self.report('why', 'build a')
        analysis = value['analysis']
        self.assertEqual(analysis['last_success']['sequence'], 2)
        self.assertEqual(analysis['first_failure_after_last_success']['sequence'], 3)
        self.assertEqual([e['path_recorded'] for e in analysis['file_changes']], ['with spaces.c'])
        self.assertFalse(analysis['causation_established'])
        self.chain([(1, 100, 1, b'build a', '-')])
        self.assertEqual(self.report('why', 'build a')['analysis']['conclusion'], 'no_successful_baseline')
        self.assertEqual(self.report('why', 'absent')['analysis']['conclusion'], 'no_recorded_match')

    def test_overflow_list_selection_and_byte_pages(self):
        first = self.capture('a same', b'a\x00\n\nend\xff')
        second = self.capture('b same', b'other')
        os.utime(first, ns=(100000000001, 100000000001))
        os.utime(second, ns=(100000000001, 100000000001))
        value = self.report('list', '--limit', '1')
        self.assertEqual(value['results'][0]['name'], 'a same')
        self.assertEqual(value['next_offset'], 1)
        page, _, raw = self.native('same', '--limit', '3')
        self.assertEqual(page['source']['path'], str(first))
        self.assertEqual(self.decoded(page['results'][0]['content']), b'a\x00\n')
        self.assertEqual(page['next_offset'], 3)
        self.assertEqual(raw[1]['results'][1][0][1]['content'][0], 8)
        rest = self.report('same', '--offset', '3')
        self.assertEqual(self.decoded(rest['results'][0]['content']), b'\nend\xff')
        self.assertFalse(rest['truncated'])

    def test_overflow_line_selection_keeps_empty_nul_and_final_partial_line(self):
        self.capture('captured', b'first\n\nnu\x00l\nlast\xff')
        head = self.report('head', '3')
        self.assertEqual([self.decoded(r['content']) for r in head['results']], [b'first', b'', b'nu\x00l'])
        tail = self.report('captured', 'tail', '2')
        self.assertEqual([self.decoded(r['content']) for r in tail['results']], [b'nu\x00l', b'last\xff'])
        self.assertEqual([r['terminated'] for r in tail['results']], [True, False])
        selected = self.report('grep', 'l')
        self.assertEqual([r['line_number'] for r in selected['results']], [3, 4])
        self.assertEqual(self.report('tail', '0')['results'], [])

    def test_bad_sources_and_arguments_do_not_become_empty_success(self):
        self.chain([(1, 100, 0, b'valid', '-')])
        with (self.directory / '.jash/chain.log').open('ab') as file: file.write(b'incomplete')
        value, terminal, _ = self.native('stats', status=1)
        self.assertFalse(value['source']['read_complete'])
        self.assertIsNone(value['matched'])
        self.assertEqual(terminal['state'], 2)
        file = self.capture('too-big', b'')
        with file.open('wb') as handle: handle.truncate(4 * 1024 * 1024 + 1)
        self.assertEqual(self.report('too-big', status=1)['error'], 'store_limit')
        for args in [('cmds', '--last', '-1'), ('why',), ('cmds', '--failed', '--ok'), ('head', '900'), ('list', 'operand'), ('--bogus',)]:
            self.assertEqual(self.report(*args, status=2)['error'], 'invalid_arguments')

    def test_absence_and_unsafe_capture_are_distinguished(self):
        empty = self.report('list')
        self.assertEqual(empty['matched'], 0)
        self.assertFalse(empty['source']['present'])
        self.assertEqual(self.report('missing', status=1)['error'], 'capture_not_found')
        directory = self.directory / '.jash/overflow'; directory.mkdir(mode=0o700)
        (directory / 'link').symlink_to(self.directory / 'nonexistent')
        value, terminal, _ = self.native('list', status=1)
        self.assertFalse(value['source']['read_complete'])
        self.assertIsNone(value['matched'])
        self.assertEqual(terminal['state'], 2)

    def test_schema_negative_controls(self):
        self.chain([(1, 100, 0, b'ok', '-')])
        value = self.report('cmds')
        for mutate in [lambda v: v.pop('source'), lambda v: v.update(extra=True), lambda v: v['source'].update(cryptographically_verified=True), lambda v: v['results'][0].update(sequence='1')]:
            broken = json.loads(json.dumps(value)); mutate(broken)
            self.assertTrue(list(self.validator.iter_errors(broken)))

    def test_all_facets_match_native_projection_and_bypass_prose_budget(self):
        self.chain([(1, 100, 1, b'build target', '-'), (2, 101, 0, b'build target', '-')])
        self.capture('capture', b'first\n\nlast\x00')
        for args in [('cmds',), ('stats',), ('adoption',), ('why', 'build'), ('list',), ('capture',), ('head', '2'), ('tail', '2'), ('grep', 'last')]:
            expected = self.report(*args)
            actual, _, _ = self.native(*args)
            self.assertEqual(actual, expected, args)
        self.env['JASH_LLM_BUDGET'] = '1'
        value, terminal, _ = self.native('cmds')
        self.assertEqual(value['matched'], 2)
        self.assertEqual(terminal['state'], 0)
        rows, terminal = native_stream(self.execute(['recall', '--data', 'cmds']).stdout)
        self.assertEqual(project(rows[0]), value)
        self.assertEqual(terminal['state'], 0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
