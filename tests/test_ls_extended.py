"""Advanced ls queries and delta use real observations, strict native frames,
and observable publication boundaries. JSON schemas are independent oracles.
"""
import base64
import copy
import json
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile
import time
import unittest

from semantic_schema_fixture import semantic_validator
from test_structured_pipe import native_rows, native_projection

ROOT = Path(__file__).resolve().parents[1]
JASH = Path(os.environ.get('JASH', ROOT / 'build/jash')).resolve()
MODES = ('--json', '--data', '--data-v2')


def raw(value):
    return value.encode() if isinstance(value, str) else base64.b64decode(value['data'], validate=True)


class LsExtended(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temporary = tempfile.TemporaryDirectory(prefix='jash-ls-contract-tools-')
        cls.addClassCleanup(temporary.cleanup)
        cls.validator = Path(temporary.name) / 'wire-validate'
        cls.fault = Path(temporary.name) / 'fault.so'
        sources = ['tests/typed_wire_validate.c', 'c/core/data_value.c', 'c/core/data_stream.c',
                   'c/core/data_wire.c', 'c/core/data_wire_decode.c', 'c/module/blake3.c']
        subprocess.run(['cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'c'),
                        *(str(ROOT / path) for path in sources), '-o', str(cls.validator)], check=True, timeout=60)
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        str(ROOT / 'tests/ls_delta_faults.c'), '-ldl', '-o', str(cls.fault)], check=True, timeout=60)

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='jash-ls-extended-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.home = self.root / 'home'; self.home.mkdir()
        self.work = self.root / 'work'; self.work.mkdir()
        self.env = {key: value for key, value in os.environ.items() if not key.startswith('JASH_')}
        self.env.update(HOME=str(self.home), JASH_SIGIL='0', JASH_TIMELINE='0', JASH_MEMO='0',
                        JASH_NUDGE='0', JASH_ADOPT='0', JASH_BRIEF='0', JASH_WORLD='0', JASH_PREFETCH='0',
                        JASH_LOOPGUARD='off', JASH_GOLD='1', CLAUDE_CODE_SESSION_ID='ls-extended-test')

    def run_ls(self, *args, mode='--data-v2', stdout=subprocess.PIPE):
        request = json.dumps({'request_version': 2, 'argv': ['ls', mode, *args], 'cwd': str(self.work)})
        return subprocess.run([str(JASH), '--exec-json', request], cwd=self.work, env=self.env,
                              stdout=stdout, stderr=subprocess.PIPE, timeout=20)

    def report(self, *args, mode='--data-v2', status=0, delta=False):
        result = self.run_ls(*args, mode=mode)
        self.assertEqual(result.returncode, status, (result.stdout[:300], result.stderr))
        typed = terminal = None
        if mode == '--json':
            rows = [json.loads(line) for line in result.stdout.splitlines()]
        else:
            validated = subprocess.run([str(self.validator)], input=result.stdout, capture_output=True, timeout=10)
            self.assertEqual(validated.returncode, 0, (result.stdout[:100], validated.stderr))
            info = json.loads(validated.stdout)
            self.assertEqual(info['logical_verified'], mode == '--data-v2')
            typed, terminal = native_rows(result.stdout)
            rows = [native_projection((12, record)) for record, _ in typed]
            self.assertEqual(terminal['producer_status'], status)
            self.assertEqual(terminal['items'], len(rows))
        schema = semantic_validator('ls-delta-row' if delta else 'ls-extended-row')
        for row in rows: schema.validate(row)
        self.assertEqual(rows[0]['row_kind'], 'header'); self.assertEqual(rows[-1]['row_kind'], 'summary')
        return rows, typed, terminal, result

    @staticmethod
    def entries(rows):
        return [row for row in rows if row['row_kind'] == 'entry']

    def test_basic_formats_keep_schema_and_never_receive_prose_budget_markers(self):
        (self.work / 'file').write_bytes(b'abc')
        self.env['JASH_LLM_BUDGET'] = '1'
        failed = self.run_ls(mode='--json')
        self.assertEqual(failed.returncode, 75, failed.stdout)
        error = json.loads(failed.stdout)
        self.assertEqual(error['schema'], 'jash.builtin.error.v1')
        self.assertEqual(error['error']['code'], 'output_budget_exceeded')
        self.assertFalse(error['complete'])
        for mode in ('--data', '--data-v2'):
            result = self.run_ls(mode=mode)
            self.assertEqual(result.returncode, 0, result.stderr)
            checked = subprocess.run([str(self.validator)], input=result.stdout, capture_output=True, timeout=10)
            self.assertEqual(checked.returncode, 0, (result.stdout, checked.stderr))
            self.assertEqual(json.loads(checked.stdout)['logical_verified'], mode == '--data-v2')
            rows, terminal = native_rows(result.stdout)
            self.assertEqual((terminal['state'], terminal['errors'], terminal['items']), (0, 0, 1))
            row = rows[0][0]
            self.assertEqual(len(row), 13)
            self.assertEqual(row['name'], (8, b'file'))
            self.assertEqual(row['path'], (9, b'./file'))
            self.assertEqual(row['size'], (3, 3))  # Basic ABI is U64, not advanced FILESIZE.
            self.assertEqual(row['type'], (10, b'file'))
            self.assertEqual(terminal['bytes'], len(b'file') + len(b'./file'))
        self.env['JASH_LLM_BUDGET'] = '0'
        result = self.run_ls(mode='--json')
        self.assertEqual(result.returncode, 0, result.stderr)
        row = json.loads(result.stdout)
        schema = semantic_validator('ls-row'); schema.validate(row)
        self.assertEqual(set(row), {'name', 'path', 'type', 'size', 'mode', 'mtime'})
        self.assertEqual((row['path'], row['size']), ('file', 3))
        wrong = copy.deepcopy(row); wrong['size'] = '3'
        self.assertTrue(list(schema.iter_errors(wrong)))
        wrong = copy.deepcopy(row); wrong['invented'] = True
        self.assertTrue(list(schema.iter_errors(wrong)))

    def test_basic_item_error_is_observable_and_nonzero_in_every_format(self):
        (self.work / 'vanish').write_bytes(b'still exists')
        self.env.update(LD_PRELOAD=str(self.fault), JASH_TEST_LS_DELTA_FAULT='metadata')
        for mode in MODES:
            result = self.run_ls(mode=mode)
            self.assertEqual(result.returncode, 1, (mode, result.stdout, result.stderr))
            if mode == '--json':
                row = json.loads(result.stdout)
                semantic_validator('ls-row').validate(row)
                self.assertEqual(row['error'], {'operation': 'metadata', 'errno': 2})
                self.assertEqual(row['type'], 'missing')
            else:
                checked = subprocess.run([str(self.validator)], input=result.stdout, capture_output=True, timeout=10)
                self.assertEqual(checked.returncode, 0, checked.stderr)
                rows, terminal = native_rows(result.stdout)
                self.assertEqual(rows[0][0]['error_op'], (10, b'metadata'))
                self.assertEqual(rows[0][0]['error_code'], (3, 2))
                self.assertEqual(rows[0][0]['size'], (0, None))
                # Enumeration completes; item error accounting prevents a
                # complete-but-error-free interpretation, and CLI remains1.
                self.assertEqual((terminal['state'], terminal['errors']), (0, 1))
        self.assertEqual((self.work / 'vanish').read_bytes(), b'still exists')

    def test_basic_empty_output_noops_and_failed_sink_preserve_contract(self):
        for mode in MODES:
            empty = self.run_ls(mode=mode)
            self.assertEqual(empty.returncode, 0, empty.stderr)
            if mode == '--json':
                self.assertEqual(empty.stdout, b'')
            else:
                rows, terminal = native_rows(empty.stdout)
                self.assertEqual(rows, [])
                self.assertEqual((terminal['state'], terminal['errors']), (0, 0))
        (self.work / 'one').touch(); (self.work / '.hidden').touch()
        noops = ['-1hCGFpAfknoqsux', '--color', '--color=always', '--columns']
        for mode in MODES:
            expected = self.run_ls('-a', mode=mode)
            same = self.run_ls('-a', *noops, mode=mode)
            self.assertEqual(same.returncode, 0, same.stderr)
            self.assertEqual(same.stdout, expected.stdout)
            with open('/dev/full', 'wb') as full:
                failure = self.run_ls(mode=mode, stdout=full)
            self.assertEqual(failure.returncode, 125, (mode, failure.stderr))

    def test_long_fields_keep_exact_native_units_and_symbolic_links(self):
        (self.work / 'file').write_bytes(b'abc')
        (self.work / 'link').symlink_to('file')
        stamp = 1_700_000_000_123_456_789
        os.utime(self.work / 'file', ns=(stamp, stamp))
        for mode in MODES:
            rows, typed, terminal, _ = self.report('-l', mode=mode)
            entries = {row['name']: row for row in self.entries(rows)}
            self.assertEqual(entries['file']['size'], 3)
            self.assertEqual(entries['file']['modified']['seconds'], 1_700_000_000)
            self.assertEqual(entries['file']['modified']['nanoseconds'], 123_456_789)
            self.assertEqual(entries['file']['modified']['native'], stamp)
            self.assertEqual(entries['link']['type'], 'symlink')
            self.assertEqual(raw(entries['link']['symlink_target']), b'file')
            self.assertEqual(entries['file']['uid'], os.getuid())
            self.assertIsInstance(entries['file']['owner']['found'], bool)
            if typed:
                data = next(row for row, _ in typed if row.get('name') == (7, b'file'))
                self.assertEqual(data['size'], (4, 3))
                self.assertEqual(data['modified'][1]['native'], (5, stamp))
                self.assertEqual(data['path'][0], 9)
                self.assertEqual(terminal['state'], 0)

    def test_recursive_reverse_sort_and_directory_operands(self):
        (self.work / 'tree/z').mkdir(parents=True); (self.work / 'tree/a').mkdir()
        (self.work / 'tree/z/leaf').touch(); (self.work / 'tree/a/other').touch()
        (self.work / 'tree/link').symlink_to('z', target_is_directory=True)
        for mode in MODES:
            rows, _, _, _ = self.report('-Rr', 'tree', mode=mode)
            entries = self.entries(rows)
            self.assertEqual([row['path'] for row in entries], ['tree/z', 'tree/link', 'tree/a', 'tree/z/leaf', 'tree/a/other'])
            self.assertEqual([row['depth'] for row in entries], [0, 0, 0, 1, 1])
            self.assertFalse(any('link/' in row['path'] for row in entries))
            rows, _, _, _ = self.report('-ld', 'tree', mode=mode)
            self.assertEqual([row['path'] for row in self.entries(rows)], ['tree'])
            self.assertEqual(self.entries(rows)[0]['type'], 'dir')

    def test_size_and_nanosecond_time_sort_and_reset(self):
        for name, size, ns in [('alpha', 5, 101), ('beta', 1, 103), ('gamma', 9, 102)]:
            path = self.work / name; path.write_bytes(b'x' * size)
            os.utime(path, ns=(1_700_000_000_000_000_000 + ns,) * 2)
        for flags, expected in [('-S', ['gamma', 'alpha', 'beta']), ('-Sr', ['beta', 'alpha', 'gamma']),
                                ('-t', ['beta', 'gamma', 'alpha']), ('-tr', ['alpha', 'gamma', 'beta'])]:
            rows, _, _, _ = self.report(flags)
            self.assertEqual([row['name'] for row in self.entries(rows)], expected)
        result = subprocess.run([str(JASH), '-c', 'ls --json -t; ls --json -l'], cwd=self.work,
                                env=self.env, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        headers = [index for index, row in enumerate(rows) if row['row_kind'] == 'header']
        self.assertEqual([row['name'] for row in self.entries(rows[headers[1]:])], ['alpha', 'beta', 'gamma'])

    def test_name_bytes_controls_exact_big_size_and_hidden_selection(self):
        names = [b'a\tcontrol\n', b'raw\xff', b'.hidden']
        for name in names:
            descriptor = os.open(os.fsencode(self.work) + b'/' + name, os.O_CREAT | os.O_WRONLY, 0o600)
            os.close(descriptor)
        sparse = self.work / 'huge'; sparse.touch()
        try: sparse_size = 9_007_199_254_740_993; os.truncate(sparse, sparse_size)
        except OSError: sparse_size = 2**40 + 1; os.truncate(sparse, sparse_size)
        for mode in MODES:
            rows, typed, _, _ = self.report('-la', mode=mode)
            entries = self.entries(rows)
            self.assertEqual({raw(row['name']) for row in entries}, set(names) | {b'huge'})
            self.assertEqual(next(row for row in entries if row['name'] == 'huge')['size'], sparse_size)
            if typed:
                byte_row = next(row for row, _ in typed if row.get('name') == (8, b'raw\xff'))
                self.assertEqual(byte_row['path'][0], 9)
        rows, _, _, _ = self.report('-l')
        self.assertNotIn('.hidden', [row['name'] for row in self.entries(rows)])

    def test_recent_large_and_pretty_are_explicit_selection(self):
        for index in range(12):
            path = self.work / f'{index:02d}.c'; path.write_bytes(b'x' * index)
            os.utime(path, (time.time() - 200000 if index == 11 else time.time(),) * 2)
        rows, _, _, _ = self.report('--recent', '--large')
        self.assertEqual([row['name'] for row in self.entries(rows)], [f'{i:02d}.c' for i in range(1, 11)])
        self.assertEqual(rows[-1]['eligible'], 11); self.assertEqual(rows[-1]['selection_omitted'], 1)
        (self.work / 'dir').mkdir(); (self.work / 'README.md').touch(); (self.work / 'config.toml').touch()
        rows, _, _, _ = self.report('--pretty-limit', '4')
        self.assertEqual({row['category'] for row in self.entries(rows)}, {'DIR', 'CODE', 'DOC', 'CONFIG'})
        self.assertEqual(rows[-1]['selected'], 4); self.assertEqual(rows[-1]['selection_omitted'], 11)
        self.assertTrue(rows[-1]['source_complete']); self.assertFalse(rows[-1]['truncated'])

    def test_description_semantic_and_relation_publish_real_heuristics(self):
        (self.work / 'lib').mkdir()
        for name in ('base.c', 'base.h', 'other.py', 'database.txt'): (self.work / 'lib' / name).touch()
        rows, _, _, _ = self.report('--describe', 'lib')
        entries = {row['name']: row for row in self.entries(rows)}
        self.assertEqual(entries['base.c']['description'], 'C source code')
        self.assertFalse(rows[0]['provenance']['content_analysis']); self.assertFalse(rows[0]['provenance']['external_model'])
        rows, _, _, _ = self.report('--semantic', 'PYTHON', 'lib')
        self.assertEqual([row['name'] for row in self.entries(rows)], ['other.py'])
        rows, _, _, _ = self.report('--relate', 'lib/base.c')
        self.assertEqual([row['name'] for row in self.entries(rows)], ['base.c', 'base.h', 'database.txt'])
        self.assertEqual(rows[0]['options']['relate'], 'lib/base.c')
        self.assertEqual(rows[0]['roots'], ['lib'])
        self.assertTrue(all(row['root'] == 'lib' for row in self.entries(rows)))
        rows, _, _, _ = self.report('--inspect', 'lib/base.c')
        self.assertIsNotNone(self.entries(rows)[0]['owner'])

    def test_preview_is_bounded_bytes_and_never_opens_fifo_or_symlink(self):
        (self.work / 'sample').write_bytes(b'one\x00\xff\ntwo\nthree\nfour\n')
        (self.work / 'long').write_bytes(b'x' * 6000)
        os.mkfifo(self.work / 'fifo'); (self.work / 'link').symlink_to('sample')
        for mode in MODES:
            rows, _, _, _ = self.report('--preview', mode=mode)
            entries = {row['name']: row for row in self.entries(rows)}
            self.assertEqual(raw(entries['sample']['preview']['bytes']), b'one\x00\xff\ntwo\nthree\n')
            self.assertEqual(entries['sample']['preview']['lines'], 3)
            self.assertTrue(entries['sample']['preview']['truncated'])
            self.assertEqual(entries['long']['preview']['byte_count'], 4096)
            self.assertIsNone(entries['fifo']['preview']); self.assertIsNone(entries['link']['preview'])

    def test_json_type_filter_and_multiple_operands_use_observed_groups(self):
        for root in ('first space', 'second\ttab'):
            (self.work / root).mkdir(); (self.work / root / 'file').touch(); (self.work / root / 'dir').mkdir()
        for mode in MODES:
            rows, _, _, _ = self.report('-l', '--where-type=file', 'first space', 'second\ttab', mode=mode)
            entries = self.entries(rows)
            self.assertEqual([row['operand_index'] for row in entries], [0, 1])
            self.assertEqual([row['path'] for row in entries], ['first space/file', 'second\ttab/file'])

    def test_source_errors_do_not_masquerade_as_empty_complete_listing(self):
        (self.work / 'exists').touch()
        for mode in MODES:
            rows, _, terminal, _ = self.report('-l', 'exists', 'missing', mode=mode, status=1)
            self.assertFalse(rows[-1]['source_complete']); self.assertGreaterEqual(rows[-1]['errors'], 1)
            failure = next(row for row in self.entries(rows) if row['path'] == 'missing')
            self.assertIsNotNone(failure['error']); self.assertIsNone(failure['size'])
            if terminal: self.assertEqual(terminal['state'], 2)

    def test_recursion_limit_is_incomplete_and_native_ignores_prose_budget(self):
        directory = self.work
        for _ in range(67): directory /= 'd'; directory.mkdir()
        self.env['JASH_LLM_BUDGET'] = '1'
        for mode in ('--data', '--data-v2'):
            rows, _, terminal, _ = self.report('-R', mode=mode, status=1)
            self.assertTrue(rows[-1]['truncated']); self.assertEqual(rows[-1]['reason'], 'depth_limit')
            self.assertEqual(terminal['state'], 3); self.assertEqual(terminal['truncated'], 1)
        result = self.run_ls('-l', mode='--json')
        self.assertEqual(result.returncode, 75)
        self.assertEqual(json.loads(result.stdout)['error']['code'], 'output_budget_exceeded')

    def test_monitor_and_tree_are_unavailable_and_options_fail_before_delta(self):
        for mode in MODES:
            for flag in ('--monitor=.', '--tree', '--classify'):
                result = self.run_ls(flag, mode=mode)
                self.assertNotEqual(result.returncode, 0)
            for arguments in [('--delta', '.', '.'), ('--delta', '-R'), ('--delta', '--semantic=x')]:
                result = self.run_ls(*arguments, mode=mode)
                self.assertEqual(result.returncode, 2)
        self.assertFalse((self.home / '.jash/gold-delta').exists())

    def test_delta_baseline_changes_type_and_byte_safe_store(self):
        for mode in MODES:
            self.env['CLAUDE_CODE_SESSION_ID'] = mode
            name = b'control\t\nraw\xff'; path = os.fsencode(self.work) + b'/' + name
            with open(path, 'wb') as stream: stream.write(b'abc')
            rows, _, terminal, _ = self.report('--delta', mode=mode, delta=True)
            changes = [row for row in rows if row['row_kind'] == 'change']
            self.assertEqual([raw(row['name']) for row in changes], [name])
            self.assertEqual(changes[0]['change'], 'baseline')
            self.assertTrue(rows[-1]['publication']['applied']); self.assertTrue(rows[-1]['publication']['durable'])
            snapshot = Path(rows[-1]['publication']['path'])
            self.assertTrue(snapshot.read_bytes().startswith(b'#JGOLD2\0'))
            self.assertIn(name, snapshot.read_bytes())
            rows, _, _, _ = self.report('--delta', mode=mode, delta=True)
            self.assertEqual(rows[-1]['summary']['changes'], 0)
            with open(path, 'ab') as stream: stream.write(b'more')
            rows, _, _, _ = self.report('--delta', mode=mode, delta=True)
            change = next(row for row in rows if row['row_kind'] == 'change')
            self.assertEqual(change['change'], 'modified'); self.assertEqual(change['size_delta'], 4)
            stamp = os.lstat(path).st_mtime_ns
            os.unlink(path); os.symlink(b'abcdefg', path); os.utime(path, ns=(stamp, stamp), follow_symlinks=False)
            rows, _, _, _ = self.report('--delta', mode=mode, delta=True)
            change = next(row for row in rows if row['row_kind'] == 'change')
            self.assertEqual(change['change'], 'modified'); self.assertEqual(change['type'], 'l')
            self.assertEqual(change['size_delta'], 0)
            os.unlink(path)
            rows, _, _, _ = self.report('--delta', mode=mode, delta=True)
            self.assertEqual(next(row for row in rows if row['row_kind'] == 'change')['change'], 'removed')
            if terminal: self.assertEqual(terminal['state'], 0)

    def test_delta_hidden_filter_has_its_own_observation_scope(self):
        (self.work / 'visible').touch(); (self.work / '.hidden').touch()
        normal, _, _, _ = self.report('--delta', delta=True)
        hidden, _, _, _ = self.report('--delta', '-a', delta=True)
        self.assertFalse(normal[0]['include_hidden']); self.assertTrue(hidden[0]['include_hidden'])
        self.assertNotEqual(normal[0]['key'], hidden[0]['key'])
        self.assertEqual({row['name'] for row in hidden if row['row_kind'] == 'change'}, {'visible', '.hidden'})
        again, _, _, _ = self.report('--delta', delta=True)
        self.assertEqual(again[-1]['summary']['changes'], 0)

    def test_delta_reads_valid_legacy_and_rejects_malformed_snapshot(self):
        (self.work / 'file').write_bytes(b'abc')
        rows, _, _, _ = self.report('--delta', delta=True)
        snapshot = Path(rows[-1]['publication']['path']); stat = (self.work / 'file').stat()
        snapshot.write_text(f'#JGOLD1 12\nfile\t{stat.st_mtime_ns}\t3\tf\n')
        rows, _, _, _ = self.report('--delta', delta=True)
        self.assertEqual(rows[-1]['summary']['changes'], 0)
        self.assertTrue(snapshot.read_bytes().startswith(b'#JGOLD2\0'))
        invalid_type = b'#JGOLD1 12\nfile\t3\t3\tq\n'
        snapshot.write_bytes(invalid_type)
        invalid, _, _, _ = self.report('--delta', status=1, delta=True)
        self.assertFalse(invalid[0]['source']['read_complete']); self.assertEqual(snapshot.read_bytes(), invalid_type)
        broken = b'#JGOLD1 12\nfile\t3\t3\tf\nignored-malformed\n'
        snapshot.write_bytes(broken)
        for mode in MODES:
            rows, _, terminal, _ = self.report('--delta', mode=mode, status=1, delta=True)
            self.assertFalse(rows[0]['source']['read_complete']); self.assertFalse(rows[0]['baseline'])
            self.assertFalse(rows[-1]['publication']['applied']); self.assertEqual(snapshot.read_bytes(), broken)
            if terminal: self.assertEqual(terminal['state'], 2)

    def test_delta_snapshot_fifo_symlink_and_item_limit_fail_without_publish(self):
        (self.work / 'file').touch()
        rows, _, _, _ = self.report('--delta', delta=True)
        snapshot = Path(rows[-1]['publication']['path']); snapshot.unlink(); os.mkfifo(snapshot)
        self.report('--delta', status=1, delta=True)
        snapshot.unlink(); snapshot.symlink_to('missing')
        self.report('--delta', status=1, delta=True)
        self.assertTrue(snapshot.is_symlink())
        snapshot.unlink()
        for index in range(4096): (self.work / f'x{index:04}').touch()
        rows, _, terminal, _ = self.report('--delta', status=1, delta=True)
        self.assertFalse(snapshot.exists()); self.assertEqual(rows[-1]['summary']['reason'], 'delta_item_limit')
        self.assertEqual(terminal['state'], 3)

    def test_delta_json_budget_and_broken_sink_precede_store_creation(self):
        (self.work / 'file').touch(); self.env['JASH_LLM_BUDGET'] = '1'
        result = self.run_ls('--delta', mode='--json')
        self.assertEqual(result.returncode, 75)
        self.assertFalse((self.home / '.jash/gold-delta').exists())
        for mode in ('--data', '--data-v2'):
            with open('/dev/full', 'wb') as full: result = self.run_ls('--delta', mode=mode, stdout=full)
            self.assertEqual(result.returncode, 125); self.assertFalse((self.home / '.jash/gold-delta').exists())
        rows, _, _, _ = self.report('--delta', delta=True)
        self.assertTrue(rows[-1]['publication']['applied'])

    def test_delta_rename_and_directory_sync_failures_preserve_effect_facts(self):
        (self.work / 'file').touch()
        self.env.update(LD_PRELOAD=str(self.fault), JASH_TEST_LS_DELTA_FAULT='rename')
        for mode in MODES:
            rows, _, terminal, _ = self.report('--delta', mode=mode, status=1, delta=True)
            receipt = rows[-1]['publication']
            self.assertFalse(receipt['applied']); self.assertFalse(receipt['durable'])
            self.assertEqual(receipt['phase'], 'publish'); self.assertIsNotNone(receipt['errno'])
            self.assertFalse(Path(receipt['path']).exists())
            if terminal: self.assertEqual(terminal['state'], 2)
        self.env['JASH_TEST_LS_DELTA_FAULT'] = 'directory-sync'
        for mode in MODES:
            rows, _, _, _ = self.report('--delta', mode=mode, status=1, delta=True)
            receipt = rows[-1]['publication']
            self.assertTrue(receipt['applied']); self.assertFalse(receipt['durable'])
            self.assertEqual(receipt['phase'], 'directory_sync'); self.assertTrue(Path(receipt['path']).exists())

    def test_real_pipe_close_before_delta_publication_and_failed_receipt_after_effect(self):
        for index in range(500): (self.work / (f'{index:04}-' + 'x' * 200)).touch()
        for mode in MODES:
            process = subprocess.Popen([str(JASH), '-c', shlex.join(['ls', mode, '--delta'])], cwd=self.work,
                                       env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if mode == '--json': self.assertTrue(process.stdout.readline().endswith(b'\n'))
            else:
                self.assertEqual(process.stdout.read(4), b'JTV1' if mode == '--data' else b'JTV2')
                for _ in range(3):
                    frame = process.stdout.read(9); self.assertEqual(len(frame), 9)
                    count = struct.unpack('>Q', frame[1:])[0]
                    self.assertEqual(len(process.stdout.read(count)), count)
            process.stdout.close(); stderr = process.stderr.read(); process.stderr.close()
            self.assertEqual(process.wait(timeout=10), 125, stderr)
            self.assertFalse((self.home / '.jash/gold-delta').exists())
        self.env.update(LD_PRELOAD=str(self.fault), JASH_TEST_LS_DELTA_FAULT='sink-after-publish')
        result = self.run_ls('--delta')
        self.assertEqual(result.returncode, 125)
        self.assertEqual(len(list((self.home / '.jash/gold-delta/ls').glob('*.snap'))), 1)
        invalid = subprocess.run([str(self.validator)], input=result.stdout, capture_output=True, timeout=10)
        self.assertNotEqual(invalid.returncode, 0)

    def test_schema_negative_controls_and_wire_corruption_are_rejected(self):
        (self.work / 'file').write_bytes(b'abc')
        rows, _, _, result = self.report('-l')
        schema = semantic_validator('ls-extended-row')
        for key, value in [('size', '3'), ('uid', '0'), ('unknown', True), ('symlink_target_truncated', 0)]:
            broken = copy.deepcopy(self.entries(rows)[0]); broken[key] = value
            self.assertTrue(list(schema.iter_errors(broken)))
        for payload in (result.stdout[:-1], result.stdout + b'junk', result.stdout.replace(b'file', b'fIle', 1)):
            checked = subprocess.run([str(self.validator)], input=payload, capture_output=True, timeout=10)
            self.assertNotEqual(checked.returncode, 0)
        rows, _, _, _ = self.report('--delta', delta=True)
        schema = semantic_validator('ls-delta-row')
        broken = copy.deepcopy(rows[-1]); broken['publication']['applied'] = 'true'
        self.assertTrue(list(schema.iter_errors(broken)))


if __name__ == '__main__': unittest.main(verbosity=2)
