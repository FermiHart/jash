#!/usr/bin/env python3
"""Render compiled contract metadata; no inferred flags or capability claims.

No third-party dependencies. Use --binary for the exact build/profile to inspect.
Only calls the opt-in discovery entry point, never an operation from the inventory.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys


def cell(value):
    return str(value).replace("\\", "\\\\").replace("|", "\\|").replace("`", "&#96;").replace("\n", " ").replace("\r", " ")


def markdown(document):
    if document["schema"] == "jash.agent-contract.invocation.v1":
        result = ["# " + cell(document["id"]), "",
                  f"Profile: **{cell(document['profile'])}**; availability: **{cell(document['availability'])}**.",
                  "", "Structural discovery; semantic validation remains mandatory.", "",
                  "| Field | Type |", "|---|---|"]
        for name in document["allowed_fields"]:
            result.append(f"| {cell(name)} | {cell(document['field_types'][name])} |")
        result += ["", "request_version exclusively aliases protocol_version.", ""]
        return "\n".join(result)
    result = ["# Agent contract inventory", "",
              f"Profile: **{cell(document['profile'])}**; target: **{cell(document['target'])}**.",
              "", "Compiled availability is not behavioral qualification. Pending schemas/options are unknown, not empty.",
              "", "| Operation ID | Contract | Summary |", "|---|---|---|"]
    for row in document["operations"]:
        result.append(f"| {cell(row['id'])} | {cell(row['contract_status'])} | {cell(row['summary'])} |")
    result += ["", "## Runtime structural arguments", ""]
    for row in document["operations"]:
        if row["kind"] != "runtime_operation":
            continue
        result += ["### " + cell(row["id"]), "", "| Field | Type | Required |", "|---|---|---|"]
        for name in row["allowed_fields"]:
            result.append(f"| {cell(name)} | {cell(row['field_types'][name])} | {'yes' if name in row['required_fields'] else 'no'} |")
        result += ["", "request_version is an exclusive alias of protocol_version. Server semantic checks remain mandatory.", ""]
    return "\n".join(result) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path(__file__).resolve().parents[1] / "build/jash")
    parser.add_argument("--operation", help="exact stable ID, e.g. runtime:job.start")
    parser.add_argument("--format", choices=("json", "markdown"), default="json")
    args = parser.parse_args()
    command = [str(args.binary.resolve()), "--contract-json"]
    if args.operation is not None:
        command.append(args.operation)
    try:
        run = subprocess.run(command, capture_output=True, timeout=15, check=False)
        if run.returncode:
            sys.stderr.buffer.write(run.stderr or run.stdout)
            return 1
        data = json.loads(run.stdout)
        if data.get("schema") not in ("jash.agent-contract.inventory.v1", "jash.agent-contract.invocation.v1") or data.get("version") != 1:
            raise ValueError("unsupported inventory schema")
        if args.format == "json":
            print(json.dumps(data, ensure_ascii=False, sort_keys=True, indent=2))
        else:
            print(markdown(data), end="")
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as exc:
        print(f"agent contract: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
