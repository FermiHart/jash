#!/usr/bin/env python3
"""Evaluator-only workloads, calibrated batches, paired A/A or A/B, and gprof.

No timing assertions in CI. All samples are retained, including slow samples.
Reuse --plan for confirmation runs and future candidates; profiling is separate.
"""

import argparse
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import platform
import random
import resource
import shutil
import statistics
import subprocess
import time


CASES = ("literal", "quoted", "parameter", "short-circuit", "list-8",
         "function", "arithmetic-control")
ROOT = Path(__file__).resolve().parents[1]


def workload(name, iterations):
    prefix = "set -e; "
    text = "alpha beta " * 16
    bodies = {
        "literal": "test x = x",
        "quoted": f"test '{text}' = '{text}'",
        "parameter": 'test "$i" -ge 0',
        "short-circuit": "false && printf BAD || test x = x",
        "list-8": "; ".join(["test x = x"] * 8),
        "function": 'f "$i"',
        "arithmetic-control": "((hits+=1))",
    }
    if name == "function":
        prefix += 'f(){ test "$1" -ge 0; }; '
    if name == "arithmetic-control":
        prefix += "hits=0; "
    code = prefix + f"for ((i=0;i<{iterations};i++)); do {bodies[name]}; done; "
    code += "printf '%s\\n' \"$i\""
    expected = f"{iterations}\n"
    if name == "arithmetic-control":
        code += "; printf '%s\\n' \"$hits\""
        expected += f"{iterations}\n"
    return {"name": name, "iterations": iterations, "code": code, "stdout": expected}


def environment(home):
    env = {"HOME": str(home), "TMPDIR": str(home / "tmp"), "PATH": "/usr/bin:/bin",
           "LC_ALL": "C", "TZ": "UTC0", "TERM": "dumb",
           "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
           "UBSAN_OPTIONS": "halt_on_error=1", "JASH_LOOPGUARD": "off"}
    for name in ("SIGIL", "CHANGES", "MEMO", "BRIEF", "NUDGE", "WORLD", "BRAIN",
                 "DRIFT", "GUARD", "GUARD_BYPASS", "GUARD_BYPASS_STRICT", "RECALL_GUARD",
                 "RISK_CHECKPOINT", "LLM_BUDGET", "AGENT", "SAFE"):
        env["JASH_" + name] = "0"
    env["JASH_TIMELINE"] = "1"  # No filesystem mutation in these workloads.
    env["JASH_SKIP_SELFCHECK"] = "1"
    return env


def identity(path):
    path = Path(path).resolve()
    return {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}


def run_once(binary, case, directory, bash=False, trace=False):
    # Preparation/validation is outside the timed region. One live child only.
    for name in ("home", "work"):
        path = directory / name
        if path.exists():
            shutil.rmtree(path)
        path.mkdir(mode=0o700)
    home, work = directory / "home", directory / "work"
    (home / "tmp").mkdir()
    (home / ".jashrc").touch()
    env = environment(home)
    command = [str(binary), *(["--noprofile", "--norc"] if bash else []), "-c", case["code"]]
    if trace:
        command = ["/usr/bin/strace", "-f", "-c", *command]
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    start = time.perf_counter_ns()
    proc = subprocess.run(command, cwd=work, env=env, stdin=subprocess.DEVNULL,
                          capture_output=True, timeout=60, umask=0o022)
    wall = (time.perf_counter_ns() - start) / 1e6
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    if proc.returncode or proc.stdout != case["stdout"].encode() or (proc.stderr and not trace):
        raise RuntimeError(f'{case["name"]}: rc={proc.returncode}, '
                           f'stdout={proc.stdout[:200]!r}, stderr={proc.stderr[-2000:]!r}')
    # Profiling output is the sole permitted work-directory artifact.
    if any(p.name != "gmon.out" or not p.is_file() for p in work.iterdir()):
        raise RuntimeError(f'{case["name"]}: unexpected work-directory mutation')
    sample = {"wall_ms": wall, "user_ms": (after.ru_utime - before.ru_utime) * 1000,
              "system_ms": (after.ru_stime - before.ru_stime) * 1000,
              "involuntary_switches": after.ru_nivcsw - before.ru_nivcsw,
              "voluntary_switches": after.ru_nvcsw - before.ru_nvcsw,
              "minor_faults": after.ru_minflt - before.ru_minflt,
              "major_faults": after.ru_majflt - before.ru_majflt}
    sample["cpu_ms"] = sample["user_ms"] + sample["system_ms"]
    return sample, proc.stderr.decode(errors="replace")


