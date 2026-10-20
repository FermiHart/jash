#!/usr/bin/env python3
"""OSIF reports/scaffolds expose actual observations and publication phases."""
import base64
import copy
import json
import os
from pathlib import Path
import re
import select
import socket
import subprocess
import tempfile
import time
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
#include <sys/utsname.h>
#include <unistd.h>
static int mode(const char* expected) { const char* p=getenv("OSIF_TEST_FAULT"); return p&&!strcmp(p,expected); }
static int descriptor_path(int fd,char path[PATH_MAX+1]) {
    char link[64]; snprintf(link,sizeof link,"/proc/self/fd/%d",fd);
    ssize_t n=readlink(link,path,PATH_MAX); if(n<0) return 0; path[n]=0; return 1;
}
static int selected(int fd,const char* key) {
    char path[PATH_MAX+1]; const char* expected=getenv(key);
    return expected&&descriptor_path(fd,path)&&!strcmp(path,expected);
}
static int temporary(int fd) { char path[PATH_MAX+1]; return descriptor_path(fd,path)&&strstr(path,"/.osif-tmp-"); }
ssize_t write(int fd,const void* bytes,size_t length) {
    static ssize_t (*next)(int,const void*,size_t); if(!next) *(void**)&next=dlsym(RTLD_NEXT,"write");
    if((mode("write")||mode("cleanup"))&&temporary(fd)) { errno=ENOSPC; return -1; }
    const char* target=getenv("OSIF_TEST_FILE");
    if(fd==1&&mode("sink-after-publish")&&target&&!access(target,F_OK)) { errno=EPIPE; return -1; }
    static int interrupted;
    if(mode("short")&&temporary(fd)) {
        if(!interrupted++) { errno=EINTR; return -1; }
        if(length>3) length=3;
    }
    return next(fd,bytes,length);
}
int fsync(int fd) {
    static int (*next)(int); if(!next) *(void**)&next=dlsym(RTLD_NEXT,"fsync");
    if((mode("fsync")&&temporary(fd))||(mode("directory-fsync")&&selected(fd,"OSIF_TEST_DIR"))) { errno=EIO; return -1; }
    return next(fd);
}
int close(int fd) {
    static int (*next)(int); if(!next) *(void**)&next=dlsym(RTLD_NEXT,"close");
    int failed=(mode("close")&&temporary(fd))||(mode("source-close")&&selected(fd,"OSIF_TEST_FILE"))||
        (mode("directory-close")&&selected(fd,"OSIF_TEST_DIR"));
    int result=next(fd); if(failed) { errno=EIO; return -1; } return result;
}
int linkat(int from,const char* source,int to,const char* target,int flags) {
    static int (*next)(int,const char*,int,const char*,int); if(!next) *(void**)&next=dlsym(RTLD_NEXT,"linkat");
    if(mode("link")&&!strncmp(source,".osif-tmp-",10)) { errno=EIO; return -1; }
    return next(from,source,to,target,flags);
}
int renameat(int from,const char* source,int to,const char* target) {
    static int (*next)(int,const char*,int,const char*); if(!next) *(void**)&next=dlsym(RTLD_NEXT,"renameat");
    if(mode("rename")&&!strncmp(source,".osif-tmp-",10)) { errno=EIO; return -1; }
    return next(from,source,to,target);
}
int unlinkat(int parent,const char* name,int flags) {
    static int (*next)(int,const char*,int); if(!next) *(void**)&next=dlsym(RTLD_NEXT,"unlinkat");
    if(mode("cleanup")&&!strncmp(name,".osif-tmp-",10)) { errno=EACCES; return -1; }
    return next(parent,name,flags);
}
int mkdirat(int parent,const char* name,mode_t permissions) {
    static int (*next)(int,const char*,mode_t); if(!next) *(void**)&next=dlsym(RTLD_NEXT,"mkdirat");
    if(mode("mkdir")) { errno=EACCES; return -1; } return next(parent,name,permissions);
}
ssize_t read(int fd,void* data,size_t length) {
    static ssize_t (*next)(int,void*,size_t); if(!next) *(void**)&next=dlsym(RTLD_NEXT,"read");
    if(selected(fd,"OSIF_TEST_FILE")) {
        if(mode("read")) { errno=EIO; return -1; }
        static int changed,interrupted,waiting;
        if(mode("change")&&!changed++) { int out=open(getenv("OSIF_TEST_FILE"),O_WRONLY|O_APPEND); if(out>=0) { (void)write(out,"changed\n",8); close(out); } }
        if(mode("source-short")) { if(!interrupted++) { errno=EINTR; return -1; } if(length>3) length=3; }
        if(mode("hold")&&!waiting++) {
            const char* ready=getenv("OSIF_TEST_READY"); const char* release=getenv("OSIF_TEST_RELEASE");
            if(ready&&release) { char byte='1'; (void)write(atoi(ready),&byte,1); while(next(atoi(release),&byte,1)<0&&errno==EINTR) {} }
        }
    }
    return next(fd,data,length);
}
struct dirent* readdir(DIR* directory) {
    static struct dirent* (*next)(DIR*); if(!next) *(void**)&next=dlsym(RTLD_NEXT,"readdir");
    if(mode("readdir")&&selected(dirfd(directory),"OSIF_TEST_DIR")) { errno=EIO; return NULL; }
    return next(directory);
}
int uname(struct utsname* value) {
    static int (*next)(struct utsname*); if(!next) *(void**)&next=dlsym(RTLD_NEXT,"uname");
    if(mode("uname")) { errno=EIO; return -1; } return next(value);
}
'''


def decoded(value):
    return value.encode() if isinstance(value, str) else base64.b64decode(value['data'], validate=True)


class OsifNative(f.NativeQueryCase):
    @classmethod
    def setUpClass(cls):
        temporary = tempfile.TemporaryDirectory(prefix='jash-osif-faults-')
        cls.addClassCleanup(temporary.cleanup)
        source = Path(temporary.name) / 'faults.c'; source.write_text(FAULT_SOURCE)
        cls.hook = Path(temporary.name) / 'faults.so'
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        str(source), '-o', str(cls.hook), '-ldl'], check=True, capture_output=True)

    def report(self, *arguments, mode='--data-v2', rc=0, strict=False, caps=None):
        result = self.run_command(['osif', mode, *arguments], strict=strict,
                                  caps=caps or ['effect:read', 'effect:mutate', 'effect:exec'])
        if rc is None: self.assertIn(result.returncode, (0, 1), result.stderr + result.stdout[:512])
        else: self.assertEqual(result.returncode, rc, result.stderr + result.stdout[:512])
        if mode == '--json':
            document = json.loads(result.stdout); validate_contract('osif', document)
            self.assertEqual(document['status'], result.returncode)
            rows, terminal = document['items'], None
        else:
            terminal = f.stream(result.stdout, credit_kind='none')
            self.assertEqual(terminal.producer, result.returncode)
            rows = [projection(item) for item in terminal.items]
            for item in terminal.items:
                self.scalar(item, 'record', 'string')
                if 'path' in item.value and item.value['path'].kind != 'nothing': self.scalar(item, 'path', 'path')
                for key in ('passed', 'failed', 'skipped', 'total', 'artifact_bytes', 'source_bytes',
                            'reports_observed', 'effects_applied', 'written_bytes', 'expected_bytes'):
                    if key in item.value: self.scalar(item, key, 'u64')
                if item.value['record'].value == 'summary': self.scalar(item, 'status', 'i64')
        for row in rows: validate_contract('osif-row', row)
        return rows, terminal

    def fault(self, kind, *, file=None, directory=None):
        self.env.update(LD_PRELOAD=str(self.hook), OSIF_TEST_FAULT=kind)
        if file is not None: self.env['OSIF_TEST_FILE'] = os.fsdecode(file)
        if directory is not None: self.env['OSIF_TEST_DIR'] = os.fsdecode(directory)

    def clear_fault(self):
        for key in tuple(self.env):
            if key.startswith('OSIF_TEST_') or key == 'LD_PRELOAD': self.env.pop(key)

    def recorded(self, target='linux', *, passed=8, total=8, failed=0, skipped=0, tier=None, body=b''):
        if tier is None: tier = 'A' if passed == total else 'B' if passed * 4 >= total * 3 else 'C' if passed * 2 >= total else 'D'
        directory = self.work / 'Docs/ports'; directory.mkdir(parents=True, exist_ok=True)
        name = os.fsencode(directory) + b'/' + os.fsencode(target) + b'-conformance.md'
        content = b'# OSIF Conformance \xe2\x80\x94 ' + os.fsencode(target) + b'\n\n'
        content += f'**Tier:** {tier} — {passed}/{total} providers PASS, {failed} FAIL, {skipped} SKIP\n'.encode()
        with open(name, 'wb') as output: output.write(content + body)
        return Path(os.fsdecode(name))

    def test_explanations_share_exact_source_and_are_readonly_in_both_profiles(self):
        for provider in ('sysid', 'proc', 'mem', 'mounts', 'net', 'uptime', 'loadavg', 'spawn'):
            legacy = self.run_command(['osif', 'explain', provider])
            self.assertEqual(legacy.returncode, 0, legacy.stderr)
            source = re.sub(rb'\x1b\[[0-9;]*m', b'', legacy.stdout).split(b'\r\n', 1)[1]
            for strict in (False, True):
                for mode in ('--json', '--data', '--data-v2'):
                    rows, terminal = self.report('explain', provider, strict=strict, mode=mode, caps=['effect:read'])
                    self.assertEqual(len(rows), 1); self.assertEqual(rows[0]['contract'].encode(), source)
                    self.assertEqual(rows[0]['provider'], provider)
                    if terminal: self.assertEqual((terminal.state, terminal.errors), (0, 0))
        self.assertEqual(list(self.work.iterdir()), [])

    def test_report_uses_actual_probe_snapshot_and_publishes_measured_artifact(self):
        legacy = self.run_command(['osif', 'report'])
        self.assertIn(legacy.returncode, (0, 1), legacy.stderr)
        artifact = next((self.work / 'Docs/ports').glob('*-conformance.md'))
        original = artifact.read_bytes()
        self.assertIn(b'osif: wrote ', legacy.stdout)
        self.assertTrue(original.startswith(b'# OSIF Conformance'))
        matrix, _ = self.report('matrix')
        self.assertEqual(matrix[0]['artifact_bytes'], len(original))
        self.fault('write')
        failed = self.run_command(['osif', 'report'])
        self.assertEqual(failed.returncode, 1); self.assertIn(b'file_write_failed', failed.stdout)
        self.assertEqual(artifact.read_bytes(), original)
        self.clear_fault()
        for strict in (False, True):
            for mode in ('--json', '--data', '--data-v2'):
                rows, terminal = self.report('report', mode=mode, strict=strict, caps=['effect:exec'], rc=None)
                probes = [row for row in rows if row['record'] == 'probe']
                model = next(row for row in rows if row['record'] == 'report')
                effects = [row for row in rows if row['record'] == 'effect']
                self.assertEqual(len(probes), 8)
                for field, status in [('passed', 'pass'), ('failed', 'fail'), ('skipped', 'skip')]:
                    self.assertEqual(model[field], sum(row['status'] == status for row in probes))
                self.assertEqual(sum(model[key] for key in ('passed', 'failed', 'skipped')), model['total'])
                artifact = self.work / model['path']; content = artifact.read_bytes()
                self.assertEqual(model['artifact_bytes'], len(content))
                header = f"**Tier:** {model['tier']} — {model['passed']}/{model['total']} providers PASS, {model['failed']} FAIL, {model['skipped']} SKIP"
                self.assertIn(header.encode(), content)
                published = next(row for row in effects if row['phase'] == 'publish')
                self.assertTrue(published['applied'] and published['durable'] and published['ok'])
                self.assertEqual(published['written_bytes'], len(content))
                self.assertTrue(rows[-1]['operation_complete'])
                self.assertEqual(rows[-1]['status'], int(model['failed'] > 0))
                if terminal: self.assertEqual((terminal.state, terminal.errors), (0, 0))
                matrix, _ = self.report('matrix', mode=mode, strict=strict, caps=['effect:read'])
                self.assertEqual(matrix[0]['passed'], model['passed'])
                self.assertEqual(matrix[0]['artifact_bytes'], len(content))

    def test_failed_probe_is_complete_observation_and_unavailable_host_is_explicit(self):
        self.fault('uname')
        rows, terminal = self.report('report', rc=1)
        model = next(row for row in rows if row['record'] == 'report')
        sysid = next(row for row in rows if row.get('capability') == 'SYSID')
        self.assertEqual(sysid['status'], 'fail'); self.assertIn('failed', sysid['detail'])
        self.assertIsNone(model['host'])
        self.assertEqual((terminal.state, terminal.errors), (0, 0))
        self.assertTrue(rows[-1]['operation_complete']); self.assertIsNone(rows[-1]['reason'])
        self.assertIn(b'**Host:** unavailable', (self.work / model['path']).read_bytes())

    def test_scaffold_generates_exact_existing_templates_and_preserves_existing_files(self):
        (self.work / 'c/osif').mkdir(parents=True)
        for strict in (False, True):
            for index, mode in enumerate(('--json', '--data', '--data-v2')):
                target = ('native_' if strict else 'normal_') + str(index)
                rows, terminal = self.report('port', target, mode=mode, strict=strict, caps=['effect:mutate'])
                directory = self.work / 'c/osif' / target
                self.assertEqual({p.name for p in directory.iterdir()}, {'syscall.c', 'osinfo.c', 'AGENT.md'})
                receipts = [row for row in rows if row.get('phase') == 'publish']
                self.assertEqual(len(receipts), 3)
                for row in receipts:
                    data = (self.work / row['path']).read_bytes()
                    self.assertEqual(row['written_bytes'], len(data)); self.assertTrue(row['applied'] and row['durable'])
                    self.assertIn(target.encode(), data)
                self.assertEqual(rows[-1]['effects_applied'], 4)
                if terminal: self.assertEqual(terminal.state, 0)
                saved = directory / 'syscall.c'; saved.write_bytes(b'preserve a real implementation\n')
                rows, _ = self.report('port', target, mode=mode, strict=strict, caps=['effect:mutate'])
                self.assertEqual(saved.read_bytes(), b'preserve a real implementation\n')
                self.assertEqual(rows[-1]['files_preserved'], 3)
                for row in rows[:-1]: self.assertEqual((row['action'], row['applied'], row['durable']), ('preserved', False, None))
        # The legacy adapter calls the same generator/publication engine.
        original = self.work; other = self.root / 'legacy'; (other / 'c/osif').mkdir(parents=True)
        self.work = other
        legacy = self.run_command(['osif', 'port', 'normal_0']); self.assertEqual(legacy.returncode, 0, legacy.stderr)
        self.work = original
        for name in ('osinfo.c', 'AGENT.md'):
            self.assertEqual((original / 'c/osif/normal_0' / name).read_bytes(), (other / 'c/osif/normal_0' / name).read_bytes())
        target = 'x' * 32; self.report('port', target)
        self.assertIn(target.encode(), (self.work / 'c/osif' / target / 'AGENT.md').read_bytes())

    def test_parser_boundaries_and_capabilities_fail_before_effects(self):
        invalid = [('report', 'unexpected'), ('matrix', 'extra'), ('explain',), ('explain', 'proc extra'),
                   ('port', 'two words'), ('port', '../escape'), ('port', 'a'), ('port', 'x' * 33),
                   ('port', 'upperCase'), ('report', '--data-v2'), ('compare',), ('learn', 'proc'),
                   ('explain', '--', '--json')]
        for mode in ('--json', '--data-v2'):
            for arguments in invalid:
                response = self.run_command(['osif', mode, *arguments])
                self.assertEqual(response.returncode, 2, (arguments, response.stderr, response.stdout))
                if mode == '--json': validate_contract('osif', json.loads(response.stdout))
                else:
                    terminal = f.stream(response.stdout, credit_kind='none')
                    self.assertEqual((terminal.state, terminal.producer, terminal.count), (2, 2, 0))
        for operation, operand, caps in [('report', [], ['effect:read']), ('port', ['demo'], ['effect:read'])]:
            response = self.run_command(['osif', '--data-v2', operation, *operand], strict=True, caps=caps)
            self.assertNotEqual(response.returncode, 0)
            self.assertIn(b'cap', response.stderr.lower())
        self.assertEqual(list(self.work.iterdir()), [])
        trailing = self.run_command(['osif', 'explain', 'proc', '--json'], strict=True, caps=['effect:read'])
        self.assertEqual(trailing.returncode, 0, trailing.stderr)
        self.assertEqual(json.loads(trailing.stdout)['items'][0]['provider'], 'proc')

    def test_matrix_parses_recorded_exact_integer_counts_sorted_paths_and_non_utf8(self):
        big = 2**53 + 1
        self.recorded('zlarge', passed=big, total=big)
        self.recorded('alpha', passed=6, total=8, failed=1, skipped=1)
        self.recorded(b'raw-\xff', passed=1, total=8, failed=0, skipped=7)
        directory = self.work / 'Docs/ports'; (directory / 'ignored-conformance.md.bak').write_bytes(b'not a report')
        before = {os.fsencode(path.name): path.read_bytes() for path in directory.iterdir()}
        for strict in (False, True):
            for mode in ('--json', '--data', '--data-v2'):
                rows, terminal = self.report('matrix', mode=mode, strict=strict, caps=['effect:read'])
                reports = rows[:-1]; self.assertEqual(len(reports), 3)
                self.assertEqual([decoded(row['target']) for row in reports], [b'alpha', b'raw-\xff', b'zlarge'])
                self.assertEqual(reports[-1]['passed'], big)
                self.assertEqual(rows[-1]['reports_observed'], 3)
                self.assertEqual(rows[-1]['source_bytes'], sum(len(value) for key, value in before.items() if key.endswith(b'.md')))
                if terminal: self.assertEqual(terminal.state, 0)
        self.assertEqual(before, {os.fsencode(path.name): path.read_bytes() for path in directory.iterdir()})

    def test_matrix_rejects_corrupt_reports_without_inventing_scores(self):
        valid = self.recorded('good')
        for content in (b'', '**Tier:** A — 8/8 providers PASS, 0 FAIL, 0 SKIP\n'.encode(),
                        '# OSIF Conformance — broken\n**Tier:** A — 1/8 providers PASS, 0 FAIL, 7 SKIP\n'.encode(),
                        '# OSIF Conformance — broken\n**Tier:** D — -1/8 providers PASS, 0 FAIL, 9 SKIP\n'.encode(),
                        '# OSIF Conformance — broken\n**Tier:** A — 18446744073709551616/18446744073709551616 providers PASS, 0 FAIL, 0 SKIP\n'.encode(),
                        '# OSIF Conformance — broken\x00\n'.encode(),
                        valid.read_bytes().replace(b'good', b'broken') + '**Tier:** A — 8/8 providers PASS, 0 FAIL, 0 SKIP\n'.encode()):
            (valid.parent / 'broken-conformance.md').write_bytes(content)
            rows, terminal = self.report('matrix', rc=1)
            self.assertEqual([row['target'] for row in rows if row['record'] == 'matrix'], ['good'])
            self.assertEqual(next(row for row in rows if row['record'] == 'error')['error'], 'invalid_report')
            self.assertFalse(rows[-1]['operation_complete']); self.assertEqual(terminal.state, 2)
        rows, _ = self.report('matrix', mode='--json', rc=1)
        self.assertEqual(rows[-1]['failures'], 1)

    def test_matrix_source_types_limits_and_missing_directory_are_explicit(self):
        rows, terminal = self.report('matrix', rc=1)
        self.assertEqual(terminal.state, 2); self.assertFalse((self.work / 'Docs').exists())
        path = self.recorded('source'); path.unlink(); os.mkfifo(path)
        rows, terminal = self.report('matrix', rc=1); self.assertEqual(terminal.state, 2)
        self.assertEqual(next(row for row in rows if row['record'] == 'error')['error'], 'report_not_regular')
        path.unlink(); path.symlink_to('/dev/zero')
        rows, _ = self.report('matrix', rc=1); self.assertEqual(rows[-1]['reports_observed'], 0)
        path.unlink(); path.write_bytes(b'x' * (256 * 1024 + 1))
        rows, terminal = self.report('matrix', rc=75); self.assertEqual(terminal.state, 3)
        self.assertEqual(rows[-1]['reports_observed'], 0)
        self.recorded('source', body=b'x' * 16385 + b'\n')
        rows, terminal = self.report('matrix', rc=75); self.assertEqual(terminal.state, 3)
        self.assertEqual(next(row for row in rows if row['record'] == 'error')['error'], 'report_line_limit')
        path.unlink()
        for index in range(257): self.recorded(f'limit{index:03d}')
        rows, terminal = self.report('matrix', rc=75)
        self.assertEqual(terminal.state, 3); self.assertEqual(rows[-1]['reports_observed'], 256)
        self.assertEqual(rows[-1]['reason'], 'report_count_limit')

    def test_matrix_total_source_limit_is_measured_across_files(self):
        body = b'# filler\n' * 28000
        for index in range(67): self.recorded(f'large{index:02d}', body=body)
        rows, terminal = self.report('matrix', rc=75)
        self.assertEqual(terminal.state, 3)
        self.assertGreater(rows[-1]['source_bytes'], 15 * 1024 * 1024)
        self.assertLessEqual(rows[-1]['source_bytes'], 16 * 1024 * 1024)
        self.assertEqual(next(row for row in rows if row.get('error') == 'source_byte_limit')['status'], 75)

    def test_checked_matrix_reads_preserve_io_errors_and_accept_short_reads(self):
        path = self.recorded('linux'); original = path.read_bytes()
        for fault, error in [('read', 'report_read_failed'), ('source-close', 'report_close_failed'),
                             ('change', 'report_changed'), ('readdir', 'directory_read_failed')]:
            path.write_bytes(original); self.fault(fault, file=path, directory=path.parent)
            rows, terminal = self.report('matrix', rc=1)
            self.assertEqual(terminal.state, 2)
            self.assertTrue(any(row.get('error') == error for row in rows), rows)
            self.assertEqual(rows[-1]['reports_observed'], 0)
            self.clear_fault()
        path.write_bytes(original); self.fault('source-short', file=path)
        rows, terminal = self.report('matrix'); self.assertEqual(terminal.state, 0)
        self.assertEqual(rows[-1]['source_bytes'], len(original))
        self.clear_fault(); self.fault('directory-close', directory=path.parent)
        rows, terminal = self.report('matrix', rc=1)
        self.assertEqual(terminal.state, 2); self.assertEqual(rows[-1]['reports_observed'], 1)
        self.assertTrue(any(row.get('error') == 'directory_close_failed' for row in rows))

    def test_scaffold_symlinks_missing_parents_and_mkdir_failure_preserve_outside_state(self):
        rows, terminal = self.report('port', 'demo', rc=1)
        self.assertEqual(terminal.state, 2); self.assertFalse((self.work / 'c').exists())
        parent = self.work / 'c/osif'; parent.mkdir(parents=True)
        outside = self.root / 'outside'; outside.mkdir(); (parent / 'demo').symlink_to(outside)
        self.report('port', 'demo', rc=1); self.assertEqual(list(outside.iterdir()), [])
        (parent / 'demo').unlink(); (parent / 'demo').mkdir()
        target = parent / 'demo/syscall.c'; target.symlink_to(outside / 'sensitive')
        self.report('port', 'demo', rc=1); self.assertFalse((outside / 'sensitive').exists())
        self.assertFalse((parent / 'demo/osinfo.c').exists())
        self.fault('mkdir'); rows, _ = self.report('port', 'new_target', rc=1)
        self.assertEqual(rows[-1]['effects_applied'], 0); self.assertFalse((parent / 'new_target').exists())

    def test_publication_faults_report_actual_applied_and_durable_phases(self):
        parent = self.work / 'c/osif'; parent.mkdir(parents=True)
        for fault, error in [('write', 'file_write_failed'), ('fsync', 'file_sync_failed'), ('close', 'file_close_failed'),
                             ('link', 'file_publish_failed'), ('directory-fsync', 'directory_sync_failed'), ('cleanup', 'file_write_failed')]:
            target = parent / fault.replace('-', '_'); target.mkdir()
            self.fault(fault, directory=target)
            rows, terminal = self.report('port', target.name, rc=1)
            receipt = next(row for row in rows if row.get('phase') == 'publish')
            self.assertEqual(receipt['error'], error); self.assertEqual(terminal.state, 2)
            self.assertEqual(receipt['applied'], fault == 'directory-fsync')
            self.assertFalse(receipt['durable']); self.assertFalse((target / 'osinfo.c').exists())
            self.assertEqual((target / 'syscall.c').exists(), receipt['applied'])
            if fault == 'cleanup':
                self.assertTrue((self.work / receipt['temporary_retained']).exists()); self.assertGreater(receipt['cleanup_errno'], 0)
            else: self.assertEqual(list(target.glob('.osif-tmp-*')), [])
            self.clear_fault()
        self.fault('short'); rows, _ = self.report('port', 'short_writes')
        self.assertTrue(rows[-1]['operation_complete']); self.clear_fault()
        target = parent / 'close_directory'; target.mkdir()
        self.fault('directory-close', directory=target)
        rows, terminal = self.report('port', target.name, rc=1)
        self.assertEqual(terminal.state, 2)
        self.assertTrue(all(row['durable'] for row in rows if row.get('phase') == 'publish'))
        self.assertTrue(any(row.get('error') == 'directory_close_failed' for row in rows))
        self.clear_fault()
        directory = self.work / 'Docs/ports'; directory.mkdir(parents=True)
        existing = directory / 'linux-conformance.md'; existing.write_bytes(b'previous report\n')
        for fault in ('write', 'fsync', 'close', 'rename'):
            self.fault(fault, directory=directory)
            rows, _ = self.report('report', mode='--json', rc=1)
            receipt = next(row for row in rows if row.get('phase') == 'publish')
            self.assertFalse(receipt['applied']); self.assertEqual(existing.read_bytes(), b'previous report\n')
            self.clear_fault()
        self.fault('directory-fsync', directory=directory)
        rows, _ = self.report('report', mode='--json', rc=1)
        receipt = next(row for row in rows if row.get('phase') == 'publish')
        self.assertTrue(receipt['applied']); self.assertFalse(receipt['durable'])
        self.assertTrue(existing.read_bytes().startswith(b'# OSIF Conformance'))

    def test_delivery_admission_and_failure_after_first_publication_stop_later_effects(self):
        (self.work / 'c/osif').mkdir(parents=True)
        for operation in ('report', 'port'):
            for mode in ('--json', '--data-v2'):
                arguments = ['osif', mode, operation] + (['noeffect'] if operation == 'port' else [])
                with open('/dev/full', 'wb') as sink:
                    result = subprocess.run([str(f.JASH), '--exec-json', json.dumps({'request_version': 2, 'argv': arguments})],
                                            cwd=self.work, env=self.env, stdout=sink, stderr=subprocess.PIPE, timeout=20)
                self.assertEqual(result.returncode, 125, result.stderr)
                self.assertFalse((self.work / 'Docs').exists()); self.assertFalse((self.work / 'c/osif/noeffect').exists())
        for mode in ('--json', '--data-v2'):
            target = self.work / ('c/osif/json_sink' if mode == '--json' else 'c/osif/typed_sink')
            self.fault('sink-after-publish', file=target / 'syscall.c')
            result = self.run_command(['osif', mode, 'port', target.name])
            self.assertEqual(result.returncode, 125, result.stderr)
            self.assertTrue((target / 'syscall.c').is_file()); self.assertFalse((target / 'osinfo.c').exists())
            self.clear_fault()
        self.env['JASH_LLM_BUDGET'] = '1'
        result = self.run_command(['osif', '--json', 'explain', 'proc'])
        self.assertEqual(result.returncode, 75); self.assertEqual(json.loads(result.stdout)['error']['code'], 'output_budget_exceeded')
        self.report('port', 'budget_receipts', mode='--json')

    def test_matrix_cancel_is_observed_at_real_read_boundary_and_session_recovers(self):
        path = self.recorded('linux')
        control, child = socket.socketpair(); ready, notify = os.pipe(); release, release_writer = os.pipe()
        env = dict(self.env, LD_PRELOAD=str(self.hook), OSIF_TEST_FAULT='hold', OSIF_TEST_FILE=str(path),
                   OSIF_TEST_READY=str(notify), OSIF_TEST_RELEASE=str(release), JASH_AGENT_CANCEL_FD=str(child.fileno()))
        process = subprocess.Popen([str(f.AGENT)], cwd=self.work, env=env, pass_fds=(child.fileno(), notify, release),
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        child.close(); os.close(notify); os.close(release); pending = {}
        def line(stream):
            fd = stream.fileno(); data = pending.pop(fd, b''); deadline = time.monotonic() + 10
            while b'\n' not in data:
                self.assertTrue(select.select([fd], [], [], max(0, deadline - time.monotonic()))[0], data)
                part = os.read(fd, 65536); self.assertTrue(part, data); data += part
            row, _, rest = data.partition(b'\n'); pending[fd] = rest; return json.loads(row)
        try:
            self.assertTrue(line(process.stdout)['cancel']['enabled'])
            request = {'id': 'osif-cancel', 'protocol_version': 2, 'argv': ['osif', '--data-v2', 'matrix'],
                       'caps': ['effect:read'], 'cancelable': True}
            process.stdin.write((json.dumps(request) + '\n').encode()); self.assertEqual(line(process.stdout)['event'], 'start')
            self.assertTrue(select.select([ready], [], [], 10)[0]); self.assertEqual(os.read(ready, 1), b'1')
            control.sendall(b'{"cancel":"osif-cancel"}\n'); os.write(release_writer, b'1')
            self.assertEqual(line(control), {'cancel': 'osif-cancel', 'accepted': True})
            chunks = []
            while True:
                row = line(process.stdout)
                if row.get('event') != 'chunk': break
                if row['stream'] == 'stdout': chunks.append(base64.b64decode(row['data_b64']))
            self.assertEqual(row['rc'], 130); self.assertTrue(row['cancelled'])
            terminal = f.stream(b''.join(chunks), credit_kind='none')
            self.assertEqual((terminal.state, terminal.producer, terminal.reason), (1, 130, 'request_cancelled'))
            summary = projection(terminal.items[-1]); validate_contract('osif-row', summary)
            self.assertFalse(summary['operation_complete']); self.assertEqual(summary['reports_observed'], 0)
            request.update(id='osif-after', argv=['osif', '--data-v2', 'explain', 'proc']); request.pop('cancelable')
            process.stdin.write((json.dumps(request) + '\n').encode()); self.assertEqual(line(process.stdout)['event'], 'start')
            chunks = []
            while True:
                row = line(process.stdout)
                if row.get('event') != 'chunk': break
                if row['stream'] == 'stdout': chunks.append(base64.b64decode(row['data_b64']))
            self.assertEqual(row['rc'], 0); self.assertEqual(f.stream(b''.join(chunks), credit_kind='none').state, 0)
        finally:
            control.close(); os.close(ready); os.close(release_writer)
            if process.poll() is None: process.kill()
            process.communicate(timeout=10)

    def test_schema_rejects_type_loss_and_false_durability(self):
        self.recorded('linux'); rows, _ = self.report('matrix')
        invalid = copy.deepcopy(rows[0]); invalid['passed'] = '8'
        with self.assertRaises(ValidationError): validate_contract('osif-row', invalid)
        invalid = copy.deepcopy(rows[-1]); invalid['reason'] = 'source_failed'
        with self.assertRaises(ValidationError): validate_contract('osif-row', invalid)
        (self.work / 'c/osif').mkdir(parents=True); rows, _ = self.report('port', 'demo')
        invalid = copy.deepcopy(next(row for row in rows if row.get('phase') == 'publish'))
        invalid['applied'] = False
        with self.assertRaises(ValidationError): validate_contract('osif-row', invalid)


if __name__ == '__main__': unittest.main()
