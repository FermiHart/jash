#!/usr/bin/env python3
"""Measure common builtin modes against explicit binaries on Linux.

Each sample uses a native measurement helper, so wait4 peak RSS excludes the
Python runner's address space. Binaries/cases are interleaved
with a fixed shuffle seed. This is warm local evidence, not a universal speed
threshold or a substitute for the semantic/native-decoder gates.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import signal
import statistics
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def request(*argv):
    return ['--exec-json', json.dumps({'argv': list(argv)}, separators=(',', ':'))]


def sample(helper, binary, argv, cwd, env):
    read_fd, write_fd = os.pipe()
    try:
        process = subprocess.Popen([str(helper), str(write_fd), str(binary), *argv],
                                   cwd=cwd, env=env, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, pass_fds=(write_fd,),
                                   start_new_session=True)
    finally:
        os.close(write_fd)
    try:
        try:
            stdout, stderr = process.communicate(timeout=20)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.communicate()
            raise
        if process.returncode:
            raise RuntimeError(dict(status=process.returncode, stderr=stderr.decode(errors='replace')))
        with os.fdopen(read_fd, 'rb', closefd=False) as source:
            value = json.load(source)
    finally:
        os.close(read_fd)
    value.update(stdout_bytes=len(stdout), stderr_bytes=len(stderr),
                 stdout_sha256=hashlib.sha256(stdout).hexdigest())
    return value


def validate_case(case, data, fixture):
    if case == 'startup_true':
        assert data == b''
    elif case == 'pwd_json':
        assert json.loads(data)['cwd'] == str(fixture)
    elif case == 'env_json':
        assert json.loads(data)['BENCH_VALUE'] == 'known-value'
    elif case == 'ls_json_256':
        rows = [json.loads(line) for line in data.splitlines()]
        assert len(rows) == 256 and {r['name'] for r in rows} == {f'item-{i:04d}' for i in range(256)}
    elif case == 'stat_json_32':
        rows = json.loads(data)
        assert len(rows) == 32 and all(r['size'] == 128 for r in rows)
    elif case == 'wc_json_1mib':
        row, = json.loads(data)
        assert (row['lines'], row['bytes']) == (8192, 1048576)
    elif case.startswith('ls_jtv'):
        assert data.startswith(b'JTV1' if case == 'ls_jtv1_256' else b'JTV2')
    elif case == 'contract_full':
        rows = json.loads(data)['operations']
        assert sum(row['kind'] == 'builtin' for row in rows) == 171
    elif case in ('contract_one', 'contract_ls'):
        row, = json.loads(data)['operations']
        assert row['id'] == ('builtin:ls' if case == 'contract_ls' else 'runtime:job.read')
    else:
        raise ValueError(case)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, action='append', required=True,
                        help='Repeat in comparison order: baseline first, candidate second.')
    parser.add_argument('--iterations', type=int, default=40)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if platform.system() != 'Linux' or not 10 <= args.iterations <= 2000:
        parser.error('Linux and 10..2000 iterations are required.')
    binaries = [path.resolve(strict=True) for path in args.binary]
    evidence = dict(kind='jash-builtin-modes-benchmark-v1', platform=platform.platform(),
                    iterations=args.iterations, seed=5630, binaries=[], comparisons=[],
                    limitations=[
                        'Warm local fixtures; not cold-cache, remote storage, device buffers or concurrent throughput.',
                        'Time is CLOCK_MONOTONIC around native fork/exec/wait4, including output delivery but excluding the Python/helper launch.',
                        'Peak RSS is Linux wait4 ru_maxrss in KiB; the target forks from a small native helper after Python has been replaced by exec.',
                        'Native cases check transport identity here; independent semantic/decoder gates qualify the values.',
                        'A larger opt-in discovery document includes more reviewed contracts; byte growth is reported, not hidden.',
                        'No universal cross-machine latency threshold or speedup claim is inferred from these samples.'])
    with tempfile.TemporaryDirectory(prefix='jash-builtin-bench-') as temporary:
        base = Path(temporary)
        helper = base / 'measure'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O2', '-Wall', '-Wextra',
                        '-Werror', str(ROOT/'tools/bench_builtin_modes.c'), '-o', str(helper)], check=True)
        home, work, listing = base/'home', base/'work', base/'work/listing'
        home.mkdir(); work.mkdir(); listing.mkdir()
        for i in range(256):
            (listing/f'item-{i:04d}').write_bytes(b'x'*127+b'\n')
        (work/'payload').write_bytes((b'x'*127+b'\n')*8192)
        env = dict(HOME=str(home), PATH=os.defpath, LC_ALL='C', TZ='UTC', BENCH_VALUE='known-value',
                   JASH_SIGIL='0', JASH_ADOPT='0', JASH_MEMO='0', JASH_NUDGE='0', JASH_BRIEF='0',
                   JASH_LOOPGUARD='off', JASH_WORLD='0', JASH_PREFETCH='0', JASH_TIMELINE='0')
        cases = dict(startup_true=['-c','true'],pwd_json=request('pwd','--json'),env_json=request('env','--json'),
                     ls_json_256=request('ls','--json',str(listing)),
                     stat_json_32=request('stat','--json',*(str(listing/f'item-{i:04d}')for i in range(32))),
                     wc_json_1mib=request('wc','--json',str(work/'payload')),
                     ls_jtv1_256=request('ls','--data',str(listing)),
                     ls_jtv2_256=request('ls','--data-v2',str(listing)),
                     contract_full=['--contract-json'],contract_one=['--contract-json','runtime:job.read'],
                     contract_ls=['--contract-json','builtin:ls'])
        samples = {}
        for number, binary in enumerate(binaries):
            identity = dict(path=str(binary),sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                            file_bytes=binary.stat().st_size,cases={})
            for name, argv in cases.items():
                initial = subprocess.run([str(binary),*argv],cwd=work,env=env,capture_output=True,timeout=20,check=True)
                validate_case(name,initial.stdout,work)
                samples[number,name]=[]
                # Keep the reproducible fixture grammar, not the transient path.
                identity['cases'][name]=dict(argv=[s.replace(str(base),'$FIXTURE') for s in argv],
                                            initial_stdout_bytes=len(initial.stdout))
            evidence['binaries'].append(identity)
        order = list(samples)
        rng = random.Random(5630)
        for _ in range(args.iterations):
            rng.shuffle(order)
            for number,name in order:
                samples[number,name].append(sample(helper,binaries[number],cases[name],work,env))
        for (number,name),values in samples.items():
            times=sorted(v['elapsed_ns']for v in values)
            record=evidence['binaries'][number]['cases'][name]
            record.update(median_ns=statistics.median(times),p95_ns=times[int((len(times)-1)*.95)],
                          peak_rss_kib=max(v['peak_rss_kib']for v in values),
                          median_stdout_bytes=statistics.median(v['stdout_bytes']for v in values),raw=values)
        if len(binaries)>1:
            before=evidence['binaries'][0]['cases']
            for number in range(1,len(binaries)):
                after=evidence['binaries'][number]['cases']
                evidence['comparisons'].append(dict(baseline=0,candidate=number,cases={
                    name:dict(median_time_ratio=after[name]['median_ns']/before[name]['median_ns'],
                              peak_rss_delta_kib=after[name]['peak_rss_kib']-before[name]['peak_rss_kib'],
                              stdout_delta_bytes=after[name]['median_stdout_bytes']-before[name]['median_stdout_bytes'])
                    for name in cases}))
    with args.output.open('x',encoding='utf-8')as output:
        json.dump(evidence,output,indent=2);output.write('\n')
    for number,binary in enumerate(evidence['binaries']):
        for name,result in binary['cases'].items():
            print(f"{number} {name}: median {result['median_ns']/1e6:.3f} ms; "
                  f"RSS {result['peak_rss_kib']} KiB; stdout {result['median_stdout_bytes']} B")
    return 0


if __name__=='__main__':
    raise SystemExit(main())
