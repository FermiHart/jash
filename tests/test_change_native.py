#!/usr/bin/env python3
"""Change source models and phase receipts checked against real project state."""
import copy
import json
import os
import re
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
from jsonschema.exceptions import ValidationError

import native_data_fixture as f
from contract_schema_fixture import projection, validate_contract


FAULT_SOURCE = r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int mode(const char* name) { const char* p=getenv("CHANGE_TEST_FAULT"); return p&&!strcmp(p,name); }
static int path_of(int fd,char path[PATH_MAX+1]) {
    char link[64]; snprintf(link,sizeof link,"/proc/self/fd/%d",fd);
    ssize_t n=readlink(link,path,PATH_MAX); if(n<0) return 0; path[n]=0; return 1;
}
static int temporary(int fd) { char path[PATH_MAX+1]; return path_of(fd,path)&&strstr(path,"/.change-native-")!=NULL; }
static int target(int fd,const char* variable) {
    const char* expected=getenv(variable); char path[PATH_MAX+1];
    return expected&&path_of(fd,path)&&!strcmp(path,expected);
}
ssize_t write(int fd,const void* data,size_t size) {
    static ssize_t (*real)(int,const void*,size_t); if(!real) *(void**)&real=dlsym(RTLD_NEXT,"write");
    if ((mode("write")||mode("cleanup"))&&temporary(fd)) { errno=ENOSPC; return -1; }
    const char* selected=getenv("CHANGE_TEST_FILE");
    if(fd==1&&mode("sink-after-publish")&&selected&&access(selected,F_OK)==0) { errno=EPIPE; return -1; }
    static int swapped;
    if(fd==1&&mode("checkpoint-swap")&&selected&&!swapped) {
        swapped=1; int file=open(selected,O_WRONLY|O_APPEND); if(file>=0) { (void)real(file,"\n",1); close(file); }
    }
    return real(fd,data,size);
}
int fsync(int fd) {
    static int (*real)(int); if(!real) *(void**)&real=dlsym(RTLD_NEXT,"fsync");
    if((mode("fsync")&&temporary(fd))||(mode("directory-fsync")&&target(fd,"CHANGE_TEST_DIR"))) { errno=EIO; return -1; }
    return real(fd);
}
int close(int fd) {
    static int (*real)(int); if(!real) *(void**)&real=dlsym(RTLD_NEXT,"close");
    int fail=(mode("close")&&temporary(fd))||(mode("source-close")&&target(fd,"CHANGE_TEST_FILE"));
    int result=real(fd); if(fail) { errno=EIO; return -1; } return result;
}
ssize_t read(int fd,void* data,size_t size) {
    static ssize_t (*real)(int,void*,size_t); if(!real) *(void**)&real=dlsym(RTLD_NEXT,"read");
    if(mode("source-read")&&target(fd,"CHANGE_TEST_FILE")) { errno=EIO; return -1; }
    return real(fd,data,size);
}
int linkat(int from,const char* source,int to,const char* name,int flags) {
    static int (*real)(int,const char*,int,const char*,int); if(!real) *(void**)&real=dlsym(RTLD_NEXT,"linkat");
    if(mode("link")&&!strncmp(source,".change-native-",15)) { errno=EIO; return -1; }
    return real(from,source,to,name,flags);
}
int renameat(int from,const char* source,int to,const char* name) {
    static int (*real)(int,const char*,int,const char*); if(!real) *(void**)&real=dlsym(RTLD_NEXT,"renameat");
    if(mode("rename")&&!strncmp(source,".change-native-",15)) { errno=EIO; return -1; }
    return real(from,source,to,name);
}
int unlinkat(int parent,const char* name,int flags) {
    static int (*real)(int,const char*,int); if(!real) *(void**)&real=dlsym(RTLD_NEXT,"unlinkat");
    if(mode("cleanup")&&!strncmp(name,".change-native-",15)) { errno=EACCES; return -1; }
    return real(parent,name,flags);
}
struct dirent* readdir(DIR* dir) {
    static struct dirent* (*real)(DIR*); if(!real) *(void**)&real=dlsym(RTLD_NEXT,"readdir");
    if(mode("readdir")&&target(dirfd(dir),"CHANGE_TEST_DIR")) { errno=EIO; return NULL; }
    return real(dir);
}
'''


class ChangeNative(f.NativeQueryCase):
    @classmethod
    def setUpClass(cls):
        temporary = tempfile.TemporaryDirectory(prefix='jash-change-faults-')
        cls.addClassCleanup(temporary.cleanup)
        source = Path(temporary.name) / 'faults.c'
        source.write_text(FAULT_SOURCE)
        cls.hook = Path(temporary.name) / 'faults.so'
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        str(source), '-o', str(cls.hook), '-ldl'], check=True, capture_output=True)

    def setUp(self):
        super().setUp()
        subprocess.run(['git', 'init', '-q', str(self.work)], check=True)

    def report(self, *arguments, mode='--data-v2', rc=0, strict=False, validate=True):
        response = self.run_command(['change', mode, *arguments], strict=strict,
                                    caps=['effect:read', 'effect:mutate', 'effect:exec'])
        self.assertEqual(response.returncode, rc, response.stderr + response.stdout[:256])
        if mode == '--json':
            rows = [json.loads(line) for line in response.stdout.splitlines() if line.strip()]
            terminal = None
        else:
            terminal = f.stream(response.stdout, credit_kind='none')
            self.assertEqual(terminal.producer, rc)
            rows = [projection(row) for row in terminal.items]
        self.assertTrue(rows)
        if validate:
            for row in rows: validate_contract('change', row)
        return rows, terminal

    def seed(self, name='demo', schema='spec-driven', complete=False):
        if not (self.work / '.changes/schemas' / schema).exists():
            self.report('schema', 'init', schema)
        self.report('propose', name, '--schema', schema)
        if complete: self.report('ff', name)
        return self.work / '.changes' / name

    def fault(self, mode, *, file=None, directory=None):
        self.env['LD_PRELOAD'] = str(self.hook)
        self.env['CHANGE_TEST_FAULT'] = mode
        if file: self.env['CHANGE_TEST_FILE'] = str(file)
        if directory: self.env['CHANGE_TEST_DIR'] = str(directory)

    def clear_fault(self):
        for key in tuple(self.env):
            if key.startswith('CHANGE_TEST_') or key == 'LD_PRELOAD': self.env.pop(key)

    def test_queries_are_readonly_complete_and_modes_share_sources(self):
        for mode in ('--json', '--data', '--data-v2'):
            for strict in (False, True):
                rows, terminal = self.report('status', mode=mode, strict=strict)
                self.assertEqual(rows[0]['changes'], [])
                self.assertEqual(rows[0]['schemas'], [])
                self.assertFalse((self.work / '.changes').exists())
                if terminal: self.assertEqual((terminal.state, terminal.errors), (0, 0))
        self.report('schema', 'init', 'spec-driven', mode='--json', strict=True)
        goal = 'goal with spaces\t"quoted"\nsecond line'
        self.report('propose', '--goal', goal, '--', '-demo', mode='--data', strict=True)
        before = (self.work / '.changes/-demo/.change.yaml').read_bytes()
        for mode in ('--json', '--data-v2'):
            rows, _ = self.report('recall', '--', '-demo', mode=mode, strict=True)
            change = rows[0]['changes'][0]
            self.assertEqual(change['goal'], goal)
            self.assertEqual(change['artifacts'][0]['status'], 'ready')
            self.assertTrue(all(x['status'] == 'blocked' for x in change['artifacts'][1:]))
        self.assertEqual((self.work / '.changes/-demo/.change.yaml').read_bytes(), before)
        legacy = self.run_command(['change', 'status', '--json'])
        self.assertEqual(legacy.returncode, 0, legacy.stderr)
        shape = json.loads(legacy.stdout)
        validate_contract('change-legacy-status', shape)
        self.assertEqual(set(shape), {'schemas', 'changes'})
        self.assertEqual(shape['changes'][0]['goal'], goal)
        self.assertEqual(shape['changes'][0]['artifacts'][0]['status'], 'pending')
        for mode in ('--json', '--data', '--data-v2'):
            for argv in (['change', mode, 'status'], ['change', 'status', mode]):
                response = self.run_command(argv, strict=True, caps=['effect:read'])
                self.assertEqual(response.returncode, 0, response.stderr)
                if mode == '--json':
                    value = json.loads(response.stdout)
                    validate_contract('change-legacy-status' if argv[1] == 'status' else 'change', value)
                else:
                    terminal = f.stream(response.stdout, credit_kind='none')
                    self.assertEqual((terminal.state, terminal.errors), (0, 0))
                    value = projection(terminal.items[-1]); validate_contract('change', value)
                self.assertEqual(value['changes'][0]['goal'], goal)
        self.assertEqual((self.work / '.changes/-demo/.change.yaml').read_bytes(), before)

    def test_schema_fork_and_custom_literal_generated_path(self):
        self.report('schema', 'init', 'spike')
        self.report('schema', 'fork', 'spike', 'local')
        path = self.work / '.changes/schemas/local/schema.yaml'
        path.write_text(path.read_text().replace('findings.md', '"notes with space.md"'))
        change = self.seed(schema='local')
        rows, terminal = self.report('continue', 'demo', strict=True)
        self.assertEqual(terminal.state, 0)
        self.assertTrue((change / 'notes with space.md').read_text().startswith('# Findings: demo'))
        receipt = next(row for row in rows if row['record'] == 'receipt' and row['phase'] == 'publish')
        self.assertEqual(receipt['detail'], (change / 'notes with space.md').stat().st_size)
        self.assertTrue(receipt['applied'] and receipt['durable'])

    def test_all_owned_templates_preserve_exact_bytes(self):
        source = (Path(__file__).resolve().parents[1] / 'c/builtins/builtin_change.c').read_text()
        templates = {}
        for key, body in re.findall(r'static const char\* CHG_TPL_(\w+) =\n(.*?);', source, re.S):
            text = ''.join(json.loads(literal) for literal in re.findall(r'"(?:\\.|[^"\\])*"', body))
            self.assertEqual(text.count('%'), 1)
            self.assertIn('%s', text)
            templates[key] = text
        schemas = {
            'spec-driven': {'proposal.md': 'PROPOSAL', 'specs/spec.md': 'SPEC',
                            'design.md': 'DESIGN', 'tasks.md': 'TASKS'},
            'bugfix': {'reproduce.md': 'REPRODUCE', 'fix.md': 'FIX', 'verify.md': 'VERIFY_BUGFIX'},
            'spike': {'findings.md': 'FINDINGS'},
        }
        self.assertEqual(len(templates), 8)
        for schema, files in schemas.items():
            name = 'template-' + schema
            change = self.seed(name=name, schema=schema, complete=True)
            for filename, key in files.items():
                self.assertEqual((change / filename).read_bytes(), (templates[key] % name).encode())

    def test_fast_forward_apply_and_actual_checkpoint_receipts(self):
        change = self.seed()
        rows, terminal = self.report('apply', 'demo', rc=1)
        self.assertEqual(rows[0]['error'], 'artifacts_incomplete')
        self.assertFalse(list((Path(self.env['HOME']) / '.jash').glob('checkpoints*.tsv')))
        rows, _ = self.report('ff', 'demo', mode='--json')
        self.assertTrue(all((change / name).is_file() for name in ('proposal.md', 'specs/spec.md', 'design.md', 'tasks.md')))
        self.assertIn('status: ready', (change / '.change.yaml').read_text())
        rows, terminal = self.report('apply', 'demo', strict=True)
        phases = [row['phase'] for row in rows if row['record'] == 'receipt']
        self.assertEqual(phases, ['checkpoint', 'publish'])
        checkpoint = rows[0]
        self.assertTrue(checkpoint['applied'] and checkpoint['durable'])
        self.assertTrue(Path(checkpoint['path']).is_file())
        self.assertIn(checkpoint['detail']['name'], Path(checkpoint['path']).read_text())
        self.assertIn('status: applying', (change / '.change.yaml').read_text())
        self.assertEqual(terminal.state, 0)

    def test_sync_multiple_requirements_modification_removal_and_conflict_before_effect(self):
        change = self.seed(schema='spike')
        specs = change / 'specs'; specs.mkdir()
        delta = specs / 'auth file.md'
        delta.write_text('## ADDED Requirements\n\n### Requirement: First\none\n\n### Requirement: Second\ntwo\n')
        self.report('sync', 'demo')
        target = self.work / 'specs/auth file.md'
        text = target.read_text()
        self.assertEqual(text.count('### Requirement:'), 2)
        self.assertIn('First\none', text); self.assertIn('Second\ntwo', text)
        before = target.read_bytes()
        rows, terminal = self.report('sync', 'demo', rc=1, strict=True)
        self.assertEqual(rows[0]['error'], 'delta_conflict')
        self.assertFalse(rows[0]['effects_started'])
        self.assertEqual(target.read_bytes(), before)
        self.assertEqual(terminal.state, 2)
        delta.write_text('## MODIFIED Requirements\n### Requirement: First\nnew\n## REMOVED Requirements\n### Requirement: Second\n')
        self.report('sync', 'demo', mode='--json')
        self.assertIn('First\nnew', target.read_text()); self.assertNotIn('Second', target.read_text())
        # A later conflict refuses the whole prepared batch, including an earlier valid target.
        delta.write_text('## ADDED Requirements\n### Requirement: First\nconflict\n')
        (specs / 'a-valid.md').write_text('## ADDED Requirements\n### Requirement: Valid\nbody\n')
        self.report('archive', 'demo', rc=1)
        self.assertFalse((self.work / 'specs/a-valid.md').exists())
        self.assertTrue(change.exists())

    def test_archive_rewind_observes_actual_timeline_inverse(self):
        change = self.seed(schema='spike', complete=True)
        self.env['JASH_TIMELINE'] = '1'
        (self.work / 'target').write_text('before\n')
        rows, _ = self.report('archive', 'demo', mode='--json')
        archived = Path(self.work / rows[-1]['path'])
        self.assertFalse(change.exists()); self.assertTrue(archived.is_dir())
        # The timeline belongs to a live shell process. The overwrite and the
        # inverse must share that process; checkpoints do not persist its ring.
        command = 'echo after > target; change --data-v2 rewind ' + shlex.quote(archived.name)
        result = subprocess.run([str(f.JASH), '-c', command], cwd=self.work, env=self.env, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        terminal = f.stream(result.stdout, credit_kind='none')
        rows = [projection(row) for row in terminal.items]
        for row in rows: validate_contract('change', row)
        self.assertTrue(change.is_dir()); self.assertFalse(archived.exists())
        self.assertEqual((self.work / 'target').read_text(), 'before\n')
        self.assertIn('status: proposed', (change / '.change.yaml').read_text())
        inverse = next(row for row in rows if row.get('phase') == 'timeline')
        self.assertTrue(inverse['applied']); self.assertIsNone(inverse['durable'])
        self.assertGreaterEqual(rows[-1]['detail']['completed'], 1)
        self.assertEqual(terminal.state, 0)

    def test_published_schema_rejects_false_types_phases_and_guarantees(self):
        query, _ = self.report('status', mode='--json')
        for key, value in (('scan_complete', False), ('max_file_bytes', '524288'), ('invented_field', 0)):
            invalid = copy.deepcopy(query[0]); invalid[key] = value
            with self.assertRaises(ValidationError): validate_contract('change', invalid)
        rows, _ = self.report('schema', 'init', 'spike')
        receipt = next(row for row in rows if row.get('phase') == 'publish')
        for key, value in (('applied', 'true'), ('phase', 'execute'), ('detail', -1)):
            invalid = copy.deepcopy(receipt); invalid[key] = value
            with self.assertRaises(ValidationError): validate_contract('change', invalid)
        invalid = copy.deepcopy(rows[-1]); invalid['atomic_batch'] = True
        with self.assertRaises(ValidationError): validate_contract('change', invalid)

    def test_verify_real_child_predicate_status_and_output_isolation(self):
        change = self.seed(complete=True)
        rows, terminal = self.report('verify', 'demo', rc=1)
        self.assertEqual((terminal.state, terminal.errors), (0, 0))
        self.assertFalse(rows[-1]['detail']['ready_to_archive'])
        (change / 'tasks.md').write_text('- [x] done\n')
        rows, terminal = self.report('verify', 'demo')
        self.assertIsNone(rows[-1]['detail']['target'])
        tests = self.work / 'tests'; tests.mkdir()
        script = tests / 'test_suite.sh'
        script.write_text('echo not-json\necho reached > child-marker\nexit 7\n')
        rows, terminal = self.report('verify', 'demo', rc=1)
        self.assertEqual((terminal.state, terminal.errors), (0, 0))
        self.assertEqual(rows[0]['detail']['command_status'], 7)
        self.assertEqual(rows[-1]['detail']['target_status'], 7)
        self.assertEqual((self.work / 'child-marker').read_text(), 'reached\n')
        self.assertIsNone(rows[0]['applied'])
        script.write_text('exit 0\n')
        rows, _ = self.report('verify', 'demo', mode='--json', strict=True)
        self.assertTrue(rows[-1]['detail']['ready_to_archive'])
        script.unlink()
        (self.work / 'Makefile').write_text('all:\n\tprintf build-ran > make-marker\n')
        rows, _ = self.report('verify', 'demo', mode='--data', strict=True)
        self.assertEqual(rows[0]['detail']['command'], 'make')
        self.assertEqual(rows[-1]['detail']['target'], 'make')
        self.assertEqual((self.work / 'make-marker').read_text(), 'build-ran')

    def test_invalid_grammar_and_store_models_fail_without_truncation(self):
        for args in (['schema', 'init'], ['propose', 'a', '--schema', 'x', '--schema', 'y'],
                     ['propose', 'a', '--wat'], ['status', '--limit', '1'], ['propose', '../escape'],
                     ['propose', 'x' * 128], ['status', '--json']):
            rows, terminal = self.report(*args, rc=2)
            self.assertEqual(rows[0]['error'], 'invalid_arguments')
            self.assertEqual(terminal.state, 2)
            self.assertFalse((self.work / '.changes').exists())
        change = self.seed(schema='spike')
        metadata = change / '.change.yaml'; before = metadata.read_bytes()
        metadata.write_bytes(before + b'status: duplicate\n')
        rows, _ = self.report('recall', 'demo', rc=1)
        self.assertEqual(rows[0]['error'], 'invalid_metadata')
        metadata.write_bytes(before)
        schema = self.work / '.changes/schemas/spike/schema.yaml'
        schema.write_bytes(b'name: spike\nartifacts:\n' + b'  - id: x\n    generates: x\n    requires: []\n' * 17)
        rows, _ = self.report('status', rc=1)
        self.assertEqual(rows[0]['error'], 'invalid_schema')

    def test_source_read_close_directory_and_nonregular_failures(self):
        change = self.seed(schema='spike')
        metadata = change / '.change.yaml'; before = metadata.read_bytes()
        for fault in ('source-read', 'source-close'):
            self.fault(fault, file=metadata)
            rows, terminal = self.report('recall', 'demo', rc=1)
            self.assertEqual(rows[0]['error'], 'file_snapshot_failed')
            self.assertEqual(terminal.state, 2)
            self.assertEqual(metadata.read_bytes(), before)
            self.clear_fault()
        self.fault('readdir', directory=self.work / '.changes')
        self.report('status', rc=1); self.clear_fault()
        metadata.unlink(); os.mkfifo(metadata)
        rows, _ = self.report('recall', 'demo', rc=1)
        self.assertEqual(rows[0]['error'], 'file_snapshot_failed')
        metadata.unlink(); outside = self.root / 'outside'; outside.write_bytes(before); metadata.symlink_to(outside)
        self.report('recall', 'demo', rc=1)
        self.assertEqual(outside.read_bytes(), before)

    def test_publication_failures_and_cleanup_receipt_match_actual_files(self):
        for index, mode in enumerate(('write', 'fsync', 'close', 'link', 'directory-fsync', 'cleanup')):
            with self.subTest(fault=mode):
                change = self.seed('case-' + str(index))
                target = change / 'proposal.md'
                self.fault(mode, directory=change)
                rows, terminal = self.report('continue', change.name, rc=1)
                receipt = next(row for row in rows if row['record'] == 'receipt')
                self.assertEqual(receipt['phase'], 'publish')
                self.assertEqual(receipt['applied'], mode == 'directory-fsync')
                self.assertFalse(receipt['durable'])
                self.assertEqual(target.exists(), mode == 'directory-fsync')
                if mode == 'cleanup':
                    self.assertIsNotNone(receipt['cleanup_errno'])
                    self.assertTrue((change / receipt['retained_temporary_name']).exists())
                else: self.assertFalse(list(change.glob('.change-native-*')))
                self.assertEqual(terminal.state, 2)
                self.clear_fault()

    def test_late_metadata_failure_keeps_checkpoint_receipt(self):
        change = self.seed(complete=True)
        before = (change / '.change.yaml').read_bytes()
        self.fault('rename')
        rows, terminal = self.report('apply', 'demo', rc=1)
        self.assertEqual([r['phase'] for r in rows if r['record'] == 'receipt'], ['checkpoint', 'publish'])
        self.assertTrue(rows[0]['applied']); self.assertFalse(rows[1]['applied'])
        self.assertTrue(Path(rows[0]['path']).exists())
        self.assertEqual((change / '.change.yaml').read_bytes(), before)
        self.assertEqual(rows[-1]['applied_receipts'], 1)
        self.assertEqual(terminal.state, 2)

    def test_admission_budget_and_sink_failure_stop_before_next_effect(self):
        change = self.seed()
        request = {'request_version': 2, 'argv': ['change', '--data-v2', 'ff', 'demo'], 'cwd': str(self.work)}
        with open('/dev/full', 'wb') as sink:
            result = subprocess.run([str(f.JASH), '--exec-json', json.dumps(request)], cwd=self.work, env=self.env,
                                    stdout=sink, stderr=subprocess.PIPE)
        self.assertEqual(result.returncode, 125)
        self.assertFalse((change / 'proposal.md').exists())
        self.env['JASH_LLM_BUDGET'] = '32'
        result = self.run_command(['change', '--json', 'ff', 'demo'])
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((change / 'proposal.md').exists())
        self.env.pop('JASH_LLM_BUDGET')
        self.fault('sink-after-publish', file=change / 'proposal.md')
        result = self.run_command(['change', '--data-v2', 'ff', 'demo'])
        self.assertEqual(result.returncode, 125)
        self.assertTrue((change / 'proposal.md').is_file())
        self.assertFalse((change / 'specs').exists())
        self.assertFalse((change / 'design.md').exists())
        with self.assertRaises(AssertionError): f.stream(result.stdout, credit_kind='none')

    def test_prepared_rewind_revalidates_checkpoint_before_any_inverse(self):
        change = self.seed(schema='spike', complete=True)
        self.env['JASH_TIMELINE'] = '1'
        rows, _ = self.report('archive', 'demo')
        checkpoint = Path(next(r['path'] for r in rows if r.get('phase') == 'checkpoint'))
        archived = self.work / rows[-1]['path']
        result = subprocess.run([str(f.JASH), '-c', 'echo later > new-file'], cwd=self.work, env=self.env, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.fault('checkpoint-swap', file=checkpoint)
        rows, terminal = self.report('rewind', archived.name, rc=1)
        self.assertEqual(rows[-1]['error'], 'checkpoint_state_changed')
        self.assertEqual(rows[-1]['receipts'], 0)
        self.assertTrue((self.work / 'new-file').is_file())
        self.assertFalse(change.exists()); self.assertTrue(archived.exists())
        self.assertEqual(terminal.state, 2)


if __name__ == '__main__':
    unittest.main(verbosity=2)
