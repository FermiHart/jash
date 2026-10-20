#!/usr/bin/env python3
"""W06 executable evidence matrix. Missing cases remain explicit denominator debt."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run_case(binary, profile, root, env, argv, *, invalid=False, identifier=None):
    request = {"argv": argv, "cwd": str(root)}
    if invalid: request["unknown_matrix_field"] = True
    if profile == "agent-strict":
        request.update(id=identifier or root.name + ("-invalid" if invalid else "-valid"), caps=["effect:read", "effect:mutate"])
        result = subprocess.run([str(binary)], input=(json.dumps(request) + "\n").encode(),
                                cwd=root, env=env, capture_output=True, timeout=20)
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        row = rows[-1]
        if invalid: return row.get("rc", 2 if "error" in row else 0), row.get("stdout", "")
        return row.get("rc", -1), row.get("stdout", json.dumps(row))
    result = subprocess.run([str(binary), "--exec-json", json.dumps(request)],
                            cwd=root, env=env, capture_output=True, timeout=20)
    return result.returncode, result.stdout.decode("utf-8")


CASES = {
    "cp": (["cp", "--json", "source", "target"], "copy"),
    "mv": (["mv", "--json", "--atomic", "source", "target"], "move"),
    "rm": (["rm", "--json", "source"], "remove"),
    "touch": (["touch", "--json", "target"], "touch"),
    "mkdir": (["mkdir", "--json", "target"], "mkdir"),
    "rmdir": (["rmdir", "--json", "empty"], "rmdir"),
    "ln": (["ln", "--json", "source", "target"], "link"),
    "chmod": (["chmod", "--json", "0700", "source"], "mode"),
    "chown": (["chown", "--json", str(os.getuid()), "source"], "owner"),
    "sed": (["sed", "--json", "-i", "s/old/new/", "source"], "edit"),
    "df": (["df", "--json-report", "."], "query"),
    "timeline": (["timeline", "--json"], "state"),
    "stat": (["stat", "--json-report", "source"], "stat"),
    "wc": (["wc", "--json-report", "source"], "wc"),
    "checkpoint": (["checkpoint", "--json"], "checkpoint"),
}


def check_effect(root, kind):
    source, target = root / "source", root / "target"
    if kind == "copy": return target.read_text() == "old\n" and source.exists()
    if kind == "move": return target.read_text() == "old\n" and not source.exists()
    if kind == "remove": return not source.exists()
    if kind == "touch": return target.is_file()
    if kind == "mkdir": return target.is_dir()
    if kind == "rmdir": return not (root / "empty").exists()
    if kind == "link": return target.stat().st_ino == source.stat().st_ino
    if kind == "mode": return source.stat().st_mode & 0o777 == 0o700
    if kind == "owner": return source.stat().st_uid == os.getuid()
    if kind == "edit": return source.read_text() == "new\n"
    return source.read_text() == "old\n" and not target.exists()


def lifecycle(binary, profile, base, env):
    if profile != "agent-strict":
        return {"supported": False, "reason": "general exec-json does not advertise receipt replay"}
    root = base / "lifecycle"; root.mkdir(); (root / "source").write_text("data")
    request = {"id": "lifecycle-move", "argv": ["mv", "-n", "source", "target"],
               "cwd": str(root), "caps": ["effect:mutate"]}
    conflict = {**request, "argv": ["rm", "target"]}
    wire = "\n".join(json.dumps(r) for r in (request, request, conflict)) + "\n"
    result = subprocess.run([str(binary)], input=wire.encode(), cwd=root, env=env, capture_output=True, timeout=20)
    rows = [json.loads(line) for line in result.stdout.splitlines() if line.startswith(b'{"id":')]
    return {"supported": True, "exact_replay": len(rows) == 3 and rows[0] == rows[1] and rows[0].get("rc") == 0,
            "conflict_denied": len(rows) == 3 and rows[2].get("rc") != 0,
            "effect_once": not (root / "source").exists() and (root / "target").read_text() == "data"}


def matrix(binary):
    binary = Path(binary).resolve()
    with tempfile.TemporaryDirectory(prefix="jash-matrix-") as tmp:
        base = Path(tmp)
        env = {k: v for k, v in os.environ.items() if not k.startswith("JASH_")}
        env.update(HOME=str(base), JASH_SIGIL="0", JASH_TIMELINE="0", JASH_MEMO="0",
                   JASH_ADOPT="0", JASH_NUDGE="0", JASH_BRIEF="0", JASH_LOOPGUARD="off")
        inventory = json.loads(subprocess.check_output([str(binary), "--contract-json"], env=env, timeout=20))
        rows = []
        for operation in inventory["operations"]:
            row = {"id": operation["id"], "qualification": "unqualified", "observations": {}}
            if operation["kind"] == "builtin" and operation["name"] in CASES:
                root = base / operation["name"]; root.mkdir()
                (root / "source").write_text("old\n"); (root / "empty").mkdir()
                argv, effect = CASES[operation["name"]]
                if effect == "checkpoint":
                    (base / ".jash").mkdir(mode=0o700, exist_ok=True)
                    setup_rc, _ = run_case(binary, inventory["profile"], root, env, ["checkpoint", "matrix"], identifier="matrix-checkpoint-setup")
                    if setup_rc: raise RuntimeError("checkpoint fixture creation failed")
                # A malformed request must fail before the same operation runs.
                bad_rc, _ = run_case(binary, inventory["profile"], root, env, argv, invalid=True)
                untouched = (root / "source").read_text() == "old\n" and not (root / "target").exists() and (root / "empty").exists()
                rc, raw = run_case(binary, inventory["profile"], root, env, argv)
                try:
                    data = json.loads(raw)
                    shape = isinstance(data, dict) and "schema" in data
                    correct = check_effect(root, effect)
                    if effect == "stat":
                        item = data["items"][0]
                        correct = correct and item["size"] == (root / "source").stat().st_size and int(item["inode"]) == (root / "source").stat().st_ino
                    if effect == "checkpoint":
                        correct = correct and any(c["name"] == "matrix" for c in data["checkpoints"])
                    if effect == "wc":
                        correct = correct and data["total"]["bytes"] == 4 and data["total"]["words"] == 1 and data["total"]["lines"] == 1
                    if effect == "move":
                        correct = correct and data["transaction"] == "committed"
                except (ValueError, OSError): shape = correct = False
                row["observations"] = {"unknown_field_before_effect": bad_rc != 0 and untouched,
                                       "result_document": rc == 0 and shape, "effect": rc == 0 and correct}
                row["qualification"] = "partial" if all(row["observations"].values()) else "failed"
                if row["qualification"] == "failed": row["failure"] = {"rc": rc, "output": raw[:2048]}
                row["argv"] = argv
                if effect == "move":
                    (root / "first").write_text("new"); (root / "second").write_text("second")
                    (root / "destination").mkdir(); (root / "destination/first").write_text("old")
                    (root / "destination/second").mkdir()
                    rollback_rc, rollback_raw = run_case(binary, inventory["profile"], root, env,
                        ["mv", "--json", "--atomic", "first", "second", "destination"], identifier="matrix-atomic-rollback")
                    rollback = json.loads(rollback_raw)
                    row["observations"]["atomic_rollback"] = (rollback_rc == 1 and rollback["transaction"] == "rolled_back" and
                        rollback["counts"]["completed"] == 0 and (root / "first").read_text() == "new" and
                        (root / "destination/first").read_text() == "old" and (root / "second").read_text() == "second")
                    if not row["observations"]["atomic_rollback"]: row["qualification"] = "failed"
                row["remaining_dimensions"] = ["option_combinations", "all_byte_inputs", "lifecycle_and_fault_matrix"]
            rows.append(row)
        # Exercise a usable reference, not merely an error-shaped undo response.
        undo_root = base / "undo"; undo_root.mkdir(); (undo_root / "source").write_text("data")
        undo_env = dict(env, JASH_TIMELINE="1")
        setup_rc, setup_raw = run_case(binary, inventory["profile"], undo_root, undo_env,
                                      ["cp", "--json", "source", "target"], identifier="matrix-undo-setup")
        reference = json.loads(setup_raw)["items"][0]["undo"] if setup_rc == 0 else None
        observations = {"unknown_field_before_effect": False, "result_document": False, "effect": False}
        if reference:
            argv = ["undo", "--json", "--ref", reference["ref"]]
            bad_rc, _ = run_case(binary, inventory["profile"], undo_root, undo_env, argv, invalid=True, identifier="matrix-undo-bad")
            observations["unknown_field_before_effect"] = bad_rc != 0 and (undo_root / "target").exists()
            rc, raw = run_case(binary, inventory["profile"], undo_root, undo_env, argv, identifier="matrix-undo-apply")
            try: observations["result_document"] = rc == 0 and json.loads(raw)["status"] == "completed"
            except (ValueError, KeyError): pass
            observations["effect"] = rc == 0 and not (undo_root / "target").exists() and (undo_root / "source").read_text() == "data"
        for row in rows:
            if row["id"] == "builtin:undo":
                row["observations"] = observations
                row["qualification"] = "partial" if all(observations.values()) else "failed"
                row["remaining_dimensions"] = ["legacy_undo", "all_inverse_kinds", "interrupted_restore"]
        for invocation in ("serve", "exec-json", "listen"):
            descriptor = json.loads(subprocess.check_output([str(binary), "--contract-json", "invocation:" + invocation], env=env, timeout=20))
            rows.append({"id": descriptor["id"], "availability": descriptor["availability"],
                         "qualification": "unqualified", "observations": {},
                         "remaining_dimensions": ["invocation_lifecycle_matrix"]})
        life = lifecycle(binary, inventory["profile"], base, env)
        life_failed = life["supported"] and not all(value for key, value in life.items() if key != "supported")
        return {"schema": "jash.contract-matrix.v1", "profile": inventory["profile"], "target": inventory["target"],
                "lifecycle_observations": life,
                "inventory_scope": "compiled-builtins-job-operations-and-known-invocations",
                "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(), "complete": False,
                "denominator": len(rows), "exercised": sum(bool(r["observations"]) for r in rows),
                "failed": sum(r["qualification"] == "failed" for r in rows) + int(life_failed), "operations": rows}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default=str(ROOT / "build/jash"))
    parser.add_argument("--require-complete", action="store_true")
    parser.add_argument("--output", help="write a new evidence artifact (existing files are refused)")
    options = parser.parse_args()
    result = matrix(options.binary)
    encoded = json.dumps(result, separators=(",", ":"))
    if options.output:
        with open(options.output, "x", encoding="utf-8") as output: output.write(encoded + "\n")
        print(json.dumps({k: result[k] for k in ("profile", "denominator", "exercised", "failed", "complete")}))
    else: print(encoded)
    return 1 if result["failed"] or (options.require_complete and not result["complete"]) else 0


if __name__ == "__main__": raise SystemExit(main())
