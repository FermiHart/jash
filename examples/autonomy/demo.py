#!/usr/bin/env python3
"""Deterministic fixture comparison, not an LLM A/B or performance benchmark."""
import argparse
from contextlib import ExitStack
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT))
from tools import jash_autonomy as workflow


def demonstrate(root, build):
    agentd = (build / "jash-agentd").resolve(strict=True)
    verifier = (build / "jef_verify").resolve(strict=True)
    for binary in (agentd, verifier, Path("/usr/bin/python3"), Path("/usr/bin/git")):
        workflow.require(binary.is_file() and os.access(binary, os.X_OK),
                         "required executable unavailable: " + str(binary))
    repo = root / "repo"
    repo.mkdir(mode=0o700)
    env = {**workflow.ENV, "HOME": str(root), "GIT_CONFIG_NOSYSTEM": "1",
           "GIT_CONFIG_GLOBAL": "/dev/null"}
    subprocess.run(["/usr/bin/git", "init", "-q", "--template=", str(repo)],
                   env=env, check=True, capture_output=True, timeout=10)
    target = repo / "calculator.py"
    checker = repo / "check.py"
    original = b"def add(a, b):\n    return a - b\n"
    correct = b"def add(a, b):\n    return a + b\n"
    frozen_check = (
        b"from calculator import add\n"
        b"for a, b, expected in ((2, 3, 5), (5, -2, 3), (-4, -6, -10), (0, 0, 0)):\n"
        b"    actual = add(a, b)\n"
        b"    if actual != expected:\n"
        b"        print(f'FAIL: add({a}, {b}) = {actual}; expected {expected}')\n"
        b"        raise SystemExit(1)\n"
        b"print('PASS: fixed sum cases')\n"
    )
    workflow.put(target, original, 0o644)
    workflow.put(checker, frozen_check, 0o644)
    # An indexed fixture needs neither a commit nor user Git configuration.
    subprocess.run(["/usr/bin/git", "-C", str(repo), "add", "calculator.py", "check.py"],
                   env=env, check=True, capture_output=True, timeout=10)
    index = (repo / ".git/index").read_bytes()
    check = ["/usr/bin/python3", "-B", "check.py"]
    baseline = subprocess.run(check, cwd=repo, env=env, capture_output=True, timeout=10)
    workflow.require(baseline.returncode == 1, "buggy fixture unexpectedly passed")
    generators = [
        ["/usr/bin/python3", "-B", "-c",
         "from pathlib import Path; "
         "Path('calculator.py').write_bytes(b'def add(a, b):\\n    return a + abs(b)\\n')"],
        ["/usr/bin/python3", "-B", "-c",
         "from pathlib import Path; "
         "Path('calculator.py').write_bytes(b'def add(a, b):\\n    return a + b\\n')"],
    ]
    bundle = root / "bundle"
    result = workflow.run(argparse.Namespace(
        repo=repo, target="calculator.py", bundle=bundle, runtime_home=root / "runtime",
        agentd=agentd, verifier=verifier, timeout_ms=2000,
        generate=[json.dumps(argv) for argv in generators], check=json.dumps(check)))
    workflow.put(root / "run.json", workflow.wire(result))
    # Demo-only trust bootstrap from OUR fresh daemon, never an external bundle.
    key = result["local_pubkey"]
    workflow.put(root / "demo-pubkey.txt", (key + "\n").encode("ascii"))

    def cli(verb, *extra, expected_rc=0):
        response = subprocess.run(
            [sys.executable, "-B", str(ROOT / "tools/jash_autonomy.py"), verb,
             "--bundle", str(bundle), "--pubkey", key, "--verifier", str(verifier),
             *map(str, extra)], capture_output=True, timeout=45)
        workflow.require(response.returncode == expected_rc,
                         f"{verb}: unexpected rc {response.returncode}: "
                         + response.stdout.decode("utf-8", errors="replace")
                         + response.stderr.decode("utf-8", errors="replace"))
        return workflow.decode(response.stdout)

    verified = cli("verify")
    workflow.put(root / "verified.json", workflow.wire(verified))
    evidence = verified["evidence"]
    workflow.require(verified["verified"] and verified["winner"] == result["winner"] == "c1",
                     "independent verification did not select c1")
    workflow.require([(e["id"], e["generation_rc"], e["check_rc"], e["eligible"])
                      for e in evidence] == [("c0", 0, 1, False), ("c1", 0, 0, True)],
                     "unexpected candidate outcomes")
    published = cli("publish", "--repo", repo, "--destination", root / "published")
    workflow.put(root / "publication.json", workflow.wire(published))
    workflow.require((root / "published/candidate").read_bytes() == correct,
                     "published bytes differ from the checked fix")
    workflow.require(target.read_bytes() == original and checker.read_bytes() == frozen_check
                     and (repo / ".git/index").read_bytes() == index,
                     "workflow changed original target, checker or Git index")
    handoff = cli("handoff", "--repo", repo)
    workflow.require(handoff["status"] == "current", "fresh handoff is not current")
    workflow.put(root / "handoff-current.json", workflow.wire(handoff))

    # Deliberate caller edit AFTER publication, not a workflow live-tree apply.
    target.write_bytes(original + b"# deliberate post-result source edit\n")
    handoff = cli("handoff", "--repo", repo)
    workflow.require(handoff["status"] == "invalidated", "stale source was not invalidated")
    workflow.put(root / "handoff-invalidated.json", workflow.wire(handoff))
    refused = cli("publish", "--repo", repo, "--destination", root / "refused",
                  expected_rc=1)
    workflow.require(refused.get("verified") is True
                     and refused["publication"]["state"] == "blocked"
                     and "source baseline invalidated" in refused.get("error", "")
                     and not (root / "refused").exists(), "stale publication was not refused")
    workflow.put(root / "publication-refused.json", workflow.wire(refused))
    output_inode = (root / "published").stat().st_ino
    recovered = cli("recover", "--repo", repo, "--destination", root / "published")
    workflow.require(recovered["publication"]["state"] == "committed"
                     and recovered["publication"]["baseline_current"] is False
                     and (root / "published").stat().st_ino == output_inode,
                     "reconciliation replaced output or required a fresh baseline")
    workflow.put(root / "publication-recovered.json", workflow.wire(recovered))
    workflow.require((repo / ".git/index").read_bytes() == index
                     and checker.read_bytes() == frozen_check
                     and target.read_bytes() == original + b"# deliberate post-result source edit\n"
                     and (root / "published/candidate").read_bytes() == correct,
                     "stale-source operations modified fixture or published artifact")
    return {
        "ok": True,
        "comparison": "deterministic fixture: naive rc-only acceptance vs verified winner; not LLM A/B",
        "baseline_check_rc": baseline.returncode,
        "candidates": [{k: e[k] for k in ("id", "generation_rc", "check_rc", "eligible")}
                       for e in evidence],
        "naive_rc_only": {"accepted": [e["id"] for e in evidence if e["generation_rc"] == 0],
                          "first_accepted": "c0"},
        "verified_winner": verified["winner"],
        "false_success_caught": "c0: generator rc 0, frozen checker rc 1",
        "source_unchanged_before_deliberate_edit": True,
        "index_unchanged": True,
        "checker_unchanged": True,
        "publication_scope": published["scope"],
        "handoff_after_source_edit": handoff["status"],
        "subsequent_publication_refused": True,
        "owned_publication_recovered": recovered["publication"]["state"],
        "key_trust": "fresh local demo runtime only; not external authentication",
        "not_measured": ["human time savings", "token savings", "model quality", "production readiness"],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build",
                        help="existing build directory; never rebuilt")
    parser.add_argument("--output", type=Path, metavar="NEWDIR",
                        help="retain fixture, receipts and DEMO PRIVATE KEY in a new directory")
    args = parser.parse_args()
    started = time.monotonic()
    root = None
    try:
        with ExitStack() as stack:
            if args.output is None:
                root = Path(stack.enter_context(tempfile.TemporaryDirectory(
                    prefix="autonomy-demo-", dir="/tmp")))
            else:
                root = workflow.new_path(args.output)
                root.mkdir(mode=0o700)
            report = demonstrate(root, args.build)
            report.update(duration_seconds=round(time.monotonic() - started, 6),
                          artifacts=str(root), artifacts_persist=args.output is not None)
            workflow.put(root / "report.json", workflow.wire(report))
            print(json.dumps(report, indent=2, sort_keys=True))
        return 0
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, subprocess.SubprocessError) as exc:
        print(json.dumps({"ok": False, "error": str(exc),
                          "duration_seconds": round(time.monotonic() - started, 6),
                          "artifacts": str(root) if root else None,
                          "artifacts_persist": args.output is not None and root is not None},
                         indent=2, sort_keys=True))
        return 1


if __name__ == "__main__":
    sys.exit(main())
