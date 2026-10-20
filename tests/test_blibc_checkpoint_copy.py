#!/usr/bin/env python3
"""INV-JASH-CHECKPOINT-BBC-001: execute the actual checkpoint producer on blibc.

The installed rm failure was an unknown-root-BOS strcpy in cp_lock. Exercise
the production bodies with that same string boundary, keep all filesystem
guards, and require the old call-site mutation to trap. No shell mode or BBC
guard is disabled.
"""
import argparse
import hashlib
from pathlib import Path
import re
import shlex
import subprocess


JASH = Path(__file__).resolve().parents[1]
BEYOND = JASH.parent / 'Beyond'


def function(text, name):
    match = re.search(r'^static\s+[^\n]*\b' + name + r'\s*\(', text, re.M)
    if not match:
        raise ValueError('missing production function: ' + name)
    start = text.index('{', match.start())
    depth = 1
    end = start + 1
    while depth:
        if text[end] == '{':
            depth += 1
        elif text[end] == '}':
            depth -= 1
        end += 1
    return text[match.start():end]


def run(argv, *, expected=0):
    result = subprocess.run([str(x) for x in argv], capture_output=True,
                            text=True, timeout=60)
    if result.returncode != expected:
        raise AssertionError((argv, result.returncode, result.stdout, result.stderr))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    out = args.out.resolve()
    if not out.is_relative_to(Path('/tmp/opencode')) or out.exists():
        parser.error('new private output below /tmp/opencode required')
    out.mkdir()
    source = JASH / 'c/builtins/builtin_checkpoint.c'
    text = source.read_text()
    before = hashlib.sha256(source.read_bytes()).hexdigest()
    declarations = '\n'.join(re.findall(
        r'^typedef struct \{[^\n]+\} (?:cp_t|cp_transaction_t);$', text, re.M))
    if len(declarations.splitlines()) != 2:
        raise AssertionError('actual checkpoint types not found')
    bodies = '\n'.join(function(text, name) for name in
                       ('cp_unlock', 'cp_lock', 'cp_snapshot'))
    prefix = r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#define CP_NAME 64
int fchmod(int, mode_t);
mode_t umask(mode_t);
uid_t geteuid(void);
extern char *__bear_copy(char *, const char *, size_t);
/* The measured GCC/blibc binary passed -1 at this actual call site. */
#undef strcpy
#define strcpy(d,s) __bear_copy((d),(s),(size_t)-1)
'''
    probe = r'''
