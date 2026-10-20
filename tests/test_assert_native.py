#!/usr/bin/env python3
"""Actual scoped assertion verification, publication and native failure receipts."""
import copy
import errno
import fcntl
import json
import os
import shlex
import subprocess
import sys
import time
import unittest
from jsonschema import Draft202012Validator
import test_management_builtins as management
from test_discovery_builtins import native_stream, project


class AssertNative(unittest.TestCase):
    run_cli = management.ManagementBuiltins.run_cli
    execute = management.ManagementBuiltins.execute
    compile_library = management.ManagementBuiltins.compile_library

    def setUp(self):
        management.ManagementBuiltins.setUp(self)
        (self.directory / '.git').mkdir()
        schema = json.loads((management.ROOT / 'schemas/builtin-assert-v1.schema.json').read_text())
        Draft202012Validator.check_schema(schema)
        self.validator = Draft202012Validator(schema)

    def validate(self, value):
        self.validator.validate(value)
        return value

    def report(self, *args, status=0):
        process = self.execute(['assert', '--json', *args], status=status)
        return [self.validate(json.loads(row)) for row in process.stdout.splitlines() if row.strip()], process

    def native(self, *args, status=0, failed=False, selector='--data-v2'):
        process = self.execute(['assert', selector, *args], status=status)
        rows, terminal = native_stream(process.stdout)
        self.assertEqual((terminal['state'], terminal['status'], terminal['errors']),
                         (2 if failed else 0, status, int(failed)))
        self.assertEqual(terminal['completeness'], 2 if failed else 1)
        self.assertEqual(terminal['logical_bytes'], 0)
        return [self.validate(project(row)) for row in rows], terminal, process

    @property
    def store(self):
        return self.directory / '.jash/assertions.tsv'

    def test_add_and_reverify_use_actual_supervisor_and_content_hashes(self):
        (self.directory / 'input space').write_bytes(b'known bytes\x00\xff')
        rows, process = self.report('binary content', '--dep', 'input space', '--check',
                                    'test -f "input space"; echo hidden; echo hidden >&2')
        self.assertEqual([row['kind'] for row in rows], ['verification', 'summary'])
        execution = rows[0]['execution']
        self.assertTrue(execution['requested']); self.assertTrue(execution['started'])
        self.assertTrue(execution['status_valid']); self.assertEqual(execution['command_status'], 0)
        self.assertTrue(execution['quiescent']); self.assertTrue(execution['descendants_quiescent'])
        self.assertEqual(process.stderr, b'')
        summary = rows[-1]; row = summary['assertions'][0]
        self.assertEqual(row['claim'], 'binary content'); self.assertEqual(row['cwd'], str(self.directory))
        self.assertEqual(row['scope'], str(self.directory)); self.assertTrue(row['assessed_in_call'])
        self.assertEqual(len(row['dependencies'][0]['blake3']), 64)
        self.assertTrue(summary['persistence']['applied']); self.assertTrue(summary['persistence']['durable'])
        self.assertTrue(self.store.read_bytes().startswith(b'v2\t'))
        native = self.native()[0]
        self.assertEqual(native[-1]['assertions'][0]['dependencies'], row['dependencies'])

    def test_status_hashes_without_execution_or_persistence_and_detects_same_size_change(self):
        dep = self.directory / 'input'; dep.write_bytes(b'one')
        self.native('content', '--dep', 'input', '--check', 'touch executed')
        (self.directory / 'executed').unlink(); original = self.store.read_bytes()
        value = self.report('--status')[0][-1]
        self.assertTrue(value['status_only']); self.assertEqual(value['source']['lock_mode'], 'shared')
        self.assertEqual(value['assertions'][0]['state'], 'vale')
        self.assertFalse(value['persistence']['requested']); self.assertFalse((self.directory / 'executed').exists())
        metadata = dep.stat(); dep.write_bytes(b'two'); os.utime(dep, ns=(metadata.st_atime_ns, metadata.st_mtime_ns))
        value = self.native('--status', status=1)[0][-1]
        self.assertEqual(value['assertions'][0]['state'], 'invalidated')
        self.assertEqual(value['assertions'][0]['reason'], 'dependency_changed')
        self.assertEqual(self.store.read_bytes(), original)
        self.assertFalse((self.directory / 'executed').exists())

    def test_false_verifiers_and_prior_true_transitions_are_complete_observations(self):
        marker = self.directory / 'available'; marker.touch()
        self.native('transition', '--check', 'test -f available')
        marker.unlink()
        rows = self.native(status=1)[0]
        self.assertEqual(rows[0]['execution']['command_status'], 1)
        self.assertEqual(rows[-1]['assertions'][0]['state'], 'caiu')
        self.assertTrue(rows[-1]['complete']); self.assertEqual(rows[-1]['predicate_status'], 1)
        value = self.report('never', '--check', 'exit 7', status=1)[0]
        self.assertEqual(value[0]['execution']['command_status'], 7)
        self.assertEqual(value[-1]['assertions'][0]['state'], 'nunca')
        marker.touch(); self.native('--forget', 'never')
        self.assertEqual(self.native()[0][-1]['assertions'][0]['state'], 'vale')

    def test_status_without_dependencies_does_not_promote_old_success(self):
        self.native('unscoped inputs', '--check', 'touch executed')
        (self.directory / 'executed').unlink()
        value = self.native('--status', status=1)[0][-1]
        self.assertEqual(value['assertions'][0]['reason'], 'no_dependencies')
        self.assertEqual(value['assertions'][0]['state'], 'unknown')
        self.assertTrue(value['complete']); self.assertFalse((self.directory / 'executed').exists())

    def test_timeout_reaps_descendants_and_retains_incomplete_state(self):
        start = time.monotonic()
        rows = self.native('deadline', '--timeout-ms', '80', '--check',
                           '(sleep 0.4; touch escaped) & wait', status=75, failed=True)[0]
        self.assertLess(time.monotonic() - start, 3)
        self.assertTrue(rows[0]['execution']['started'])
        self.assertEqual(rows[-1]['assertions'][0]['state'], 'unknown')
        self.assertEqual(rows[-1]['assertions'][0]['reason'], 'timeout')
        self.assertFalse(rows[-1]['complete']); self.assertIsNone(rows[-1]['predicate_status'])
        self.assertTrue(rows[-1]['persistence']['applied'])
        time.sleep(0.45)
        self.assertFalse((self.directory / 'escaped').exists())
        self.native('--status', status=75, failed=True)

    def test_legacy_unscoped_and_other_scopes_are_preserved_without_execution(self):
        (self.directory / '.jash').mkdir(mode=0o700)
        legacy = b'legacy claim\ttouch unscoped-executed\t1\t1\t1\n'
        self.store.write_bytes(legacy)
        value = self.native('--status')[0][-1]
        self.assertEqual(value['legacy_unscoped'], 1); self.assertEqual(value['assertions'], [])
        self.native('local', '--check', 'true')
        other = self.directory / 'other'; other.mkdir(); (other / '.git').mkdir()
        command = 'cd ' + shlex.quote(str(other)) + '; assert --json --status'
        value = self.validate(json.loads(self.run_cli('-c', command).stdout))
        self.assertEqual(value['assertions'], []); self.assertEqual(value['legacy_unscoped'], 1)
        self.native('--forget', 'local')
        self.assertEqual(self.store.read_bytes(), legacy)
        self.assertFalse((self.directory / 'unscoped-executed').exists())

    def test_invalid_arguments_and_bad_store_are_refused_before_execution(self):
        cases = [('x' * 256, '--check', 'touch bad'),
                 ('claim', '--check', 'x' * 512),
                 ('claim', '--dep', 'x' * 4096, '--check', 'touch bad'),
                 ('claim', '--timeout-ms', '0', '--check', 'touch bad'),
                 ('--status', 'claim', '--check', 'touch bad'),
                 ('claim', '--check', 'touch bad', '--check', 'true')]
        for args in cases:
            self.native(*args, status=2, failed=True)
        self.assertFalse(self.store.exists()); self.assertFalse((self.directory / 'bad').exists())
        (self.directory / '.jash').mkdir(mode=0o700, exist_ok=True)
        for payload in (b'bad row\n', b'claim\ttouch bad\t1\t1\t1\n\x00\n'):
            self.store.write_bytes(payload)
            value = self.native('claim', '--check', 'touch bad', status=1, failed=True)[0][-1]
            self.assertFalse(value['source']['read_complete']); self.assertEqual(self.store.read_bytes(), payload)
        self.store.unlink(); external = self.directory / 'external'; external.write_bytes(b'untouched')
        self.store.symlink_to(external)
        self.native('claim', '--check', 'touch bad', status=1, failed=True)
        self.assertEqual(external.read_bytes(), b'untouched'); self.assertFalse((self.directory / 'bad').exists())

    def test_dependency_fifo_failure_is_bounded_and_not_predicate_false(self):
        os.mkfifo(self.directory / 'fifo')
        start = time.monotonic()
        rows = self.native('fifo input', '--dep', 'fifo', '--check', 'touch bad', status=1, failed=True)[0]
        self.assertLess(time.monotonic() - start, 2)
        self.assertFalse(rows[0]['execution']['started'])
        self.assertEqual(rows[-1]['assertions'][0]['reason'], 'dependency_unavailable')
        self.assertFalse((self.directory / 'bad').exists())

    def test_lock_contention_is_immediate_and_never_runs_verifier(self):
        self.native('seed', '--check', 'true')
        original = self.store.read_bytes()
        with (self.directory / '.jash/assertions.lock').open('r+b') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            start = time.monotonic()
            value = self.native('busy', '--check', 'touch bad', status=1, failed=True)[0][-1]
            self.assertIn(value['errno'], (errno.EAGAIN, errno.EWOULDBLOCK))
            self.native('--status', status=1, failed=True)
            self.assertLess(time.monotonic() - start, 2)
        self.assertEqual(self.store.read_bytes(), original); self.assertFalse((self.directory / 'bad').exists())

    def test_json_budget_and_sink_admission_precede_store_creation_and_execution(self):
        for selector in ('--json', '--data-v2'):
            with open('/dev/full', 'wb') as sink:
                self.execute(['assert', selector, 'claim', '--check', 'touch bad'], status=125, stdout=sink)
            self.assertFalse(self.store.exists()); self.assertFalse((self.directory / 'bad').exists())
            self.assertFalse((self.directory / '.jash/assertions.lock').exists())
        self.env['JASH_LLM_BUDGET'] = '1'
        value = json.loads(self.execute(['assert', '--json', 'claim', '--check', 'touch bad'], status=75).stdout)
        self.assertEqual(value['schema'], 'jash.builtin.error.v1')
        self.assertFalse(self.store.exists()); self.assertFalse((self.directory / 'bad').exists())
        for selector in ('--data', '--data-v2'):
            self.native('data budget ' + selector, '--check', 'true', selector=selector)

    def test_persistence_phase_receipts_and_namespace_change(self):
        library = self.compile_library('assert_fault', r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int published;
static int target(int fd,int temporary) {
 char link[64],path[4096];snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);
 ssize_t n=readlink(link,path,sizeof(path)-1);const char*root=getenv("ASSERT_TEST_ROOT");
 if(n<=0||!root)return 0;path[n]=0;size_t size=strlen(root);
 return temporary ? !strncmp(path,root,size)&&!strncmp(path+size,"/.assertions.",13) : !strcmp(path,root);
}
int renameat(int a,const char*b,int c,const char*d) {
 int(*next)(int,const char*,int,const char*)=dlsym(RTLD_NEXT,"renameat");
 int result=next(a,b,c,d);if(!result&&!strcmp(d,"assertions.tsv")&&target(c,0))published=1;return result;
}
int fsync(int fd) {
 int(*next)(int)=dlsym(RTLD_NEXT,"fsync");const char*mode=getenv("ASSERT_TEST_PHASE");
 if(mode&&((!strcmp(mode,"before")&&target(fd,1))||(!strcmp(mode,"after")&&published&&target(fd,0)))){errno=EIO;return -1;}
 return next(fd);
}
''')
        self.env.update(LD_PRELOAD=str(library), ASSERT_TEST_ROOT=str(self.directory / '.jash'))
        for phase, applied in [('before', False), ('after', True)]:
            self.env['ASSERT_TEST_PHASE'] = phase
            rows = self.native('phase ' + phase, '--check', 'true', status=1, failed=True)[0]
            final = rows[-1]
            self.assertEqual(final['error'], 'assertion_persistence_failed')
            self.assertEqual(final['persistence']['applied'], applied)
            self.assertFalse(final['persistence']['durable']); self.assertEqual(final['persistence']['errno'], errno.EIO)
            self.assertEqual(self.store.exists(), applied)
        del self.env['LD_PRELOAD']; del self.env['ASSERT_TEST_PHASE']
        rows = self.native('move source', '--check', 'mv "$HOME/.jash" "$HOME/moved"; mkdir -m 700 "$HOME/.jash"', status=1, failed=True)[0]
        self.assertEqual(rows[-1]['error'], 'assertion_persistence_failed')
        self.assertFalse(rows[-1]['persistence']['applied'])
        self.assertFalse(self.store.exists())
        self.assertNotIn(b'move source'.hex().encode(), (self.directory / 'moved/assertions.tsv').read_bytes())

    def test_real_provider_stops_after_failed_first_receipt_without_persisting(self):
        sources = ['tests/test_assert_delivery.c', 'c/builtins/builtin_assert.c',
                   'c/builtins/registry.c', 'c/core/argv.c', 'c/core/agent_json.c',
                   'c/core/agent_process.c', 'c/core/agent_cgroup.c', 'c/core/execution_ir.c',
                   'c/parser/cmdsub.c', 'c/core/structured_output.c', 'c/core/json_writer.c',
                   'c/core/data_json.c', 'c/core/data_value.c', 'c/core/data_stream.c',
                   'c/core/data_wire.c', 'c/core/data_wire_decode.c', 'c/core/data_wire_import.c',
                   'c/core/data_emit.c', 'c/module/blake3.c', 'c/theme/io.c']
        binary = self.directory / 'assert-delivery'
        command = [os.environ.get('CC', 'cc'), '-std=gnu11', '-DJASH_TARGET_POSIX', '-O1', '-g',
                   '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                   '-I' + str(management.ROOT / 'c'),
                   *[str(management.ROOT / source) for source in sources], '-lm', '-o', str(binary)]
        if os.environ.get('SANITIZE') == '1':
            command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
        built = subprocess.run(command, capture_output=True, timeout=45)
        self.assertEqual(built.returncode, 0, built.stderr)
        result = subprocess.run([str(binary)], cwd=self.directory, env=self.env, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_schema_rejects_invented_success_and_missing_publication_evidence(self):
        value = self.report('false claim', '--check', 'false', status=1)[0][-1]
        wrong = copy.deepcopy(value); wrong['predicate_status'] = 0
        self.assertFalse(self.validator.is_valid(wrong))
        wrong = copy.deepcopy(value); wrong['complete'] = False
        self.assertFalse(self.validator.is_valid(wrong))
        wrong = copy.deepcopy(value); del wrong['persistence']['scope']
        self.assertFalse(self.validator.is_valid(wrong))
        wrong = copy.deepcopy(value); wrong['persistence']['applied'] = False
        self.assertFalse(self.validator.is_valid(wrong))


if __name__ == '__main__':
    unittest.main(verbosity=2)
