#!/usr/bin/env python3
"""Actual copied-tree effects, observations, checked cleanup and native contracts."""
import base64
import copy
import errno
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import unittest

from jsonschema import Draft202012Validator
import test_management_builtins as management
from test_discovery_builtins import native_stream, project


class SpeculateNative(unittest.TestCase):
    run_cli = management.ManagementBuiltins.run_cli
    execute = management.ManagementBuiltins.execute
    compile_library = management.ManagementBuiltins.compile_library

    def setUp(self):
        management.ManagementBuiltins.setUp(self)
        self.validators = {}
        for kind, name in [('document', 'v1'), ('row', 'row-v1')]:
            schema = json.loads((management.ROOT / 'schemas' / ('builtin-speculate-' + name + '.schema.json')).read_text())
            Draft202012Validator.check_schema(schema)
            self.validators[kind] = Draft202012Validator(schema)

    def validate(self, value, kind='document'):
        self.validators[kind].validate(value)
        return value

    def report(self, *args, status=0):
        result = self.execute(['speculate', '--json', *args], status=status)
        return self.validate(json.loads(result.stdout)), result

    def native(self, *args, status=0, failed=False, selector='--data-v2'):
        result = self.execute(['speculate', selector, *args], status=status)
        rows, terminal = native_stream(result.stdout)
        self.assertEqual((terminal['state'], terminal['completeness'], terminal['status'], terminal['errors']),
                         (2 if failed else 0, 2 if failed else 1, status, int(failed)))
        self.assertEqual(terminal['logical_bytes'], 0)
        projected = [self.validate(project(row), 'row') for row in rows]
        inspected = self.run_cli('--exec-json', json.dumps({'protocol_version': 2, 'argv': ['json', '--inspect-data']}), input=result.stdout)
        projection = json.loads(inspected.stdout)
        self.assertTrue(projection['terminal']['hash_verified'])
        self.assertEqual(projection['items'], projected)
        return projected, rows, terminal, result

    @staticmethod
    def decoded(value):
        return base64.b64decode(value['data']) if isinstance(value, dict) else value.encode()

    def clean_owned_retention(self, cleanup):
        self.assertTrue(cleanup['retained_path_verified'])
        path = Path(cleanup['root_path'])
        self.assertEqual(path.parent, Path('/tmp'))
        self.assertTrue(path.name.startswith(('jash-speculate-native-', '.jspec.')), path)
        self.addCleanup(shutil.rmtree, path, True)
        return path

    def effect_command(self):
        self.env['SPEC_EFFECT_MARKER'] = str(self.directory / 'outside-effect')
        return ['python3', '-c', 'import os; open(os.environ["SPEC_EFFECT_MARKER"],"w").write("executed")']

    def test_json_diff_is_real_and_original_tree_is_unchanged(self):
        (self.directory / 'modify').write_bytes(b'before')
        (self.directory / 'delete').write_bytes(b'delete')
        (self.directory / 'retype').write_bytes(b'old type')
        (self.directory / 'mode').write_bytes(b'same bytes')
        (self.directory / 'mode').chmod(0o600)
        program = ('import os; open("modify","wb").write(b"after"); os.unlink("delete"); '
                   'os.unlink("retype"); os.mkdir("retype"); os.chmod("mode",0o644); '
                   'os.mkdir("build"); open("build/new file","w").write("new"); '
                   'os.symlink("modify","new link"); print("observed output")')
        value, result = self.report('python3', '-c', program)
        changes = {item['path']: item for item in value['changes']}
        self.assertEqual({key: row['change'] for key, row in changes.items()}, {
            'build': 'created', 'build/new file': 'created', 'delete': 'deleted',
            'mode': 'modified', 'modify': 'modified', 'new link': 'created', 'retype': 'type_changed'})
        self.assertNotEqual(changes['modify']['before']['blake3'], changes['modify']['after']['blake3'])
        self.assertEqual(changes['mode']['before']['blake3'], changes['mode']['after']['blake3'])
        self.assertEqual(changes['new link']['after']['link_target'], 'modify')
        self.assertEqual(value['modified'], ['mode', 'modify', 'retype'])
        self.assertEqual((value['created_count'], value['modified_count'], value['deleted_count']), (3, 3, 1))
        self.assertEqual(value['summary']['type_changed_count'], 1)
        self.assertEqual((value['rc'], value['stdout'], value['network']), (0, 'observed output\n', None))
        self.assertEqual(result.stderr, b'')
        self.assertEqual((value['cleanup'], value['sandbox']), ('removed', None))
        self.assertFalse(Path(value['summary']['cleanup']['candidate_path']).exists())
        self.assertEqual((self.directory / 'modify').read_bytes(), b'before')
        self.assertTrue((self.directory / 'delete').is_file())
        self.assertTrue((self.directory / 'retype').is_file())
        self.assertEqual((self.directory / 'mode').stat().st_mode & 0o777, 0o600)
        self.assertFalse((self.directory / 'build').exists())

    def test_native_aliases_keep_paths_filesizes_and_complete_nonzero_status(self):
        for selector in ('--data', '--data-v2'):
            with self.subTest(selector=selector):
                rows, native, terminal, _ = self.native('sh', '-c', 'printf abc; printf error >&2; exit 7', status=7, selector=selector)
                self.assertEqual([row['kind'] for row in rows], ['plan', 'copy', 'execution', 'summary'])
                self.assertEqual(native[0][1]['cwd'][0], 9)
                self.assertEqual(native[1][1]['bytes_written'][0], 4)
                self.assertEqual(native[2][1]['stdout'][1]['bytes'][0], 8)
                self.assertEqual(rows[2]['command_status'], 7)
                self.assertTrue(rows[2]['complete']); self.assertTrue(rows[-1]['complete'])
                self.assertEqual(terminal['errors'], 0)
                self.assertEqual(self.decoded(rows[2]['stdout']['bytes']), b'abc')
                self.assertTrue(rows[2]['quiescent']); self.assertTrue(rows[2]['descendants_quiescent'])
                self.assertIsNone(rows[2]['cleanup_errno'])

    def test_argv_bytes_spaces_selector_position_and_literal_suffix(self):
        program = 'import os,sys; os.write(1,b"|".join(os.fsencode(a) for a in sys.argv[1:])); open(sys.argv[1],"w").write("x")'
        args = ['speculate', '--keep', '--capture-bytes', '128', '--json', '--',
                'python3', '-c', program, b'name \xff', '--json']
        value = self.validate(json.loads(self.execute(args).stdout))
        path = self.clean_owned_retention(value['summary']['cleanup'])
        self.assertEqual(self.decoded(value['stdout']), b'name \xff|--json')
        self.assertEqual(self.decoded(value['plan']['argv'][-2]), b'name \xff')
        self.assertEqual(self.decoded(value['created'][0]), b'name \xff')
        self.assertEqual(os.listdir(os.fsencode(path / 'tree')).count(b'name \xff'), 1)
        self.assertEqual(value['cleanup'], 'kept')
        self.assertEqual(value['sandbox'], str(path / 'tree'))

    def test_omissions_are_explicit_and_no_fifo_or_symlink_is_followed(self):
        outside = self.directory / 'external'
        outside.write_text('untouched')
        os.symlink(outside, self.directory / 'link')
        os.mkfifo(self.directory / 'fifo')
        (self.directory / 'node_modules').mkdir()
        (self.directory / 'node_modules/ignored').write_text('excluded')
        value, _ = self.report('sh', '-c', 'test ! -e link && test ! -e fifo && test ! -e node_modules')
        omissions = {row['path']: row['reason'] for row in value['plan']['omissions']}
        self.assertEqual(omissions['link'], 'symlink_not_copied')
        self.assertEqual(omissions['fifo'], 'nonregular_not_copied')
        self.assertEqual(omissions['node_modules'], 'configured_name_exclusion')
        self.assertEqual(value['created_count'], 0)
        self.assertEqual(outside.read_text(), 'untouched')
        self.assertFalse(value['plan']['isolation']['filesystem_namespace'])
        self.assertIsNone(value['plan']['isolation']['network'])

    def test_pre_execution_file_and_inventory_bounds_do_not_run_command(self):
        command = self.effect_command()
        huge = self.directory / 'huge'
        with huge.open('wb') as output:
            output.truncate(16 * 1024 * 1024 + 1)
        value, _ = self.report(*command, status=75)
        self.assertEqual((value['kind'], value['error'], value['errno']), ('summary', 'source_inventory_failed', errno.E2BIG))
        self.assertEqual(value['cleanup']['state'], 'not_created')
        self.assertIsNone(value['command_status']); self.assertIsNone(value['changes_count'])
        huge.unlink()
        for index in range(4097):
            (self.directory / ('entry-' + str(index))).touch()
        rows, _, _, _ = self.native(*command, status=75, failed=True)
        self.assertEqual(rows[0]['error'], 'source_inventory_failed')
        self.assertEqual(rows[0]['cleanup']['state'], 'not_created')
        self.assertFalse((self.directory / 'outside-effect').exists())

    def test_diff_bound_reports_unknown_counts_after_real_execution(self):
        program = 'import os; [open("new-"+str(n),"w").close() for n in range(1025)]'
        rows, _, _, _ = self.native('python3', '-c', program, status=75, failed=True)
        self.assertEqual(rows[2]['command_status'], 0)
        self.assertTrue(rows[2]['complete'])
        self.assertEqual(rows[-1]['error'], 'copy_diff_failed')
        self.assertFalse(rows[-1]['changes_complete']); self.assertIsNone(rows[-1]['changes_count'])
        self.assertEqual(rows[-1]['cleanup']['state'], 'removed')
        self.assertFalse((self.directory / 'new-0').exists())

    def test_binary_output_preview_and_quiet_observation(self):
        program = 'import os; os.write(1,b"a\\x00\\xffbcdef"); os.write(2,b"error")'
        value, _ = self.report('--capture-bytes', '3', 'python3', '-c', program)
        self.assertEqual(self.decoded(value['stdout']), b'a\x00\xff')
        stream = value['execution']['stdout']
        self.assertEqual((stream['retained_bytes'], stream['observed_bytes'], stream['truncated']), (3, 8, True))
        self.assertTrue(value['summary']['complete'])
        quiet, _, _, _ = self.native('--quiet', 'python3', '-c', program)
        stream = quiet[2]['stdout']
        self.assertTrue(stream['observed']); self.assertTrue(stream['omitted_by_quiet'])
        self.assertEqual((stream['bytes'], stream['retained_bytes'], stream['observed_bytes']), (None, 0, 8))

    def test_timeout_and_successful_leader_leave_no_live_descendants(self):
        command = self.effect_command()
        script = 'sleep 2; ' + 'python3 -c \'import os; open(os.environ["SPEC_EFFECT_MARKER"],"w").write("late")\''
        start = time.monotonic()
        rows, _, _, _ = self.native('--timeout', '1', 'sh', '-c', script, status=75, failed=True)
        self.assertLess(time.monotonic() - start, 5)
        self.assertTrue(rows[2]['timed_out']); self.assertTrue(rows[2]['quiescent'])
        self.assertTrue(rows[2]['descendants_quiescent']); self.assertEqual(rows[-1]['cleanup']['state'], 'removed')
        rows, _, _, _ = self.native('sh', '-c', '(' + script + ') & exit 0', status=1, failed=True)
        self.assertFalse(rows[2]['complete']); self.assertTrue(rows[2]['descendants_quiescent'])
        self.assertTrue(rows[2]['descendants_forced'] or rows[2]['quiescence_forced'])
        time.sleep(2.1)
        self.assertFalse((self.directory / 'outside-effect').exists())

    def test_supervisor_output_limit_is_failed_observation_with_real_cleanup(self):
        program = 'import os; [os.write(1,b"x"*65536) for _ in range(128)]'
        rows, _, _, _ = self.native('--capture-bytes', '64', 'python3', '-c', program, status=75, failed=True)
        self.assertTrue(rows[2]['output_limit_hit'])
        self.assertFalse(rows[2]['complete']); self.assertFalse(rows[-1]['complete'])
        self.assertEqual(rows[-1]['cleanup']['state'], 'removed')
        self.assertEqual(rows[2]['stdout']['retained_bytes'], 64)
        self.assertTrue(rows[0]['limits']['spool_overshoot_possible'])

    def test_admission_budget_sink_and_invalid_arguments_precede_effect(self):
        command = self.effect_command()
        for selector in ('--json', '--data', '--data-v2'):
            with open('/dev/full', 'wb') as full:
                self.execute(['speculate', selector, *command], status=125, stdout=full)
            self.assertFalse((self.directory / 'outside-effect').exists())
        self.env['JASH_LLM_BUDGET'] = '4096'
        value, _ = self.report(*command, status=75)
        self.assertEqual(value['error'], 'receipt_admission_failed')
        self.assertFalse((self.directory / 'outside-effect').exists())
        rows, _, _, _ = self.native('true')
        self.assertTrue(rows[-1]['complete'])
        del self.env['JASH_LLM_BUDGET']
        invalid = [[], ['--timeout', '0', *command], ['--timeout', '1x', *command],
                   ['--capture-bytes', '262145', *command], ['--json', *command],
                   ['--data', *command], ['--quiet', '--quiet', *command],
                   ['touch', '../outside'], ['touch', '/tmp/outside']]
        for args in invalid:
            with self.subTest(args=args):
                value, _ = self.report(*args, status=2)
                self.assertFalse(value['ok'])
        self.assertFalse((self.directory / 'outside-effect').exists())

    def fault_library(self):
        return self.compile_library('speculate-faults', r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int copied;
static int fdpath(int fd, char *out, size_t size) {
    char name[64]; snprintf(name,sizeof name,"/proc/self/fd/%d",fd);
    ssize_t n=readlink(name,out,size-1); if(n<0)return -1;out[n]=0;return 0;
}
ssize_t read(int fd,void *buf,size_t size) {
    static ssize_t(*real)(int,void*,size_t);if(!real)real=dlsym(RTLD_NEXT,"read");
    const char *mode=getenv("SPEC_FAULT"),*source=getenv("SPEC_FAULT_SOURCE"); char path[4096];
    if(mode&&source&&!strcmp(mode,"copy-read")&&!fdpath(fd,path,sizeof path)&&!strcmp(path,source)){errno=EIO;return -1;}
    return real(fd,buf,size);
}
ssize_t write(int fd,const void *buf,size_t size) {
    static ssize_t(*real)(int,const void*,size_t);if(!real)real=dlsym(RTLD_NEXT,"write");
    const char *mode=getenv("SPEC_FAULT");char path[4096];
    if(mode&&!strcmp(mode,"copy-receipt")) {
        if(fd==1&&copied){errno=EPIPE;return -1;}
        if(fd!=1&&!fdpath(fd,path,sizeof path)&&strstr(path,"/jash-speculate-native-")&&strstr(path,"/tree/seed"))copied=1;
    }
    return real(fd,buf,size);
}
int unlinkat(int fd,const char *name,int flags) {
    static int(*real)(int,const char*,int);if(!real)real=dlsym(RTLD_NEXT,"unlinkat");
    const char *mode=getenv("SPEC_FAULT");char path[4096];
    if(mode&&!strcmp(mode,"cleanup")&&!fdpath(fd,path,sizeof path)&&!strncmp(path,"/tmp/.jspec.",12)){errno=EACCES;return -1;}
    return real(fd,name,flags);
}
''')

    def test_copy_read_failure_and_mid_phase_sink_failure_never_execute(self):
        (self.directory / 'seed').write_text('seed')
        command = self.effect_command()
        self.env['LD_PRELOAD'] = str(self.fault_library())
        self.env['SPEC_FAULT_SOURCE'] = str(self.directory / 'seed')
        self.env['SPEC_FAULT'] = 'copy-read'
        value, _ = self.report(*command, status=1)
        self.assertFalse(value['copy']['complete']); self.assertEqual(value['copy']['errno'], errno.EIO)
        self.assertIsNone(value['execution']); self.assertIsNone(value['rc'])
        self.assertEqual(value['cleanup'], 'removed')
        self.assertFalse((self.directory / 'outside-effect').exists())
        self.env['SPEC_FAULT'] = 'copy-receipt'
        for selector in ('--json', '--data-v2'):
            result = self.execute(['speculate', selector, *command], status=125)
            self.assertGreater(len(result.stdout), 0)
            self.assertFalse((self.directory / 'outside-effect').exists())
            if selector == '--json':
                with self.assertRaises(json.JSONDecodeError):
                    json.loads(result.stdout)
            else:
                with self.assertRaises(AssertionError):
                    native_stream(result.stdout)

    def test_cleanup_failure_retains_only_verified_owned_tree(self):
        (self.directory / 'seed').write_text('seed')
        self.env['LD_PRELOAD'] = str(self.fault_library())
        self.env['SPEC_FAULT'] = 'cleanup'
        value, _ = self.report('true', status=1)
        self.assertTrue(value['execution']['complete']); self.assertTrue(value['summary']['changes_complete'])
        self.assertFalse(value['summary']['complete']); self.assertEqual(value['summary']['error'], 'cleanup_failed')
        cleanup = value['summary']['cleanup']
        self.assertEqual((cleanup['state'], cleanup['errno']), ('retained', errno.EACCES))
        path = self.clean_owned_retention(cleanup)
        self.assertTrue(path.is_dir())

    def test_replacement_namespace_is_preserved_and_not_claimed_as_owned(self):
        moved = self.directory / 'moved-workspace'
        self.env['SPEC_MOVED'] = str(moved)
        program = ('import os; root=os.path.dirname(os.getcwd()); os.rename(root,os.environ["SPEC_MOVED"]); '
                   'os.mkdir(root); open(os.path.join(root,"replacement-sentinel"),"w").write("foreign")')
        value, _ = self.report('python3', '-c', program, status=1)
        cleanup = value['summary']['cleanup']
        replacement = Path(cleanup['candidate_path'])
        self.assertEqual(replacement.parent, Path('/tmp'))
        self.assertTrue(replacement.name.startswith('jash-speculate-native-'))
        self.addCleanup(shutil.rmtree, replacement, True)
        self.assertEqual((replacement / 'replacement-sentinel').read_text(), 'foreign')
        self.assertTrue(moved.is_dir())
        self.assertEqual(cleanup['state'], 'retained')
        self.assertFalse(cleanup['retained_path_verified']); self.assertIsNone(cleanup['root_path'])
        self.assertIsNone(cleanup['tree_path']); self.assertIsNone(value['sandbox'])
        self.assertEqual(cleanup['errno'], errno.ESTALE)

    def test_schema_rejects_invented_isolation_and_completion(self):
        value, _ = self.report('true')
        cases = []
        invalid = copy.deepcopy(value); invalid['network'] = False; cases.append(invalid)
        invalid = copy.deepcopy(value); invalid['plan']['isolation']['filesystem_namespace'] = True; cases.append(invalid)
        invalid = copy.deepcopy(value); invalid['execution']['cleanup_errno'] = errno.EIO; cases.append(invalid)
        invalid = copy.deepcopy(value); invalid['summary']['changes_count'] = None; cases.append(invalid)
        invalid = copy.deepcopy(value); invalid['summary']['cleanup']['root_path'] = '/tmp/unverified'; cases.append(invalid)
        for invalid in cases:
            self.assertTrue(list(self.validators['document'].iter_errors(invalid)))
        bad_row = copy.deepcopy(value['execution']); bad_row['descendants_quiescent'] = False
        self.assertTrue(list(self.validators['row'].iter_errors(bad_row)))


if __name__ == '__main__':
    unittest.main()