int main(int argc, char **argv) {
    if (argc != 3) return 10;
    if (!strcmp(argv[1], "snapshot")) {
        struct { uint64_t left; cp_t rows[2]; uint64_t right; } box;
        memset(&box, 0xA5, sizeof(box));
        int invalid = 0, truncated = 0;
        int count = cp_snapshot(argv[2], box.rows, 2, &invalid, &truncated, NULL, 1);
        if (box.left != 0xA5A5A5A5A5A5A5A5ULL ||
            box.right != 0xA5A5A5A5A5A5A5A5ULL) return 11;
        printf("%d %d %d\n", count, invalid, truncated);
        if (count > 0) printf("%s\n%s\n%d\n%ld\n", box.rows[0].name,
                             box.rows[0].cwd, box.rows[0].depth, box.rows[0].ts);
        return 0;
    }
    struct { uint64_t left; cp_transaction_t value; uint64_t right; } box;
    memset(&box, 0xA5, sizeof(box));
    int result = cp_lock(argv[2], &box.value);
    if (box.left != 0xA5A5A5A5A5A5A5A5ULL ||
        box.right != 0xA5A5A5A5A5A5A5A5ULL) return 12;
    if (!strcmp(argv[1], "reject")) {
        if (result != -1 || box.value.parent != -1 || box.value.lock != -1) return 13;
        return 0;
    }
    if (result != 0 || strcmp(box.value.name, strrchr(argv[2], '/') + 1)) return 14;
    cp_unlock(&box.value);
    return box.value.parent == -1 && box.value.lock == -1 ? 0 : 15;
}
'''
    defs = shlex.split(run(['make', '--no-print-directory', '-s', '-C',
                            BEYOND / 'toolchain/bear',
                            'print-sovereign-defsyms']).stdout)
    for name in ('fchmod', 'umask', 'geteuid', 'mkdir', 'fstat', 'strsep'):
        defs += ['--defsym', name + '=bear_' + name]
    runtime = out / 'runtime'
    runtime.mkdir()
    bear = BEYOND / 'toolchain/bear/bear'
    basm = BEYOND / 'toolchain/BearBasm/basm'
    linker = BEYOND / 'BearLinker/bear-ld-host'
    libc = BEYOND / 'surfaces/blibc'
    flags = ['-ffunction-sections', '-I' + str(libc / 'include'),
             '-I' + str(libc), '-I' + str(libc / 'posix/include')]
    for name, path in (
            ('start_main', libc / 'osif/linux/start_main.c'),
            ('posix_minimal', BEYOND / 'BearLinker/sovereign-shims/posix_minimal.c')):
        run([bear, *flags, '-c', path, '-o', runtime / (name + '.o')])
    for name, path in (('start', libc / 'osif/linux/start.basm'),
                       ('syscall', libc / 'osif/linux/syscall_x86_64.basm')):
        run([basm, '-f', 'elf64', path, '-o', runtime / (name + '.o')])

    def build(name, production):
        unit = out / (name + '.c')
        unit.write_text(prefix + declarations + '\n' + production + probe)
        obj = out / (name + '.o')
        exe = out / name
        run([bear, *flags, '-c', unit, '-o', obj])
        run([linker, '--gc-sections', '-T', BEYOND / 'BearLinker/userspace.ld',
             *defs, '-o', exe, runtime / 'start.o', runtime / 'start_main.o',
             runtime / 'posix_minimal.o', obj, runtime / 'syscall.o',
             libc / 'build/libblibc.a'])
        exe.chmod(0o700)
        return exe

    positive = build('checkpoint', bodies)
    parent = out / 'owned'
    parent.mkdir(mode=0o700)
    run([positive, 'lock', parent / 'checkpoint.tsv'])
    run([positive, 'lock', parent / ('n' * 127)])
    run([positive, 'reject', out / 'never-created' / ('n' * 128)])
    if (out / 'never-created').exists():
        raise AssertionError('oversized name created its parent before refusal')
    exposed = out / 'exposed'
    exposed.mkdir(mode=0o700)
    exposed.chmod(0o777)
    run([positive, 'reject', exposed / 'checkpoint.tsv'])
    symlink = out / 'symlink'
    symlink.symlink_to(parent, target_is_directory=True)
    run([positive, 'reject', symlink / 'checkpoint.tsv'])

    def snapshot(data, expected):
        path = parent / 'rows.tsv'
        path.write_bytes(data)
        path.chmod(0o600)
        actual = run([positive, 'snapshot', path]).stdout.splitlines()
        if actual != expected:
            raise AssertionError((actual, expected))

    snapshot(b'alpha\t3\t17\t/tmp/with space\n',
             ['1 0 0', 'alpha', '/tmp/with space', '3', '17'])
    snapshot(b'n' * 63 + b'\t0\t1\t' + b'c' * 511 + b'\n',
             ['1 0 0', 'n' * 63, 'c' * 511, '0', '1'])
    snapshot(b'n' * 64 + b'\t0\t1\tcwd\n', ['0 1 0'])
    snapshot(b'name\t0\t1\t' + b'c' * 512 + b'\n', ['0 1 0'])
    snapshot(b'name\t0\t1\tcwd', ['-1 0 0'])
    snapshot(b'name\t0\t1\tc\x00wd\n', ['-1 0 0'])
    old = bodies.replace('memcpy(transaction->name, slash + 1, strlen(slash + 1) + 1);',
                         'strcpy(transaction->name, slash + 1);')
    if old == bodies:
        raise AssertionError('old call-site mutation did not apply')
    negative = build('old-copy', old)
    refused = run([negative, 'lock', parent / 'old.tsv'], expected=134)
    if 'bounded-by-construction TRAP' not in refused.stderr:
        raise AssertionError('old copy did not reach the real BBC refusal')
    if hashlib.sha256(source.read_bytes()).hexdigest() != before:
        raise AssertionError('production source changed during the check')
    print('checkpoint/blibc: actual lock/snapshot, bounds, filesystem refusals and old-copy trap PASS')


if __name__ == '__main__':
    main()
