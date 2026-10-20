#!/usr/bin/env python3
"""State predicates, bounded command captures and atomic snapshot observations."""
import base64
import copy
import errno
import json
import os
import shlex
import subprocess
import sys
import threading
import unittest
from jsonschema import Draft202012Validator
import test_management_builtins as management
import test_history_native as history
from test_discovery_builtins import native_stream, project


class ExpectNative(unittest.TestCase):
    run_cli = management.ManagementBuiltins.run_cli
    execute = management.ManagementBuiltins.execute
    compile_library = management.ManagementBuiltins.compile_library
    chain = history.HistoryNative.chain

    def setUp(self):
        management.ManagementBuiltins.setUp(self)
        self.python_fixture_count = 0
        schema = json.loads((management.ROOT / 'schemas/builtin-expect-v1.schema.json').read_text())
        Draft202012Validator.check_schema(schema)
        self.validator = Draft202012Validator(schema)

    def validate(self, value):
        self.validator.validate(value)
        return value

    def report(self, *args, status=0):
        process = self.execute(['expect', '--json', *args], status=status)
        return [self.validate(json.loads(row)) for row in process.stdout.splitlines() if row.strip()], process

    def native(self, *args, status=0, failed=False, selector='--data-v2'):
        process = self.execute(['expect', selector, *args], status=status)
        rows, terminal = native_stream(process.stdout)
        self.assertEqual((terminal['state'], terminal['status'], terminal['errors']),
                         (2 if failed else 0, status, int(failed)))
        self.assertEqual(terminal['logical_bytes'], 0)
        self.assertEqual(terminal['completeness'], 2 if failed else 1)
        return [self.validate(project(row)) for row in rows], terminal, process

    def python(self, source):
        # Exact-byte producers run ordinary private scripts.  This preserves
        # the default agent guard and avoids path-dependent treatment of -c.
        path = self.directory / ('expect-command-%d.py' % self.python_fixture_count)
        self.python_fixture_count += 1
        path.write_text(source + '\n', encoding='utf-8')
        return shlex.join([sys.executable, str(path)])

    def test_agent_guard_denial_preserves_status_and_prevents_fixture_effect(self):
        self.env['JASH_GUARD_BYPASS_STRICT'] = '1'
        command = '/usr/bin/python3 -c ' + shlex.quote(
            'from pathlib import Path;Path("guard-effect").write_text("unexpected")')
        denied = self.run_cli('-c', command, status=126)
        self.assertIn(b'[agent-guard] BLOCKED', denied.stderr)
        self.assertFalse((self.directory / 'guard-effect').exists())
        for selector in ('--json', '--data-v2'):
            with self.subTest(selector=selector):
                value = (self.report('cmd', command, 'rc', '126')[0][-1]
                         if selector == '--json'
                         else self.native('cmd', command, 'rc', '126')[0][-1])
                self.assertEqual(value['execution']['command_status'], 126)
                self.assertTrue(value['execution']['complete'])
                self.assertTrue(value['ok'])
                self.assertIn(b'[agent-guard] BLOCKED',
                              base64.b64decode(value['execution']['stdout']['data']))
                self.assertFalse((self.directory / 'guard-effect').exists())

    def test_simple_external_subshell_preserves_parent_bytes_effect_and_terminal(self):
        fixture = self.directory / 'expect-external-fixture.sh'
        fixture.write_text("printf 'O\\000\\377'\nprintf 'E\\000\\376' >&2\n"
                           "printf applied >external-effect\nexit 7\n")
        # Deliberately unquoted simple argv covers the former direct-exec path.
        command = '/bin/sh expect-external-fixture.sh'
        effect = self.directory / 'external-effect'
        text = self.run_cli('-c', '( ' + command + ' ); printf "|parent=%s|" "$?"')
        self.assertEqual(text.stdout, b'O\0\xff|parent=7|')
        self.assertEqual(text.stderr, b'E\0\xfe')
        self.assertEqual(effect.read_bytes(), b'applied')
        for selector in ('--json', '--data-v2'):
            with self.subTest(selector=selector):
                effect.unlink()
                value = (self.report('cmd', command, 'rc', '7')[0][-1]
                         if selector == '--json'
                         else self.native('cmd', command, 'rc', '7')[0][-1])
                self.assertTrue(value['complete'])
                self.assertTrue(value['ok'])
                self.assertEqual(value['execution']['command_status'], 7)
                self.assertEqual(base64.b64decode(value['execution']['stdout']['data']),
                                 b'O\0\xffE\0\xfe')
                self.assertEqual(effect.read_bytes(), b'applied')

    def test_capture_remains_complete_when_pipe_close_yields_to_collector(self):
        # Widen the scheduling window after closing a pipe writer. Closing both
        # output writers before notifying the collector used to report EPIPE
        # even though the child's bytes, effects and exit status were complete.
        library = self.compile_library('yield_pipe_close', r'''#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
static pid_t owner;
__attribute__((constructor)) static void init(void) { owner = getpid(); }
int close(int fd) {
    int saved = errno;
    struct stat st;
    int flags = fcntl(fd, F_GETFL);
    int yield = fd > 2 && getpid() == owner && flags >= 0 &&
        (flags & O_ACCMODE) == O_WRONLY && !fstat(fd, &st) && S_ISFIFO(st.st_mode);
    errno = saved;
    int result = (int)syscall(SYS_close, fd);
    saved = errno;
    if (!result && yield) {
        const char* marker = getenv("CAPTURE_CLOSE_MARKER");
        if (marker) {
            int out = open(marker, O_WRONLY | O_CREAT | O_APPEND, 0600);
            if (out >= 0) { (void)write(out, "1", 1); (void)syscall(SYS_close, out); }
        }
        struct timespec pause = {0, 100000000};
        while (nanosleep(&pause, &pause) && errno == EINTR) {}
    }
    errno = saved;
    return result;
}
''')
        marker = self.directory / 'close-yielded'
        self.env.update(LD_PRELOAD=str(library), CAPTURE_CLOSE_MARKER=str(marker))
        fixture = self.directory / 'yield-close-command.sh'
        fixture.write_text("printf 'O\\000\\377'\nprintf 'E\\000\\376' >&2\n"
                           "printf applied >close-effect\nexit 7\n")
        for selector in ('--json', '--data', '--data-v2'):
            with self.subTest(selector=selector):
                marker.unlink(missing_ok=True)
                effect = self.directory / 'close-effect'
                effect.unlink(missing_ok=True)
                args = ('cmd', '/bin/sh yield-close-command.sh', 'rc', '7')
                value = (self.report(*args)[0][-1] if selector == '--json' else
                         self.native(*args, selector=selector)[0][-1])
                self.assertTrue(marker.read_bytes(), 'fault fixture did not yield on any pipe close')
                self.assertTrue(value['complete'])
                self.assertTrue(value['ok'])
                self.assertTrue(value['execution']['complete'])
                self.assertIsNone(value['execution']['adapter_errno'])
                self.assertEqual(value['execution']['command_status'], 7)
                self.assertEqual(base64.b64decode(value['execution']['stdout']['data']), b'O\0\xffE\0\xfe')
                self.assertEqual(effect.read_bytes(), b'applied')

    def boundary_executions(self, command, stdout, status=7, stderr=b''):
        """The same command must retain bytes, status and effects in all modes."""
        self.env['JASH_LLM_BUDGET'] = '4000000'
        for selector in ('text', '--json', '--data-v2'):
            with self.subTest(selector=selector):
                for name in ('before', 'after', 'observed.json'):
                    (self.directory / name).unlink(missing_ok=True)
                if selector == 'text':
                    process = self.run_cli('-c', '( ' + command +
                                           ' ); printf "|parent=%s|" "$?"')
                    self.assertEqual(process.stdout, stdout + ('|parent=%d|' % status).encode())
                    self.assertEqual(process.stderr, stderr)
                else:
                    value = (self.report('cmd', command, 'rc', str(status))[0][-1]
                             if selector == '--json' else
                             self.native('cmd', command, 'rc', str(status))[0][-1])
                    self.assertTrue(value['complete'])
                    self.assertTrue(value['execution']['complete'])
                    self.assertTrue(value['ok'])
                    self.assertEqual(value['execution']['command_status'], status)
                    self.assertEqual(base64.b64decode(value['execution']['stdout']['data']),
                                     stdout + stderr)
                yield selector

    def test_large_subshell_body_preserves_later_effects_bytes_and_final_status(self):
        command = 'touch before; echo ' + 'x' * 3000 + '; touch after; echo TAIL; false'
        self.assertGreater(len(command), 2047)
        for _ in self.boundary_executions(command, b'x' * 3000 + b'\nTAIL\n', status=1):
            self.assertTrue((self.directory / 'before').exists())
            self.assertTrue((self.directory / 'after').exists())

    def test_external_subshell_preserves_wide_argv_and_full_tokens(self):
        observer = self.python('''import json,os,sys
from pathlib import Path
value={'argv':sys.argv[1:],'tail_effect':sys.argv[-1]=='TAIL'}
Path('observed.json').write_text(json.dumps(value))
if value['tail_effect']:Path('after').write_text('applied')
os.write(1,json.dumps(value,separators=(',',':')).encode()+b'\\0\\xff')
sys.exit(7)''')
        for arguments in (['A%03d' % i for i in range(39)] + ['TAIL'],
                          ['x' * 1200, 'TAIL']):
            expected = {'argv': arguments, 'tail_effect': True}
            output = json.dumps(expected, separators=(',', ':')).encode() + b'\0\xff'
            command = observer + ' ' + ' '.join(arguments)
            for _ in self.boundary_executions(command, output):
                self.assertEqual(json.loads((self.directory / 'observed.json').read_text()), expected)
                self.assertEqual((self.directory / 'after').read_bytes(), b'applied')

    def test_external_subshell_preserves_environment_below_execution_limits(self):
        observer = self.python('''import json,os,sys
from pathlib import Path
keys=json.loads(Path('keys.json').read_text())
value={'environment':{key:os.environ.get(key) for key in keys},
       'tail_effect':os.environ.get('BOUNDARY_TAIL')=='kept'}
Path('observed.json').write_text(json.dumps(value))
if value['tail_effect']:Path('after').write_text('applied')
os.write(1,json.dumps(value,separators=(',',':')).encode()+b'\\0\\xff')
sys.exit(7)''')
        baseline = self.env.copy()
        for additions in ({'E%03d' % i: 'v' for i in range(160)},
                          {'BOUNDARY_PADDING': 'p' * 6000}):
            self.env = baseline.copy()
            additions['BOUNDARY_TAIL'] = 'kept'
            self.env.update(additions)
            (self.directory / 'keys.json').write_text(json.dumps(list(additions)))
            expected = {'environment': additions, 'tail_effect': True}
            output = json.dumps(expected, separators=(',', ':')).encode() + b'\0\xff'
            for _ in self.boundary_executions(observer, output):
                self.assertEqual(json.loads((self.directory / 'observed.json').read_text()), expected)
                self.assertEqual((self.directory / 'after').read_bytes(), b'applied')

    def test_external_environment_overflow_refuses_before_exec_and_redirect(self):
        observer = self.python('from pathlib import Path;Path("after").write_text("unexpected")')
        baseline = self.env.copy()
        diagnostic = b'jash: environment exceeds execution limit (512 entries or 65536 bytes)\n'
        for additions in ({'E%03d' % i: 'v' for i in range(600)},
                          {'PADDING_%d' % i: 'p' * 6000 for i in range(12)}):
            self.env = baseline.copy()
            self.env.update(additions)
            for command in (observer, 'true; ' + observer,
                            observer + ' | /bin/cat',
                            observer + ' >redirect-effect',
                            observer + ' & wait'):
                with self.subTest(command=command):
                    for _ in self.boundary_executions(command, b'', status=126, stderr=diagnostic):
                        self.assertFalse((self.directory / 'after').exists())
                        self.assertFalse((self.directory / 'redirect-effect').exists())

    def test_shared_environment_admission_accepts_exact_capacity_and_refuses_next_byte(self):
        source = self.directory / 'envp_boundary.c'
        binary = self.directory / 'envp_boundary'
        source.write_text(r'''#include <core/envp.h>
#include <stdio.h>
#include <stdlib.h>
static unsigned count;
static int large;
static char keys[JASH_ENVP_MAX + 1][16];
static char value[32767];
uint32_t jash_env_count(const jash_state_t* state) { (void)state; return count; }
int jash_env_at(const jash_state_t* state, uint32_t i, const char** key, const char** val) {
    (void)state;
    if (i >= count) return 0;
    *key = large ? "K" : keys[i];
    *val = large ? value + (large == 2 && i == 1) : "v";
    return 1;
}
int main(void) {
    char buf[JASH_ENVP_BYTES];
    char* envp[JASH_ENVP_MAX + 1];
    unsigned admitted = 0, bytes = 0;
    for (unsigned i = 0; i <= JASH_ENVP_MAX; i++)
        snprintf(keys[i], sizeof(keys[i]), "E%03u", i);
    count = JASH_ENVP_MAX;
    if (jash_envp_build(envp, JASH_ENVP_MAX, buf, sizeof(buf), NULL, &admitted, &bytes) ||
        admitted != JASH_ENVP_MAX || envp[admitted]) return 1;
    count++;
    if (!jash_envp_build(envp, JASH_ENVP_MAX, buf, sizeof(buf), NULL, &admitted, &bytes) ||
        errno != E2BIG || envp[0] || admitted || bytes) return 2;
    large = 1; count = 2;
    memset(value, 'x', 32765); value[32765] = 0;
    if (jash_envp_build(envp, JASH_ENVP_MAX, buf, sizeof(buf), NULL, &admitted, &bytes) ||
        admitted != 2 || bytes != sizeof(buf) || envp[2]) return 3;
    large = 2; value[32765] = 'x'; value[32766] = 0;
    if (!jash_envp_build(envp, JASH_ENVP_MAX, buf, sizeof(buf), NULL, &admitted, &bytes) ||
        errno != E2BIG || envp[0] || admitted || bytes) return 4;
    puts("exact 512 entries and 65536 bytes admitted; overflow refused");
    return 0;
}
''')
        compiler = subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                                   '-Werror', '-I' + str(management.ROOT / 'c'), str(source),
                                   '-o', str(binary)], env=self.env, capture_output=True, timeout=25)
        self.assertEqual(compiler.returncode, 0, compiler.stderr)
        result = subprocess.run([str(binary)], env=self.env, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b'exact 512 entries and 65536 bytes admitted', result.stdout)

    def test_file_predicates_and_false_results_have_complete_observations(self):
        (self.directory / 'empty').touch(); (self.directory / 'full').write_bytes(b'hello world')
        (self.directory / 'directory').mkdir(); (self.directory / 'program').write_text('exit 0\n')
        (self.directory / 'program').chmod(0o700)
        cases = [('file', 'full', 0), ('exists', 'missing', 1), ('dir', 'directory', 0),
                 ('dir', 'full', 1), ('no', 'missing', 0), ('absent', 'full', 1),
                 ('empty', 'empty', 0), ('empty', 'full', 1), ('nonempty', 'full', 0),
                 ('exec', 'program', 0), ('exec', 'full', 1)]
        for predicate, path, status in cases:
            with self.subTest(predicate=predicate, path=path):
                value = self.native(predicate, path, status=status)[0][-1]
                self.assertTrue(value['complete']); self.assertEqual(value['ok'], status == 0)
                self.assertEqual(value['predicate_status'], status)
                self.assertEqual(value['files'][0]['present'], path != 'missing')
                self.assertFalse(value['effects_may_have_applied'])

    def test_binary_file_contents_full_equality_and_second_granularity_times(self):
        payload = b'x' * 65534 + b'needle\0\xff' + b'y' * 2000
        first = self.directory / 'first'; second = self.directory / 'second'
        first.write_bytes(payload); second.write_bytes(payload)
        value = self.report('contains', 'first', 'needle')[0][-1]
        self.assertEqual(value['files'][0]['bytes_read'], len(payload))
        self.assertTrue(value['files'][0]['content_read_complete'])
        self.native('contains', 'first', 'absent', status=1)
        self.native('contains', 'first', '', status=1)
        self.native('match', 'first', 'second'); second.write_bytes(payload + b'changed')
        self.native('same', 'first', 'second', status=1)
        os.utime(first, ns=(100000000001, 100000000001)); os.utime(second, ns=(101000000001, 101000000001))
        self.native('newer', 'second', 'first'); self.native('older', 'first', 'second')
        os.utime(second, ns=(100999999999, 100999999999))
        self.native('newer', 'second', 'first', status=1)

    def test_link_resolution_and_negation_do_not_hide_observation_errors(self):
        (self.directory / 'target').touch()
        (self.directory / 'valid').symlink_to('target'); (self.directory / 'broken').symlink_to('missing')
        self.native('link', 'valid'); self.native('broken', 'broken')
        self.native('link', 'broken', status=1); self.native('not', 'link', 'broken')
        (self.directory / 'loop').symlink_to('loop')
        value = self.native('not', 'file', 'loop', status=1, failed=True)[0][-1]
        self.assertIsNone(value['ok']); self.assertEqual(value['errno'], errno.ELOOP)
        self.assertFalse(value['files'][0]['metadata_observed'])

    def test_previous_status_prefers_process_and_declares_unverified_chain_source(self):
        unknown = self.native('rc', '0', status=2, failed=True)[0][-1]
        self.assertEqual(unknown['error'], 'no_previous_status_source')
        (self.directory / '.jash').mkdir(mode=0o700, exist_ok=True)
        self.chain([(9007199254740993, 100, 7, b'synthetic previous command', '-')])
        known = self.native('rc', '7')[0][-1]['previous_status']
        self.assertEqual(known['source'], 'retained_chain_unverified')
        self.assertEqual(known['sequence'], 9007199254740993)
        self.assertFalse(known['chain_source']['cryptographically_verified'])
        process = self.run_cli('-c', 'false; expect --json rc 1')
        value = self.validate(json.loads(process.stdout))
        self.assertEqual(value['previous_status']['source'], 'current_process')
        (self.directory / '.jash/chain.log').write_bytes(b'broken\n')
        self.native('rc', '0', status=1, failed=True)

    def test_cmd_captures_combined_binary_output_in_subshell(self):
        command = self.python('import os,sys;os.write(1,b"one\\0");os.write(2,b"two\\xff");sys.exit(7)')
        value, _, process = self.native('cmd', command, 'output', 'two')
        result = value[-1]
        self.assertEqual(base64.b64decode(result['execution']['stdout']['data']), b'one\0two\xff')
        self.assertEqual(base64.b64decode(result['execution']['stderr']['data']), b'')
        self.assertEqual(result['execution']['command_status'], 7)
        self.assertEqual(result['execution_scope'], 'subshell')
        self.assertEqual(result['output_scope'], 'stdout_stderr_merged')
        self.assertEqual(process.stderr, b'')
        self.native('cmd', command, 'rc', '7')
        self.native('cmd', command, 'rc', '0', status=1)
        (self.directory / 'inner').mkdir()
        script = "expect --json cmd 'export EXPECT_INNER=changed; cd inner; echo marker' output marker; env --json"
        rows = [json.loads(line) for line in self.run_cli('-c', script).stdout.splitlines() if line.strip()]
        self.assertNotIn('EXPECT_INNER', rows[-1])

    def test_capture_overflow_is_not_a_negative_or_negated_predicate(self):
        command = self.python('import os;open("effect","w").write("yes");os.write(1,b"x"*4096+b"needle")')
        value = self.native('--capture-bytes', '32', 'not', 'cmd', command, 'output', 'needle', status=75, failed=True)[0][-1]
        self.assertIsNone(value['ok']); self.assertEqual(value['error'], 'output_capture_limit')
        self.assertTrue(value['effects_may_have_applied']); self.assertTrue((self.directory / 'effect').exists())
        self.assertEqual(value['execution']['stdout_bytes_seen'], 4102)
        self.assertTrue(value['execution']['stdout_truncated'])

    def test_snapshot_records_compares_updates_exact_bytes_and_observes_command_status(self):
        first_command = self.python('import os,sys;os.write(1,b"snapshot\\0bytes");os.write(2,b"diagnostic");sys.exit(7)')
        first = self.native('snapshot', 'golden', first_command)[0]
        self.assertEqual([row['kind'] for row in first], ['attempt', 'result'])
        self.assertIsNone(first[0]['holds_before_negation'])
        self.assertEqual(first[-1]['execution']['command_status'], 7)
        snap = first[-1]['snapshot']; self.assertEqual(snap['action'], 'record')
        self.assertTrue(snap['applied']); self.assertTrue(snap['durable'])
        path = self.directory / '.jash/snapshots/golden'
        self.assertEqual(path.read_bytes(), b'snapshot\0bytesdiagnostic')
        match = self.report('snapshot', 'golden', first_command)[0][-1]
        self.assertEqual(match['snapshot']['action'], 'compare'); self.assertFalse(match['snapshot']['applied'])
        changed = self.native('snapshot', 'golden', 'echo changed', status=1)[0][-1]
        self.assertFalse(changed['ok']); self.assertTrue(changed['complete'])
        update = self.report('snapshot', 'golden', '--update', 'echo changed')[0][-1]
        self.assertEqual(update['snapshot']['action'], 'update'); self.assertEqual(path.read_bytes(), b'changed\n')
        self.native('snapshot', 'golden', 'echo changed')

    def test_snapshot_path_validation_trust_and_missing_command_precede_publication(self):
        for args in [('snapshot', '../escape', 'touch effect'), ('snapshot', '.memory-lock', 'touch effect'),
                     ('snapshot', 'good', '--update'), ('snapshot', 'good')]:
            self.native(*args, status=2, failed=True)
            self.assertFalse((self.directory / 'effect').exists())
        directory = self.directory / '.jash/snapshots'; directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        target = self.directory / 'untouched'; target.write_bytes(b'original')
        (directory / 'golden').symlink_to(target)
        value = self.native('snapshot', 'golden', '--update', 'echo replacement', status=1, failed=True)[0][-1]
        self.assertFalse(value['snapshot']['applied']); self.assertFalse(value['snapshot']['read_complete'])
        self.assertIsNone(value['snapshot']['present_before']); self.assertEqual(target.read_bytes(), b'original')

    def test_snapshot_publication_failure_reports_applied_without_durable(self):
        library = self.compile_library('snapshot_fault', '''#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int published;
int renameat(int a,const char*b,int c,const char*d) {
 int (*next)(int,const char*,int,const char*)=dlsym(RTLD_NEXT,"renameat");
 int rc=next(a,b,c,d); if(!rc&&!strcmp(d,"golden"))published=1; return rc;
}
int fsync(int fd) {
 int (*next)(int)=dlsym(RTLD_NEXT,"fsync"); char link[64],path[4096];
 snprintf(link,sizeof(link),"/proc/self/fd/%d",fd); ssize_t n=readlink(link,path,sizeof(path)-1);
 if(n>0){path[n]=0;const char*root=getenv("SNAPSHOT_TEST_ROOT");
  if(published&&root&&!strcmp(path,root)){errno=EIO;return -1;}}
 return next(fd);
}
''')
        self.env.update(LD_PRELOAD=str(library), SNAPSHOT_TEST_ROOT=str(self.directory / '.jash/snapshots'))
        value = self.native('snapshot', 'golden', 'echo durable-test', status=1, failed=True)[0][-1]
        self.assertEqual(value['error'], 'snapshot_publish_failed')
        self.assertTrue(value['snapshot']['applied']); self.assertFalse(value['snapshot']['durable'])
        self.assertEqual((self.directory / '.jash/snapshots/golden').read_bytes(), b'durable-test\n')

    def test_until_waits_for_actual_state_and_timeout_is_complete_false(self):
        timer = threading.Timer(0.2, lambda: (self.directory / 'arrived').touch())
        timer.start(); self.addCleanup(timer.join)
        rows = self.native('--until', '2', 'file', 'arrived')[0]
        self.assertGreater(rows[-1]['wait']['attempts'], 1)
        self.assertEqual(len(rows), rows[-1]['wait']['attempts'] + 1)
        self.assertFalse(rows[-1]['wait']['timed_out'])
        timeout = self.native('--until', '1', 'file', 'never', status=1)[0][-1]
        self.assertTrue(timeout['complete']); self.assertTrue(timeout['wait']['timed_out'])
        self.assertGreaterEqual(timeout['wait']['elapsed'], 1000000000)
        self.assertLessEqual(timeout['wait']['attempts'], 11)

    def test_until_command_emits_each_effect_receipt_before_next_attempt(self):
        command = self.python('from pathlib import Path;p=Path("counter");n=int(p.read_text())+1 if p.exists() else 1;p.write_text(str(n));print("ready" if n>=2 else "waiting")')
        rows = self.report('--until', '1', '--capture-bytes', '64', 'cmd', command, 'output', 'ready')[0]
        self.assertEqual([row['kind'] for row in rows], ['attempt', 'attempt', 'result'])
        self.assertEqual([row['holds_before_negation'] for row in rows[:-1]], [False, True])
        self.assertEqual((self.directory / 'counter').read_text(), '2')

    def test_delivery_budget_and_invalid_options_admit_before_effects(self):
        for selector in ('--json', '--data'):
            for args in [('cmd', 'touch effect', 'rc', '0'), ('snapshot', 'golden', 'touch effect')]:
                with open('/dev/full', 'wb') as sink:
                    self.execute(['expect', selector, *args], stdout=sink, status=125)
                self.assertFalse((self.directory / 'effect').exists())
                self.assertFalse((self.directory / '.jash/snapshots').exists())
        for args in [('--until', '0', 'cmd', 'touch effect', 'rc', '0'),
                     ('cmd', 'touch effect', 'wrong', 'x'), ('cmd', 'touch effect', 'rc', 'no'),
                     ('--capture-bytes', '0', 'cmd', 'touch effect', 'rc', '0')]:
            self.native(*args, status=2, failed=True)
            self.assertFalse((self.directory / 'effect').exists())
        self.env['JASH_LLM_BUDGET'] = '1'
        value = json.loads(self.execute(['expect', '--json', 'cmd', 'touch effect', 'rc', '0'], status=75).stdout)
        self.assertEqual(value['schema'], 'jash.builtin.error.v1')
        self.assertFalse((self.directory / 'effect').exists())
        for selector in ('--data', '--data-v2'):
            self.native('cmd', 'true', 'rc', '0', selector=selector)

    def test_file_byte_limit_and_literal_paths_are_honest(self):
        path = self.directory / 'large'; path.write_bytes(b'x' * (16*1024*1024 + 1))
        value = self.native('not', 'contains', 'large', 'needle', status=75, failed=True)[0][-1]
        self.assertIsNone(value['ok']); self.assertEqual(value['error'], 'file_byte_limit')
        name = b'literal space-\xff--json'
        descriptor = os.open(os.fsencode(self.directory) + b'/' + name, os.O_WRONLY | os.O_CREAT, 0o600)
        os.close(descriptor)
        value = self.native('file', name)[0][-1]
        self.assertEqual(base64.b64decode(value['files'][0]['path']['data']), name)

    def test_schema_rejects_false_sources_and_predicate_promotion(self):
        value = self.report('file', 'missing', status=1)[0][-1]
        wrong = copy.deepcopy(value); wrong['ok'] = True
        self.assertFalse(self.validator.is_valid(wrong))
        wrong = copy.deepcopy(value); wrong['complete'] = False
        self.assertFalse(self.validator.is_valid(wrong))
        wrong = copy.deepcopy(value); del wrong['files'][0]['metadata_observed']
        self.assertFalse(self.validator.is_valid(wrong))


if __name__ == '__main__':
    unittest.main(verbosity=2)
