#!/usr/bin/env python3
"""Measure opt-in contract discovery separately from ordinary shell startup.

Writes an exclusive evidence file. This is a baseline, not a speedup claim.
Iterations use a private HOME/cwd; raw times and binary identities are retained.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import statistics
import subprocess
import tempfile
import time


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int((len(ordered) - 1) * fraction))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, action="append", required=True)
    parser.add_argument("--iterations", type=int, default=100)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 10 <= args.iterations <= 2000:
        parser.error("iterations must be in 10..2000")
    root = Path(__file__).resolve().parents[1]
    evidence = {"kind": "agent-contract-baseline-v1", "platform": platform.platform(),
                "head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
                "iterations": args.iterations, "seed": 56, "binaries": [],
                "limitations": ["No pre-change binary comparison; no speedup claimed.",
                                "Warm repeated process launches, not cold-cache or protocol throughput.",
                                "Each sample includes subprocess launch, pipe output capture and wait."]}
    with tempfile.TemporaryDirectory(prefix="jash-contract-bench-") as tmp:
        env = {k: v for k, v in os.environ.items() if not k.startswith("JASH_")}
        env.update(HOME=tmp, PATH="/usr/bin:/bin", LC_ALL="C", TZ="UTC",
                   JASH_SIGIL="0", JASH_ADOPT="0", JASH_MEMO="0", JASH_NUDGE="0",
                   JASH_BRIEF="0", JASH_LOOPGUARD="off", JASH_WORLD="0")
        for path in args.binary:
            binary = str(path.resolve())
            inventory = json.loads(subprocess.check_output([binary, "--contract-json"], cwd=tmp, env=env))
            cases = {"contract_full": ["--contract-json"],
                     "contract_one": ["--contract-json", "runtime:job.read"]}
            if inventory["profile"] == "general":
                cases.update(startup_true=["-c", "true"], builtins=["--builtins-json"])
            digest = hashlib.sha256()
            with open(binary, "rb") as handle:
                for chunk in iter(lambda: handle.read(131072), b""):
                    digest.update(chunk)
            record = {"path": binary, "sha256": digest.hexdigest(), "bytes": Path(binary).stat().st_size,
                      "profile": inventory["profile"], "coverage": inventory["coverage"], "cases": {}}
            timings = {name: [] for name in cases}
            for name, argv in cases.items():
                initial = subprocess.run([binary, *argv], cwd=tmp, env=env, capture_output=True, timeout=15, check=True)
                record["cases"][name] = {"argv": argv, "stdout_bytes": len(initial.stdout)}
            rng = random.Random(56)
            for _ in range(args.iterations):
                order = list(cases)
                rng.shuffle(order)
                for name in order:
                    start = time.perf_counter_ns()
                    # PIPE makes communicate wait on readiness instead of
                    # timeout's exponential waitpid polling with DEVNULL.
                    subprocess.run([binary, *cases[name]], cwd=tmp, env=env, capture_output=True,
                                   timeout=15, check=True)
                    timings[name].append(time.perf_counter_ns() - start)
            for name, values in timings.items():
                record["cases"][name].update(raw_ns=values, median_ns=statistics.median(values),
                                            p95_ns=percentile(values, 0.95), p99_ns=percentile(values, 0.99))
            evidence["binaries"].append(record)
    with args.output.open("x", encoding="utf-8") as handle:
        json.dump(evidence, handle, indent=2)
        handle.write("\n")
    for binary in evidence["binaries"]:
        for name, stats in binary["cases"].items():
            print(f"{binary['profile']} {name}: median={stats['median_ns']/1e6:.3f} ms "
                  f"p95={stats['p95_ns']/1e6:.3f} ms stdout={stats['stdout_bytes']} B")
    print(f"evidence: {args.output}")


if __name__ == "__main__":
    main()