def summary(samples):
    wall = sorted(s["wall_ms"] for s in samples)
    median = statistics.median(wall)
    return {"median_ms": median, "p95_ms": wall[math.ceil(.95 * len(wall)) - 1],
            "mad_percent": 100 * statistics.median(abs(t - median) for t in wall) / median,
            "cpu_median_ms": statistics.median(s["cpu_ms"] for s in samples),
            "median_involuntary_switches": statistics.median(s["involuntary_switches"] for s in samples)}


def paired_ratio(rows, numerator, denominator, seed, metric="wall_ms"):
    # Resample whole six-round permutation blocks, retaining local drift/order.
    blocks = [[math.log(r[numerator][metric] / r[denominator][metric])
               for r in rows[i:i + 6]] for i in range(0, len(rows), 6)]
    if len(blocks) < 2 or any(len(b) != 6 for b in blocks):
        raise ValueError("paired inference needs at least two complete six-round blocks")
    rng = random.Random(seed)
    estimates = sorted(math.exp(statistics.median(
        [value for block in rng.choices(blocks, k=len(blocks)) for value in block]))
        for _ in range(4000))
    return {"median_paired_ratio": math.exp(statistics.median(v for b in blocks for v in b)),
            "ci95": [estimates[99], estimates[3899]],
            "method": "percentile bootstrap of six-round blocks; 4000 draws; median log ratio"}


