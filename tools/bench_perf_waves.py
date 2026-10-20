#!/usr/bin/env python3
"""Byte-checked ten-wave benchmark: frozen jash, candidate jash, and Bash.

Timing is informational, never a CI correctness gate. The default core profile
retains the original ten waves, including the agent lane. The redirections
profile isolates redirect workloads. Bash is only a cost floor when jash undo
or agent observations are active, not a feature-equivalent comparison.
"""

import argparse
import hashlib
import itertools
import json
import os
import platform
from pathlib import Path
import resource
import shutil
import statistics
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parent.parent


def workloads(profile="core"):
    payload = "abcdefgh" * 512
    if profile == "append":
        def append_case(name, size, count, body="printf '%s\\n' appended >>output"):
            return (name, f"for ((i=0;i<{count};i++)); do {body}; done; echo $i",
                    {"output": (b"x" * size, 0o640)})
        return {
            1: append_case("append-small-200", 7, 200),
            2: append_case("append-4KiB-200", 4096, 200),
            3: append_case("append-32KiB-100", 32768, 100),
            4: append_case("append-capture-boundary-32", 65520, 32),
            5: append_case("append-128KiB-32", 131072, 32),
            6: append_case("empty-append-dedup-200", 4096, 200, "printf '' >>output"),
            7: ("overwrite-dedup-control-200", "p='" + "x" * 4096 + "'; "
                "for ((i=0;i<200;i++)); do printf '%s' \"$p\" >output; done; echo $i",
                {"output": (b"x" * 4096, 0o640)}),
        }
    if profile == "read":
        short = b"input with spaces and a \\backslash\nsecond line\n"
        return {
            1: ("read-short-200", "for ((i=0;i<200;i++)); do read -r line <input || exit; done; "
                "printf '%s:%s\\n' \"$i\" \"$line\"", {"input": (short, 0o640)}),
            2: ("read-short-2000", "for ((i=0;i<2000;i++)); do read -r line <input || exit; done; "
                "printf '%s:%s\\n' \"$i\" \"$line\"", {"input": (short, 0o640)}),
            3: ("read-400-bytes-200", "for ((i=0;i<200;i++)); do read -r line <input || exit; done; "
                "printf '%s:%s\\n' \"$i\" \"$line\"", {"input": (b"x" * 400 + b"\ntail\n", 0o640)}),
            4: ("read-shared-fd-500", "exec 3<input; for ((i=0;i<500;i++)); do "
                "read -r line <&3 || exit; done; exec 3<&-; printf '%s:%s\\n' \"$i\" \"$line\"",
                {"input": ((b"x" * 34 + b"\n") * 500, 0o640)}),
            5: ("read-while-500", "exec <input; count=0; while read -r line; do count=$((count+1)); done; "
                "printf '%s\\n' \"$count\"", {"input": ((b"x" * 34 + b"\n") * 500, 0o640)}),
            6: ("read-count-fallback-200", "for ((i=0;i<200;i++)); do read -r -n 3 line <input || exit; done; "
                "printf '%s:%s\\n' \"$i\" \"$line\"", {"input": (b"abcdef\ntail\n", 0o640)}),
        }
    if profile == "redirections":
        large = "abcdefgh" * 1536  # Below the existing 16KiB source-segment limit.
        seed = (b"before\n", 0o640)
        # Fixtures are explicit data, never inferred from the shell program.
        return {
            1: ("truncate-small", "for ((i=0;i<200;i++)); do "
                "printf '%s\\n' small > output; done; echo $i", {"output": seed}),
            2: ("truncate-4KiB-original200", "p='" + payload + "'; for ((i=0;i<200;i++)); do "
                "printf '%s' \"$p\" > output; done; echo $i", {}),
            3: ("truncate-12KiB", "p='" + large + "'; for ((i=0;i<40;i++)); do "
                "printf '%s' \"$p\" > output; done; echo $i",
                {"output": (large.encode(), 0o640)}),
            4: ("append", "for ((i=0;i<200;i++)); do "
                "printf '%s\\n' appended >> output; done; echo $i", {"output": seed}),
            5: ("quoted-dynamic-filename", "file='quoted output'; for ((i=0;i<200;i++)); do "
                "printf '%s\\n' quoted > \"$file\"; done; echo $i", {"quoted output": seed}),
            6: ("noclobber-override", "set -C; for ((i=0;i<200;i++)); do "
                "printf '%s\\n' clobbered >| output; done; set +C; echo $i", {"output": seed}),
            7: ("fddup-ordering-fallback", "f(){ printf 'out\\n'; printf 'err\\n' >&2; }; "
                "for ((i=0;i<100;i++)); do f > combined 2>&1; f 2>&1 > ordered; done; echo $i",
                {"combined": seed, "ordered": seed}),
            8: ("input-builtin-read", "for ((i=0;i<200;i++)); do "
                "read -r line < input || exit; done; printf '%s:%s\\n' \"$i\" \"$line\"",
                {"input": (b"input with spaces and a \\backslash\nsecond line\n", 0o640)}),
            9: ("redirected-function", "f(){ printf '%s\\n' \"$1\"; }; "
                "for ((i=0;i<200;i++)); do f function > output; done; echo $i", {"output": seed}),
            10: ("external-pipeline-fallback", "for ((i=0;i<40;i++)); do "
                 "/bin/cat < input > external; /bin/cat < input | /bin/cat > piped; done; echo $i",
                 {"input": (payload.encode(), 0o640), "external": seed, "piped": seed}),
        }
    core = {
        1: ("startup", "printf '%s\\n' startup"),
        2: ("dispatch", "for ((i=0;i<2000;i++)); do test x = x; done; echo $i"),
        3: ("printf", "for ((i=0;i<250;i++)); do printf '%s:%d\\n' "
            + "'" + "abcdefgh" * 32 + "' 123456; done"),
        4: ("expansion", "v=payload; for ((i=0;i<1000;i++)); do "
            ": \"$v\" \"${v:-fallback}\" \"$((i+1))\"; done; echo $i"),
        5: ("functions", "f(){ :; }; for ((i=0;i<1000;i++)); do "
            "f a b c d e f g h; done; echo $i"),
        6: ("lists", ":;" * 2048 + "echo complete"),
        7: ("pipelines", "for ((i=0;i<40;i++)); do printf x | cat >/dev/null; done; echo $i"),
        8: ("redirects", "p='" + payload + "'; for ((i=0;i<200;i++)); do "
            "printf '%s' \"$p\" > output; done; echo $i"),
        9: ("positionals", "for ((i=0;i<1000;i++)); do set -- a b c d e f g h; done; "
            "printf '%s:%s\\n' \"$#\" \"$8\""),
        10: ("agent-fschange", "printf changed > tree/d0/f0"),
    }
    tree = {f"tree/d{d}/f{f}": (b"seed", 0o644) for d in range(20) for f in range(50)}
    return {wave: (name, code, tree if wave == 10 else {}) for wave, (name, code) in core.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", type=Path, default=ROOT / "build/jash")
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--freeze", type=Path, help="Copy candidate to an exclusive baseline path and exit")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--profile", choices=("core", "redirections", "read", "append"), default="core")
    parser.add_argument("--timeline", choices=("on", "off", "default"), default="default",
                        help="on: include jash undo cost; off: disable undo; default: leave JASH_TIMELINE unset")
    parser.add_argument("--runs", type=int, default=20)
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--waves", type=int, nargs="+")
    parser.add_argument("--check", action="store_true", help="Only validate results; do not time")
    parser.add_argument("--trace", action="store_true", help="Also record separate strace -c runs")
    parser.add_argument("--store-depth", type=int, default=0,
                        help="Extra fixture directories under TMPDIR to measure backup path traversal")
    args = parser.parse_args()
    if args.freeze:
        with args.freeze.open("xb") as dst, args.candidate.open("rb") as src:
            shutil.copyfileobj(src, dst)
        args.freeze.chmod(args.candidate.stat().st_mode & 0o777)
        print("baseline", args.freeze, hashlib.sha256(args.freeze.read_bytes()).hexdigest())
        return
    if not args.baseline or not args.output or args.runs < 1 or args.warmup < 0:
        parser.error("require --baseline, --output, positive --runs and nonnegative --warmup")
    if not 0 <= args.store_depth <= 16:
        parser.error("--store-depth must be between 0 and 16")
    cases = workloads(args.profile)
    if args.waves is None:
        args.waves = sorted(cases)
    if any(w not in cases for w in args.waves):
        parser.error(f"waves for {args.profile} must be in {sorted(cases)}")
    binaries = {"baseline": args.baseline.resolve(), "candidate": args.candidate.resolve(),
                "bash": Path("/usr/bin/bash")}
    undo_active = args.timeline != "off"
    timeline_note = {"on": "JASH_TIMELINE=1: jash undo active and its cost included",
                     "off": "JASH_TIMELINE=0: jash undo explicitly disabled",
                     "default": "JASH_TIMELINE unset (not inherited): jash default undo cost included"}[args.timeline]
    report = {"schema": 1, "profile": args.profile, "timeline": args.timeline,
              "store_depth": args.store_depth,
              "backup_validation": "append with undo checks the initial preimage outside timing",
              "platform": platform.platform(),
              "cpu_affinity": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
              "timeline_note": timeline_note, "runs": args.runs, "warmup": args.warmup,
              "comparison": "cumulative candidate vs frozen baseline, not isolated per-change attribution",
              "bash_comparison": ("cost floor only: Bash has no jash undo" if undo_active else
                                  "same shell workload with jash undo explicitly disabled")
                                 + ("; core agent wave is always cost floor only" if args.profile == "core" else ""),
              "reset": "HOME, TMPDIR, work files and fixture contents/modes reset before every invocation, outside timing",
              "validation": "rc=0, stdout bytes and work file contents/modes; private HOME undo state excluded",
              "umask": "0022",
              "order": "six permutations, rotated per wave", "waves": [],
              "binaries": {name: {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                           for name, path in binaries.items()}}
    report["git_head"] = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    report["worktree_diff_sha256"] = hashlib.sha256(subprocess.check_output(["git", "diff"], cwd=ROOT)).hexdigest()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    print(f"profile={args.profile}, timeline={args.timeline}: {timeline_note}; "
          + report["bash_comparison"], flush=True)
    with args.output.open("x") as output, tempfile.TemporaryDirectory(prefix="jash-perf-waves-") as tmp:
        base = Path(tmp)
        home, work = base / "home", base / "work"
        store_tmp = home / "tmp"
        for depth in range(args.store_depth):
            store_tmp /= f"d{depth:02d}"
        if len(os.fsencode(store_tmp)) + len("/jash_timeline_") + 20 >= 256:
            raise RuntimeError("fixture TMPDIR exceeds the existing backup path capacity")
        env = {"HOME": str(home), "TMPDIR": str(store_tmp), "PATH": "/usr/bin:/bin",
               "LC_ALL": "C", "TERM": "dumb", "JASH_SIGIL": "0", "JASH_CHANGES": "0",
               "JASH_MEMO": "0", "JASH_BRIEF": "0", "JASH_NUDGE": "0", "JASH_WORLD": "0",
               "JASH_LOOPGUARD": "off", "JASH_DRIFT": "0", "JASH_GUARD": "0",
               "JASH_GUARD_BYPASS": "0", "JASH_GUARD_BYPASS_STRICT": "0",
               "JASH_RECALL_GUARD": "0", "JASH_RISK_CHECKPOINT": "0", "JASH_SKIP_SELFCHECK": "1",
               "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1", "UBSAN_OPTIONS": "halt_on_error=1"}
        if args.timeline != "default":
            env["JASH_TIMELINE"] = "1" if args.timeline == "on" else "0"
        report["environment"] = env
        report["cwd"] = str(work)
        for wave in args.waves:
            name, code, fixtures = cases[wave]
            agent_wave = args.profile == "core" and wave == 10
            agent = {"JASH_AGENT": "1", "JASH_CHANGES": "1"} if agent_wave else {}
            commands = {label: [str(binary), *(["--noprofile", "--norc"] if label == "bash" else []),
                                "-c", code] for label, binary in binaries.items()}
            row = {"wave": wave, "name": name, "commands": commands, "environment_overrides": agent,
                   "agent_cost_floor_only": agent_wave, "undo_cost_included": undo_active,
                   "bash_cost_floor_only": undo_active or agent_wave,
                   "fixtures": {path: {"bytes": len(data), "mode": mode,
                                       "sha256": hashlib.sha256(data).hexdigest()}
                                for path, (data, mode) in fixtures.items()},
                   "samples": {label: [] for label in binaries}}

            def run(label, trace=False, timed=False):
                # Include undo creation in shell time, but never cleanup or fixture preparation.
                for directory in (home, work):
                    if directory.exists():
                        shutil.rmtree(directory)
                    directory.mkdir(mode=0o700)
                (home / ".jashrc").touch()
                store_tmp.mkdir(mode=0o700, parents=True)
                for relative, (data, mode) in fixtures.items():
                    path = work / relative
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(data)
                    path.chmod(mode)
                command = commands[label]
                if trace:
                    command = ["/usr/bin/strace", "-f", "-c", *command]
                if timed:
                    before = resource.getrusage(resource.RUSAGE_CHILDREN)
                    start = time.perf_counter_ns()
                proc = subprocess.run(command, cwd=work, env={**env, **agent}, capture_output=True,
                                      stdin=subprocess.DEVNULL, umask=0o022, timeout=60, check=False)
                sample = {}
                if timed:
                    wall = (time.perf_counter_ns() - start) / 1e9
                    after = resource.getrusage(resource.RUSAGE_CHILDREN)
                    sample = {"wall": wall, "user": after.ru_utime - before.ru_utime,
                              "system": after.ru_stime - before.ru_stime}
                if proc.returncode != 0 or any(marker in proc.stderr for marker in
                       (b"AddressSanitizer", b"runtime error:", b"LeakSanitizer")):
                    raise RuntimeError(f"wave {wave} {label}: rc={proc.returncode}, stderr={proc.stderr[-2000:]!r}")
                disk = {str(path.relative_to(work)): [path.stat().st_mode & 0o777,
                        hashlib.sha256(path.read_bytes()).hexdigest()]
                        for path in work.rglob("*") if path.is_file()}
                if args.profile == "append" and undo_active and label != "bash":
                    preimage = fixtures["output"][0]
                    backup = store_tmp / "shared" / ("bk_" + hashlib.sha256(preimage).hexdigest())
                    if not backup.is_file() or backup.read_bytes() != preimage:
                        raise RuntimeError(f"wave {wave} {label}: initial undo preimage missing or corrupt")
                if agent_wave and label != "bash":
                    envelopes = [json.loads(line) for line in proc.stderr.splitlines() if line.startswith(b"{")]
                    if not any("tree/d0/f0" in item.get("files_changed", {}).get("modified", []) for item in envelopes):
                        raise RuntimeError(f"missing agent observation: {proc.stderr!r}")
                return (proc.stdout, disk), sample, proc.stderr.decode(errors="replace")

            expected, _, _ = run("bash")
            for label in binaries:
                actual, _, _ = run(label)
                if actual != expected:
                    raise RuntimeError(f"wave {wave} {label}: stdout/disk mismatch; "
                                       f"stdout={actual[0][:256]!r}, expected={expected[0][:256]!r}")
            row["stdout_sha256"] = hashlib.sha256(expected[0]).hexdigest()
            row["stdout_bytes"] = len(expected[0])
            row["files"] = expected[1]
            row["returncode"] = 0
            if not args.check:
                orders = list(itertools.permutations(binaries))
                for iteration in range(args.warmup + args.runs):
                    for label in orders[(iteration + wave) % len(orders)]:
                        actual, sample, _ = run(label, timed=True)
                        if actual != expected:
                            raise RuntimeError(f"timed output mismatch: wave {wave} {label}")
                        if iteration >= args.warmup:
                            row["samples"][label].append(sample)
                row["summary"] = {}
                for label, samples in row["samples"].items():
                    times = sorted(s["wall"] for s in samples)
                    row["summary"][label] = {"median_ms": statistics.median(times) * 1000,
                        "p95_ms": times[max(0, (95 * len(times) + 99) // 100 - 1)] * 1000,
                        "cpu_ms": statistics.median(s["user"] + s["system"] for s in samples) * 1000}
            if args.trace:
                row["strace"] = {}
                for label in binaries:
                    actual, _, stderr = run(label, trace=True)
                    if actual != expected:
                        raise RuntimeError(f"trace output mismatch: wave {wave} {label}")
                    row["strace"][label] = stderr
            report["waves"].append(row)
            print(f"W{wave:02d} {name}: " + json.dumps(row.get("summary", "correctness OK")), flush=True)
        json.dump(report, output, indent=2)
        output.write("\n")


if __name__ == "__main__":
    main()
