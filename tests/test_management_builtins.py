#!/usr/bin/env python3
"""Semantic management reports and effects, using only private synthetic fixtures."""
import base64
import json
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile
import unittest

from jsonschema import Draft202012Validator
from test_discovery_builtins import native_stream, project

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get('JASH_BUILD_DIR', ROOT / 'build')).resolve()
JASH = Path(os.environ.get('JASH', BUILD / 'jash')).resolve()
SCHEMAS = {'unalias': 'alias', 'jash_state': 'persist'}


class ManagementBuiltins(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix='jash-management-')
        self.addCleanup(tmp.cleanup)
        self.directory = Path(tmp.name)
        self.env = dict(HOME=str(self.directory), PATH=os.defpath, LC_ALL='C.UTF-8',
                        JASH_SIGIL='0', JASH_NUDGE='0', JASH_ADOPT='0', JASH_MEMO='0',
                        JASH_BRIEF='0', JASH_LOOPGUARD='off')

    def run_cli(self, *args, status=0, input=b'', stdout=subprocess.PIPE):
        result = subprocess.run([str(JASH), *args], cwd=self.directory, env=self.env,
                                input=input, stdout=stdout, stderr=subprocess.PIPE, timeout=25)
        self.assertEqual(result.returncode, status, (args, result.returncode, result.stdout, result.stderr))
        return result

    def execute(self, argv, status=0, stdout=subprocess.PIPE):
        encoded = [{'encoding': 'base64', 'data': base64.b64encode(value).decode()}
                   if isinstance(value, bytes) else value for value in argv]
        return self.run_cli('--exec-json', json.dumps({'protocol_version': 2, 'argv': encoded}), status=status, stdout=stdout)

    def validate(self, document):
        name = SCHEMAS.get(document['command'], document['command'])
        schema = json.loads((ROOT / 'schemas' / ('builtin-' + name + '-v1.schema.json')).read_text())
        Draft202012Validator.check_schema(schema)
        Draft202012Validator(schema).validate(document)
        return document

    def report(self, name, *args, status=0):
        return self.validate(json.loads(self.execute([name, '--json', *args], status=status).stdout))

    def script_reports(self, script, status=0):
        result = self.run_cli('-c', script, status=status)
        return [self.validate(json.loads(line)) for line in result.stdout.splitlines()], result

    def compile_library(self, name, source):
        code = self.directory / (name + '.c')
        library = self.directory / (name + '.so')
        code.write_text(source)
        command = [os.environ.get('CC', 'cc'), '-shared', '-fPIC', '-O0', '-I' + str(ROOT / 'c'),
                   '-I' + str(ROOT / 'c/sys'), '-I' + str(ROOT / 'c/osif'), str(code), '-o', str(library)]
        result = subprocess.run(command, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=25)
        self.assertEqual(result.returncode, 0, result.stderr)
        library.chmod(0o600)
        return library

    def plugin_fixture(self):
        return self.compile_library('synthetic plugin', '''#include <plugin.h>
#include <stdio.h>
static jash_plugin_handle_t value = {{"audit_fixture", "1.2", "synthetic plugin"}, 0, 0};
jash_plugin_handle_t* jash_plugin_init(void) { puts("fixture-plugin-init"); return &value; }
void jash_plugin_shutdown(jash_plugin_handle_t* p) { (void)p; puts("fixture-plugin-stop"); }
''')

    def module_fixture(self):
        library = self.compile_library('synthetic module', '''#include <module/modjash.h>
#include <stdio.h>
static int init(void) { puts("fixture-module-init"); return 0; }
static void stop(void) { puts("fixture-module-stop"); }
static int dispatch(const jmod_call_t* p) { (void)p; return 0; }
jmod_ops_t jmod_ops = {.init=init, .shutdown=stop, .dispatch=dispatch,
.name="audit_module", .version="2.3", .description="synthetic module", .caps_used=JMOD_CAP_FS_READ};
''')
        body = library.read_bytes()
        manifest = b'name=audit_module\nversion=2.3\ncaps=0x1\ndescription=synthetic module\n'
        header = struct.pack('<8sII32sIIII', b'JMODv01\0', 1, 0, b'\0'*32, len(manifest), len(body), 0, 0)
        module = self.directory / 'synthetic module.modjash'
        module.write_bytes(header + manifest + body)
        module.chmod(0o600)
        return module

    def test_alias_exact_values_and_actual_unset(self):
        value = 'echo synthetic "quotes"; tabs\t and line\nnext'
        assignment = shlex.quote('audit_alias=' + value)
        reports, _ = self.script_reports(f'alias --json {assignment}; alias --json audit_alias; unalias --json audit_alias; alias --json audit_alias', status=1)
        self.assertEqual(reports[0]['items'][0]['value'], value)
        self.assertTrue(reports[0]['items'][0]['changed'])
        self.assertEqual(reports[1]['items'][0]['value'], value)
        self.assertFalse(reports[1]['items'][0]['changed'])
        self.assertEqual(reports[2]['items'][0]['before'], value)
        self.assertFalse(reports[2]['items'][0]['present'])
        self.assertEqual(reports[3]['items'][0]['error'], 'not_found')

    def test_alias_prevalidates_without_accepted_truncation(self):
        long_value = 'x' * 256
        result = self.run_cli('-c', 'alias good=x ' + shlex.quote('too_long=' + long_value) + '; alias --json good', status=1)
        report = self.validate(json.loads(result.stdout))
        self.assertEqual(report['items'][0]['error'], 'not_found')
        result = self.execute(['alias', '--data', 'x=' + long_value], status=2)
        rows, terminal = native_stream(result.stdout)
        self.assertEqual(rows, [])
        self.assertEqual((terminal['state'], terminal['status']), (2, 2))

    def test_enable_matches_compiled_registry_and_changes_resolution(self):
        registry = json.loads(self.run_cli('--builtins-json').stdout)
        initial = self.report('enable')
        self.assertEqual({item['name'] for item in initial['items']}, {item['name'] for item in registry['builtins']})
        reports, _ = self.script_reports('enable --json -n pwd; enable --json -n; enable --json pwd; enable --json synthetic_missing', status=1)
        self.assertEqual((reports[0]['items'][0]['enabled_before'], reports[0]['items'][0]['enabled']), (True, False))
        self.assertEqual([item['name'] for item in reports[1]['items']], ['pwd'])
        self.assertTrue(reports[2]['items'][0]['enabled'])
        self.assertFalse(reports[3]['items'][0]['known'])
        self.assertIsNone(reports[3]['items'][0]['enabled'])

    def test_hash_receipts_match_table_and_clear_scope(self):
        executable = self.directory / 'audit_hash'
        executable.write_text('#!/bin/sh\nexit 0\n'); executable.chmod(0o700)
        self.env['PATH'] = str(self.directory) + ':' + os.defpath
        reports, _ = self.script_reports('hash --json audit_hash; hash --json; hash --json -r; hash --json')
        first = reports[0]['items'][0]
        self.assertEqual(first['path'], str(executable))
        self.assertTrue(first['changed'])
        self.assertEqual(reports[1]['items'][0]['path'], str(executable))
        self.assertEqual(reports[2]['items'][0]['cleared'], 1)
        self.assertEqual(reports[2]['items'][0]['scope'], 'builtin-hash-table')
        self.assertEqual(reports[3]['items'], [])
        self.assertEqual(self.report('hash', 'missing_hash', status=1)['items'][0]['error'], 'not_found')

    def test_plugin_callbacks_and_native_registry_receipts(self):
        fixture = self.plugin_fixture()
        path = shlex.quote(str(fixture))
        reports, result = self.script_reports(f'plugin --json load {path}; plugin --json list; plugin --json unload audit_fixture; plugin --json list')
        self.assertIn(b'fixture-plugin-init', result.stderr)
        self.assertIn(b'fixture-plugin-stop', result.stderr)
        self.assertTrue(reports[0]['items'][0]['loaded'])
        self.assertEqual(reports[0]['items'][0]['name'], 'audit_fixture')
        self.assertEqual(reports[1]['items'][0]['version'], '1.2')
        self.assertEqual(reports[1]['items'][0]['path'], str(fixture))
        self.assertFalse(reports[2]['items'][0]['loaded'])
        self.assertTrue(reports[2]['items'][0]['changed'])
        self.assertEqual(reports[3]['items'], [])
        self.assertEqual(self.report('plugin', 'unload', 'missing', status=1)['items'][0]['error'], 'not_loaded')

    def test_module_callbacks_reload_and_policy(self):
        fixture = self.module_fixture()
        path = shlex.quote(str(fixture))
        reports, result = self.script_reports(f'module --json load {path}; module --json info audit_module; module --json reload audit_module; module --json unload audit_module')
        self.assertIn(b'fixture-module-init', result.stderr)
        self.assertIn(b'fixture-module-stop', result.stderr)
        self.assertEqual(reports[0]['items'][0]['caps_declared'], 1)
        self.assertEqual(reports[1]['items'][0]['path'], str(fixture))
        self.assertTrue(reports[2]['items'][0]['unloaded'])
        self.assertTrue(reports[2]['items'][0]['loaded'])
        self.assertFalse(reports[3]['items'][0]['loaded'])
        self.env['JASH_MODULE_CAPS_STRICT'] = '1'
        denied = self.report('module', 'load', str(fixture), status=1)['items'][0]
        self.assertFalse(denied['changed'])
        self.assertEqual(denied['error'], 'load_failed')

    def test_module_failed_reload_records_prior_unload(self):
        fixture = self.module_fixture()
        path = shlex.quote(str(fixture))
        reports, _ = self.script_reports(f'module --json load {path}; rm {path} >/dev/null; module --json reload audit_module', status=1)
        row = reports[-1]['items'][0]
        self.assertTrue(row['unloaded'])
        self.assertTrue(row['changed'])
        self.assertFalse(row['loaded'])
        self.assertEqual(row['error'], 'load_failed')

    def test_management_native_rows_equal_json_queries(self):
        for name in ('alias', 'enable', 'hash', 'plugin', 'module'):
            expected = self.report(name)['items']
            for selector in ('--data', '--data-v2'):
                with self.subTest(name=name, selector=selector):
                    result = self.execute([name, selector])
                    rows, terminal = native_stream(result.stdout)
                    self.assertEqual([project(row) for row in rows], expected)
                    self.assertEqual((terminal['state'], terminal['completeness'], terminal['status']), (0, 1, 0))
        result = self.execute(['unalias', '--data', 'missing'], status=1)
        rows, terminal = native_stream(result.stdout)
        self.assertEqual((terminal['state'], terminal['status']), (2, 1))
        self.assertEqual(project(rows[0])['error'], 'not_found')

    def test_argument_errors_emit_failed_terminal_before_mutation(self):
        for name, args in [('alias', ['name=x', '--bogus']), ('unalias', ['-a', 'name']),
                           ('enable', ['-n', 'pwd', '--bogus']), ('hash', ['-r', 'sh']),
                           ('plugin', ['load', 'missing', 'extra']), ('module', ['load', 'missing', 'extra'])]:
            with self.subTest(name=name):
                result = self.execute([name, '--data', *args], status=2)
                rows, terminal = native_stream(result.stdout)
                self.assertEqual(rows, [])
                self.assertEqual((terminal['state'], terminal['status']), (2, 2))
                result = self.execute([name, '--json', *args], status=2)
                self.assertEqual(json.loads(result.stdout)['schema'], 'jash.builtin.error.v1')

    def test_jash_state_checks_publication_and_restore_errors(self):
        reports, _ = self.script_reports('alias saved_alias="echo stored"; export AUDIT_PERSIST=stored; jash_state --json save; alias saved_alias=changed; export AUDIT_PERSIST=changed; jash_state --json restore; alias --json saved_alias')
        self.assertTrue(reports[0]['items'][0]['applied'])
        self.assertTrue(reports[0]['items'][0]['durable'])
        self.assertTrue(reports[1]['items'][0]['applied'])
        self.assertEqual(reports[2]['items'][0]['value'], 'echo stored')
        saved = self.directory / '.jash_state'
        self.assertEqual(saved.stat().st_mode & 0o777, 0o600)
        saved.write_text('# JASH STATE FILE v2\n[ENV2]\nAUDIT_PERSIST=zz\n[END]\n')
        failure = self.report('jash_state', 'restore', status=1)['items'][0]
        self.assertFalse(failure['applied'])
        self.assertEqual(failure['error'], 'invalid_state_file')
        clear = self.report('jash_state', 'clear')['items'][0]
        self.assertTrue(clear['applied'])
        self.assertFalse(saved.exists())
        self.assertFalse(self.report('jash_state', 'clear')['items'][0]['applied'])

        # Automatic session restore uses the same reader with CWD enabled.
        # A missing saved directory must not survive as a fictitious PWD.
        sessions = self.directory / '.jash/sessions'
        sessions.mkdir(parents=True, exist_ok=True)
        missing = str(self.directory / 'missing-saved-directory')
        stale = sessions / 'stale-cwd.state'
        self.env['JASH_SESSION'] = 'stale-cwd'
        try:
            for contents in (
                f'# JASH STATE FILE\n[CWD]\n{missing}\n[ENV]\nPWD={missing}\nAUDIT_PERSIST=stored\n[ALIAS]\n',
                '# JASH STATE FILE v2\n[CWD2]\ncwd=' + os.fsencode(missing).hex() +
                '\n[ENV2]\nPWD=' + os.fsencode(missing).hex() +
                '\nAUDIT_PERSIST=' + b'stored'.hex() + '\n[ALIAS2]\n[END]\n',
            ):
                stale.write_text(contents)
                result = self.run_cli('-c', 'pwd --json; env --json')
                cwd, environment = [json.loads(line) for line in result.stdout.splitlines()]
                self.assertEqual(cwd['cwd'], str(self.directory))
                self.assertEqual(environment['PWD'], str(self.directory))
                self.assertEqual(environment['AUDIT_PERSIST'], 'stored')
        finally:
            del self.env['JASH_SESSION']

    def test_config_does_not_report_secret_and_observes_files(self):
        for command in ('config', 'config help'):
            self.assertIn(b'Usage:', self.run_cli('-c', command).stdout)
            self.assertFalse((self.directory / '.config').exists())
        initial = self.report('config', 'show')['items']
        self.assertFalse(next(row for row in initial if row['component'] == 'key_file')['exists'])
        self.report('config', 'init')
        key = 'synthetic-test-only-value'
        result = self.execute(['config', '--json', 'set-key', key])
        document = self.validate(json.loads(result.stdout))
        self.assertNotIn(key.encode(), result.stdout + result.stderr)
        target = self.directory / '.config/jash/groq.key'
        self.assertEqual(target.read_text(), key + '\n')
        self.assertEqual(target.stat().st_mode & 0o777, 0o600)
        self.assertTrue(document['items'][-1]['changed'])
        self.assertTrue(document['items'][-1]['durable'])
        observed = self.report('config', 'show')['items']
        self.assertTrue(next(row for row in observed if row['component'] == 'key_file')['key_configured'])

    def test_doctor_probe_preserves_existing_file_and_byte_counts(self):
        tmp = self.directory / 'temporary "quoted"'
        tmp.mkdir()
        sentinel = tmp / '.jash_doctor_probe'
        sentinel.write_text('synthetic sentinel')
        self.env['TMPDIR'] = str(tmp)
        rows = self.report('doctor')['items']
        self.assertEqual(sentinel.read_text(), 'synthetic sentinel')
        self.assertEqual(list(tmp.iterdir()), [sentinel])
        writable = next(row for row in rows if row['check'] == 'write.tmpdir')
        self.assertTrue(writable['healthy'])
        self.assertEqual(writable['path'], str(tmp))
        disk = next(row for row in rows if row['check'] == 'disk.tmpdir')
        observed = os.statvfs(tmp)
        self.assertEqual(disk['total_bytes'], observed.f_blocks * observed.f_frsize)
        self.assertLessEqual(disk['available_bytes'], disk['total_bytes'])


    def fault_library(self):
        library = self.directory / 'management_faults.so'
        result = subprocess.run([os.environ.get('CC', 'cc'), '-shared', '-fPIC',
            str(ROOT / 'tests/management_io_faults.c'), '-ldl', '-o', str(library)],
            env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=25)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.env['LD_PRELOAD'] = str(library)
        self.env['JASH_TEST_MANAGEMENT_ROOT'] = str(self.directory)

    def test_persist_faults_distinguish_publication_from_durability(self):
        self.fault_library()
        state = self.directory / '.jash_state'
        for fault, error, applied in [('state_file_fsync', 'fsync_failed', False),
                                      ('state_close', 'close_failed', False),
                                      ('state_rename', 'rename_failed', False),
                                      ('state_dir_fsync', 'directory_sync_failed', True)]:
            with self.subTest(fault=fault):
                state.unlink(missing_ok=True)
                self.env['JASH_TEST_MANAGEMENT_FAULT'] = fault
                row = self.report('jash_state', 'save', status=1)['items'][0]
                self.assertEqual((row['error'], row['applied'], row['durable']), (error, applied, False))
                self.assertEqual(state.exists(), applied)
                self.assertEqual(list(self.directory.glob('.jash_state.tmp.*')), [])
        self.env['JASH_TEST_MANAGEMENT_FAULT'] = 'state_read'
        row = self.report('jash_state', 'restore', status=1)['items'][0]
        self.assertEqual((row['error'], row['applied']), ('read_failed', False))
        self.env['JASH_TEST_MANAGEMENT_FAULT'] = 'state_unlink'
        row = self.report('jash_state', 'clear', status=1)['items'][0]
        self.assertEqual((row['error'], row['applied']), ('unlink_failed', False))
        self.assertTrue(state.exists())
        self.env['JASH_TEST_MANAGEMENT_FAULT'] = 'state_dir_fsync'
        row = self.report('jash_state', 'clear', status=1)['items'][0]
        self.assertEqual((row['error'], row['applied'], row['durable']), ('directory_sync_failed', True, False))
        self.assertFalse(state.exists())

    def test_config_faults_preserve_old_key_before_publication(self):
        self.report('config', 'init')
        target = self.directory / '.config/jash/groq.key'
        self.fault_library()
        for fault, error, changed in [('config_file_fsync', 'fsync_failed', False),
                                      ('config_rename', 'publish_failed', False),
                                      ('config_dir_fsync', 'directory_sync_failed', True)]:
            with self.subTest(fault=fault):
                target.write_text('synthetic-old-value\n')
                self.env['JASH_TEST_MANAGEMENT_FAULT'] = fault
                result = self.execute(['config', '--json', 'set-key', 'synthetic-new-value'], status=1)
                row = self.validate(json.loads(result.stdout))['items'][-1]
                self.assertEqual((row['error'], row['changed'], row['durable']), (error, changed, False))
                self.assertEqual(target.read_text(), 'synthetic-new-value\n' if changed else 'synthetic-old-value\n')
                self.assertNotIn(b'synthetic-new-value', result.stdout + result.stderr)
                self.assertEqual(list(target.parent.glob('groq.key.tmp.*')), [])

    def test_doctor_gaps_exact_count_and_input_errors(self):
        gaps = self.directory / '.jash/gaps.tsv'
        gaps.parent.mkdir(exist_ok=True)
        gaps.write_text('id\tstatus\tnote\none\topen\tvalue\ntwo\tclosed\tvalue\nthree\topen\tvalue\n')
        rows = self.report('doctor')['items']
        row = next(row for row in rows if row['check'] == 'gaps.open')
        self.assertEqual(row['value'], 2)
        gaps.write_bytes(b'one\topen\tbad\x00data\n')
        row = next(row for row in self.report('doctor', status=1)['items'] if row['check'] == 'gaps.open')
        self.assertFalse(row['ok'])
        self.assertEqual(row['error'], 'invalid_gaps_file')
        self.assertIsNone(row['value'])
        gaps.write_bytes(b'x' * 65537)
        row = next(row for row in self.report('doctor', status=1)['items'] if row['check'] == 'gaps.open')
        self.assertEqual(row['error'], 'gaps_line_limit_exceeded')

    def test_all_management_modes_check_sink_failures(self):
        cases = [('alias', []), ('unalias', ['-a']), ('enable', []), ('hash', []), ('plugin', ['list']),
                 ('module', ['list']), ('jash_state', ['clear']), ('config', ['show']), ('doctor', [])]
        for name, args in cases:
            for selector in ('--json', '--data'):
                with self.subTest(name=name, selector=selector):
                    with open('/dev/full', 'wb') as sink:
                        self.execute([name, selector, *args], status=125, stdout=sink)

    def test_management_binary_values_remain_reversible(self):
        assignment = b'audit_bytes=value-\xff-quote"'
        result = self.execute(['alias', '--json', assignment])
        value = self.validate(json.loads(result.stdout))['items'][0]['value']
        self.assertEqual(base64.b64decode(value['data']), assignment.split(b'=', 1)[1])
        rows, terminal = native_stream(self.execute(['alias', '--data', assignment]).stdout)
        self.assertEqual(rows[0][1]['value'][0], 8)
        self.assertEqual(project(rows[0])['value'], value)
        self.assertEqual(terminal['state'], 0)

    def test_final_management_providers_native_kinds_and_errors(self):
        for name, args in [('jash_state', ['clear']), ('config', ['show']), ('doctor', [])]:
            with self.subTest(name=name):
                result = self.execute([name, '--data', *args])
                rows, terminal = native_stream(result.stdout)
                self.assertTrue(rows)
                self.assertEqual(terminal['state'], 0)
                for row in rows:
                    self.assertEqual(row[1]['path'][0], 9)
                if name == 'doctor':
                    disk = next(row for row in rows if project(row)['check'] == 'disk.root')
                    self.assertEqual(disk[1]['total_bytes'][0], 4)
                result = self.execute([name, '--data', 'invalid_subcommand'], status=2)
                rows, terminal = native_stream(result.stdout)
                self.assertEqual(rows, [])
                self.assertEqual((terminal['state'], terminal['status']), (2, 2))


if __name__ == '__main__':
    unittest.main(verbosity=2)