def write_json(path, data):
    with path.open("x") as stream:
        json.dump(data, stream, indent=2)
        stream.write("\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, help="Defaults to baseline: explicit A/A control")
    parser.add_argument("--output", type=Path, required=True, help="New artifact directory, never overwritten")
    parser.add_argument("--plan", type=Path, help="Reuse exact iteration counts from an earlier run")
    parser.add_argument("--cases", nargs="+", choices=CASES, default=list(CASES))
    parser.add_argument("--rounds", type=int, default=36)
    parser.add_argument("--warmups", type=int, default=6)
    parser.add_argument("--target-ms", type=float, default=150)
    parser.add_argument("--seed", type=int, default=20260910)
    parser.add_argument("--trace", action="store_true")
    parser.add_argument("--profile-binary", type=Path, help="Only profile, separately from release timing")
    parser.add_argument("--profile-scale", type=int, default=10)
    args = parser.parse_args()
    if args.rounds < 12 or args.rounds % 6 or args.warmups < 0 or args.profile_scale < 1:
        parser.error("rounds must be >=12 and divisible by 6; warmups >=0; profile-scale >=1")
    if not math.isfinite(args.target_ms) or not 50 <= args.target_ms <= 2000:
        parser.error("target-ms must be between 50 and 2000")
    if args.profile_binary and not args.plan:
        parser.error("profiling requires a calibrated --plan")
    if len(set(args.cases)) != len(args.cases):
        parser.error("duplicate cases")
    args.output = args.output.absolute()
    args.output.mkdir(mode=0o700)  # Refuse an existing directory, including symlinks.
    binaries = {"baseline": args.baseline.resolve(),
                "candidate": (args.candidate or args.baseline).resolve(), "bash": Path("/usr/bin/bash")}
    identities = {label: identity(path) for label, path in binaries.items()}
    report = {"schema": 1, "runner": identity(__file__), "binaries": identities,
              "aa": identities["baseline"]["sha256"] == identities["candidate"]["sha256"],
              "platform": platform.platform(),
              "affinity": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
              "argv": list(os.sys.argv), "rounds": args.rounds, "warmups": args.warmups,
              "environment": environment(args.output.resolve() / "home"),
              "order": "all six permutations, shuffled within every six-round block",
              "seed": args.seed, "started_unix": time.time(), "load_start": os.getloadavg(),
              "isolation": "fresh HOME/work per invocation; warm executable/page caches; TZ=UTC0",
              "scope": "CPU-oriented evaluator; undo enabled, agent/Sigil hooks disabled explicitly",
              "profiling": bool(args.profile_binary), "calibration": {}, "results": []}
    profile_identity = identity(args.profile_binary) if args.profile_binary else None
    report["profile_binary"] = profile_identity
    write_json(args.output / "metadata.json", report)
    try:
        if args.plan:
            loaded = json.loads(args.plan.read_text())
            plan = [row for row in loaded if row["name"] in args.cases]
            if len(plan) != len(args.cases) or {row["name"] for row in plan} != set(args.cases):
                raise ValueError("plan does not cover requested cases exactly")
            if any(not isinstance(row["iterations"], int) or row["iterations"] < 1 or
                   row != workload(row["name"], row["iterations"]) for row in plan):
                raise ValueError("plan differs from this runner's workload contract")
        else:
            plan = []
            for name in args.cases:
                n = 1000
                attempts = []
                for _ in range(4):
                    case = workload(name, n)
                    sample, _ = run_once(binaries["baseline"], case, args.output)
                    attempts.append({"iterations": n, **sample})
                    if .8 * args.target_ms <= sample["wall_ms"] <= 1.3 * args.target_ms:
                        break
                    n = max(1, min(2_000_000, round(n * args.target_ms / sample["wall_ms"])))
                plan.append(workload(name, n))
                report["calibration"][name] = attempts
        write_json(args.output / "plan.json", plan)
        rng = random.Random(args.seed)
        for case in plan:
            if args.profile_binary:
                from eval_gmon import inspect
                scaled = workload(case["name"], case["iterations"] * args.profile_scale)
                run_once(args.profile_binary.resolve(), scaled, args.output)
                gmon = args.output / "work/gmon.out"
                if not gmon.is_file():
                    raise RuntimeError("profile binary did not produce gmon.out")
                saved = args.output / (case["name"] + ".gmon")
                gmon.rename(saved)
                profile = subprocess.check_output(["gprof", "-b", str(args.profile_binary.resolve()), str(saved)])
                (args.output / (case["name"] + ".gprof.txt")).write_bytes(profile)
                decoded = inspect(args.profile_binary, saved)
                n = scaled["iterations"]
                # Sanity anchors prevent plausible-looking but misattributed profiles.
                expected_calls = {
                    "jash_builtin_test_argv": 0 if case["name"] == "arithmetic-control" else n * (8 if case["name"] == "list-8" else 1),
                    "jash_b_printf": 2 if case["name"] == "arithmetic-control" else 1,
                    "jash_eval_function": n if case["name"] == "function" else 0,
                }
                for name, count in expected_calls.items():
                    if decoded["calls"].get(name, 0) != count:
                        raise RuntimeError(f"profile attribution failed for {name}: expected {count}")
                decoded["anchor_calls"] = expected_calls
                write_json(args.output / (case["name"] + ".counts.json"), decoded)
                report["results"].append({**scaled, "profile_binary": identity(args.profile_binary),
                                          "decoder": identity(Path(__file__).with_name("eval_gmon.py")),
                                          "counts": decoded})
                print(case["name"], "profile captured", scaled["iterations"], flush=True)
                continue
            row = {**case, "rounds": []}
            for iteration in range(args.warmups):
                labels = list(binaries)
                rng.shuffle(labels)
                for label in labels:
                    run_once(binaries[label], case, args.output, bash=label == "bash")
            with (args.output / (case["name"] + ".samples.jsonl")).open("x") as evidence:
                for block in range(args.rounds // 6):
                    orders = list(itertools.permutations(binaries))
                    rng.shuffle(orders)
                    for order in orders:
                        result = {"block": block, "order": order}
                        for label in order:
                            result[label], _ = run_once(binaries[label], case, args.output, bash=label == "bash")
                        row["rounds"].append(result)
                        evidence.write(json.dumps(result) + "\n")
                        evidence.flush()
            row["summary"] = {label: summary([r[label] for r in row["rounds"]]) for label in binaries}
            row["candidate_vs_baseline"] = paired_ratio(row["rounds"], "candidate", "baseline", args.seed)
            row["cpu_candidate_vs_baseline"] = paired_ratio(row["rounds"], "candidate", "baseline", args.seed, "cpu_ms")
            row["candidate_vs_bash"] = paired_ratio(row["rounds"], "candidate", "bash", args.seed)
            ratio = row["candidate_vs_baseline"]
            row["aa_noise_gate"] = (abs(ratio["median_paired_ratio"] - 1) <= .03 and
                                     ratio["ci95"][0] <= 1 <= ratio["ci95"][1]) if report["aa"] else None
            row["aa_precision_within_3pct"] = (.97 <= ratio["ci95"][0] and
                                               ratio["ci95"][1] <= 1.03) if report["aa"] else None
            if args.trace:
                row["strace"] = {label: run_once(binary, case, args.output, bash=label == "bash", trace=True)[1]
                                 for label, binary in binaries.items()}
            report["results"].append(row)
            print(case["name"], "N=", case["iterations"], json.dumps(row["summary"]),
                  "A/A gate:", row["aa_noise_gate"], "ratio:", ratio, flush=True)
        if any(identity(binaries[label]) != info for label, info in identities.items()):
            raise RuntimeError("binary changed during measurement")
        if profile_identity and identity(args.profile_binary) != profile_identity:
            raise RuntimeError("profile binary changed during collection")
        if identity(__file__) != report["runner"]:
            raise RuntimeError("runner changed during measurement")
        report["load_end"] = os.getloadavg()
        report["finished_unix"] = time.time()
        write_json(args.output / "report.json", report)
    except Exception as exc:
        write_json(args.output / "failure.json", {"error": str(exc), "partial": report})
        raise


if __name__ == "__main__":
    main()
