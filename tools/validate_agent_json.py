#!/usr/bin/env python3
"""Independent Draft 2020-12 validation of emitted agent documents.

Install tests/requirements-agent-json.txt in a private venv for qualification.
Missing validator is an error, never a silently passed/skipped schema gate.
"""
import json
import os
from pathlib import Path
import runpy
import shlex
import subprocess
import tempfile
import socket
import array
import time
import base64

from jsonschema import Draft202012Validator


def local_control_document(build, root, env, validator, data_validator, stream_validator):
    key = root / "local-scope"; key.write_text("1" * 64); key.chmod(0o600)
    path = root / "local-socket"
    server = subprocess.Popen([str(build / "jash-agentd"), "--listen", str(path), "--scope-file", str(key)],
                              cwd=root, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    client = socket.socket(socket.AF_UNIX); client.settimeout(5)
    stream = None; lane = None; received = []
    try:
        deadline = time.monotonic() + 5
        while True:
            try: client.connect(str(path)); break
            except (FileNotFoundError, ConnectionRefusedError):
                if server.poll() is not None or time.monotonic() >= deadline: raise
                time.sleep(.01)
        stream = client.makefile("rwb", buffering=0)
        json.loads(stream.readline())
        stream.write((json.dumps({"scope": "1" * 64, "control": "fd-v1", "data": "fd-v1"}, separators=(",", ":")) + "\n").encode())
        validator.validate(json.loads(stream.readline()))
        data, ancillary, flags, _ = client.recvmsg(1, socket.CMSG_SPACE(4), socket.MSG_CMSG_CLOEXEC)
        for level, kind, payload in ancillary:
            if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                values = array.array("i"); values.frombytes(payload); received.extend(values)
        if data != b"C" or flags & socket.MSG_CTRUNC or len(received) != 1: raise RuntimeError("invalid control attachment")
        data_validator.validate(json.loads(stream.readline()))
        data, ancillary, flags, _ = client.recvmsg(1, socket.CMSG_SPACE(4), socket.MSG_CMSG_CLOEXEC)
        for level, kind, payload in ancillary:
            if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                values = array.array("i"); values.frombytes(payload); received.extend(values)
        if data != b"D" or flags & socket.MSG_CTRUNC or len(received) != 2: raise RuntimeError("invalid data attachment")
        handshake = json.loads(stream.readline())
        if not handshake["cancel"]["enabled"]: raise RuntimeError("control unavailable after attachment")
        lane = socket.socket(fileno=received.pop())
        def exchange(request):
            stream.write((json.dumps(request, separators=(",", ":")) + "\n").encode())
            return json.loads(stream.readline())
        started = exchange({"id":"schema-stream-job","protocol_version":2,"op":"job.start",
                            "argv":["/usr/bin/printf","schema-stream"],"background":True,
                            "caps":["effect:exec","job:create"]})
        terminal = exchange({"id":"schema-stream-wait","protocol_version":2,"op":"job.wait",
                             "job_id":started["job_id"],"wait_ms":5000,"caps":["job:read"]})
        if not terminal["terminal"]: raise RuntimeError(terminal)
        opened = exchange({"id":"schema-stream-open","protocol_version":2,"op":"data.stream.open",
                           "job_id":started["job_id"],"stream":"stdout","offset":0,"caps":["job:read"]})
        stream_validator.validate(opened)
        credited = exchange({"id":"schema-stream-credit","protocol_version":2,"op":"data.stream.credit",
                             "stream_ref":opened["stream_ref"],"credit_bytes":64,"ack_offset":0,
                             "caps":["job:read"]})
        stream_validator.validate(credited)
        packet = lane.recv(96 + 16384)
        if packet[:4] != b"JDS1" or packet[96:] != b"schema-stream": raise RuntimeError("invalid JDS1 document fixture")
        cancelled = exchange({"id":"schema-stream-cancel","protocol_version":2,"op":"data.stream.cancel",
                              "stream_ref":opened["stream_ref"],"caps":["job:read"]})
        stream_validator.validate(cancelled)
        return 5
    finally:
        if lane: lane.close()
        for fd in received: os.close(fd)
        if stream: stream.close()
        client.close()
        server.terminate()
        try: server.communicate(timeout=5)
        except subprocess.TimeoutExpired: server.kill(); server.communicate(timeout=5)


def data_documents(build, directory, env, validators):
    count = 0
    def start():
        proc = subprocess.Popen([str(build / "jash-agentd")], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=directory, env=env)
        json.loads(proc.stdout.readline()); return proc
    def send(proc, op, schema, **fields):
        nonlocal count
        request = dict(id="schema-data-" + op, protocol_version=2, op=op, caps=["store:read", "store:manage", "data:create", "data:read", "data:manage"], **fields)
        proc.stdin.write((json.dumps(request) + "\n").encode()); proc.stdin.flush()
        response = json.loads(proc.stdout.readline()); validators[schema].validate(response)
        if not response["ok"]: raise RuntimeError(response)
        count += 1; return response
    def stop(proc):
        proc.stdin.close(); proc.stdin = None
        try: proc.communicate(timeout=10)
        except subprocess.TimeoutExpired: proc.kill(); proc.communicate(timeout=10)
    proc = start()
    try:
        send(proc, "budget.get", "agent-budget-v1.schema.json")
        policy = send(proc, "data.policy.get", "agent-data-policy-v1.schema.json")
        changed = send(proc, "data.policy.set", "agent-data-policy-v1.schema.json", revision=policy["revision"], max_bytes=1048576, fd_enabled=True)
        proof = json.loads(Path(directory, ".jash/agent_data_policy_history", changed["intent_receipt_ref"]).read_bytes())
        validators["agent-data-policy-intent-v1.schema.json"].validate(json.loads(base64.b64decode(proof["payload"]))); count += 1
        send(proc, "data.probe", "agent-data-probe-v1.schema.json")
        buffer = send(proc, "data.import", "agent-data-v1.schema.json", transport="inline", data_b64="AAH/")
        send(proc, "data.read", "agent-data-v1.schema.json", artifact_ref=buffer["artifact_ref"], offset=0, limit=3)
        durable = send(proc, "data.promote", "agent-artifact-v1.schema.json", artifact_ref=buffer["artifact_ref"])
        proof = json.loads(Path(directory, ".jash/agent_artifact_receipts", durable["receipt_ref"]).read_bytes())
        validators["agent-artifact-receipt-v1.schema.json"].validate(json.loads(base64.b64decode(proof["payload"]))); count += 1
        send(proc, "data.release", "agent-data-v1.schema.json", artifact_ref=buffer["artifact_ref"])
    finally: stop(proc)
    proc = start()
    try:
        attached = send(proc, "data.attach", "agent-data-v1.schema.json", artifact_ref=durable["artifact_ref"])
        output = send(proc, "data.read", "agent-data-v1.schema.json", artifact_ref=attached["artifact_ref"], offset=0, limit=3)
        if output["data_b64"] != "AAH/": raise RuntimeError(output)
        send(proc, "data.release", "agent-data-v1.schema.json", artifact_ref=attached["artifact_ref"])
    finally: stop(proc)
    return count

def main():
    root = Path(__file__).resolve().parents[1]
    build = Path(os.environ.get("JASH_BUILD_DIR", root / "build")).resolve()
    validators = {}
    for path in sorted((root / "schemas").glob("agent-*.schema.json")):
        schema = json.loads(path.read_text())
        Draft202012Validator.check_schema(schema)
        validators[path.name] = Draft202012Validator(schema)
    count = 0
    with tempfile.TemporaryDirectory(prefix="jash-schema-") as temporary:
        env = {k: v for k, v in os.environ.items() if not k.startswith("JASH_")}
        env.update(HOME=temporary, JASH_SIGIL="0", JASH_ADOPT="0", JASH_NUDGE="0")
        count += local_control_document(build, Path(temporary), env,
            validators["agent-local-control-v1.schema.json"],
            validators["agent-local-data-v1.schema.json"],
            validators["agent-stream-v1.schema.json"])
        start = {"id": "schema-session-job", "protocol_version": 2, "op": "job.start", "argv": ["/usr/bin/printf", "observation"],
                 "background": True, "caps": ["effect:exec", "job:create"]}
        execution = subprocess.run([str(build / "jash-agentd")], input=(json.dumps(start) + "\n").encode(), cwd=temporary,
                                   env=env, capture_output=True, check=True, timeout=20)
        job_id = json.loads(execution.stdout.splitlines()[-1])["job_id"]
        for identifier, available in ((job_id, True), ("j-" + "0" * 32, False)):
            request = {"id": "schema-result", "protocol_version": 2, "op": "job.result", "job_id": identifier, "caps": ["job:read"]}
            result = subprocess.run([str(build / "jash-agentd")], input=(json.dumps(request) + "\n").encode(), cwd=temporary,
                                    env=env, capture_output=True, check=True, timeout=20)
            document = json.loads(result.stdout.splitlines()[-1])
            validators["agent-job-result-v1.schema.json"].validate(document)
            if document["available"] != available: raise RuntimeError("job observation availability mismatch")
            count += 1
        store_state = None
        for operation, fields in (("store.inspect", {}), ("store.gc", {"dry_run": True, "older_than_seconds": 0}),
                                  ("store.quota", {"max_bytes": 0, "dry_run": True}), ("store.release", {"dry_run": True}),
                                  ("store.journal.quota", {"max_bytes": 64 * 1024 * 1024}),
                                  ("store.journal.gc", {"dry_run": True, "older_than_seconds": 0})):
            if operation == "store.release":
                root_entry = next(item for item in store_state["items"] if item["kind"] == "job_archive")
                fields.update(root_namespace=root_entry["namespace"], root_name=root_entry["name"], root_blake3=root_entry["root_blake3"])
            request = {"id": "schema-" + operation, "protocol_version": 2, "op": operation, "caps": ["store:read", "store:manage"], **fields}
            result = subprocess.run([str(build / "jash-agentd")], input=(json.dumps(request) + "\n").encode(), cwd=temporary,
                                    env=env, capture_output=True, check=True, timeout=20)
            document = json.loads(result.stdout.splitlines()[-1])
            validators["agent-store-v1.schema.json"].validate(document)
            if not document["ok"]: raise RuntimeError(document)
            count += 1
            if operation == "store.inspect": store_state = document
            else:
                proof = json.loads(Path(temporary, ".jash/agent_maintenance", document["receipt_ref"]).read_bytes())
                payload = json.loads(base64.b64decode(proof["payload"]))
                validators["agent-maintenance-receipt-v1.schema.json"].validate(payload); count += 1
                intent = json.loads(Path(temporary, ".jash/agent_maintenance", payload["intent_ref"]).read_bytes())
                validators["agent-maintenance-receipt-v1.schema.json"].validate(json.loads(base64.b64decode(intent["payload"]))); count += 1
                decision = json.loads(Path(temporary, ".jash/agent_maintenance", payload["decision_ref"]).read_bytes())
                validators["agent-maintenance-receipt-v1.schema.json"].validate(json.loads(base64.b64decode(decision["payload"]))); count += 1
        journal = Path(temporary, ".jash/agent_maintenance")
        public = next(path for path in journal.glob("*.result") if json.loads(path.read_bytes())["id"] == "schema-store.quota")
        public.rename(journal / ("." + public.name + ".0.0.tmp"))
        for target, preview, status in (("missing", False, "unresolved"), ("schema-store.quota", True, "recoverable"),
                                         ("schema-store.quota", False, "recovered"), ("schema-store.quota", False, "already_committed")):
            request = {"id": "schema-resolution", "protocol_version": 2, "op": "store.resolve", "target_id": target,
                       "dry_run": preview, "caps": ["store:manage"]}
            result = subprocess.run([str(build / "jash-agentd")], input=(json.dumps(request) + "\n").encode(), cwd=temporary,
                                    env=env, capture_output=True, check=True, timeout=20)
            document = json.loads(result.stdout.splitlines()[-1])
            validators["agent-maintenance-resolution-v1.schema.json"].validate(document)
            if document["status"] != status: raise RuntimeError(document)
            count += 1
            inspect = {"id": "schema-target-inspection", "protocol_version": 2, "op": "store.inspect", "target_id": target, "caps": ["store:read"]}
            result = subprocess.run([str(build / "jash-agentd")], input=(json.dumps(inspect) + "\n").encode(), cwd=temporary,
                                    env=env, capture_output=True, check=True, timeout=20)
            view = json.loads(result.stdout.splitlines()[-1])
            validators["agent-maintenance-inspection-v1.schema.json"].validate(view)
            expected = "unresolved" if target == "missing" else "recoverable" if preview else "committed"
            if view["state"] != expected: raise RuntimeError(view)
            count += 1
        hook = Path(temporary, "admission-fault.so")
        subprocess.run(["cc", "-shared", "-fPIC", "-o", str(hook), str(root / "tests/runtime_cancel_faults.c"), "-ldl"], check=True, capture_output=True, timeout=30)
        original = {"id": "schema-close-preparation", "protocol_version": 2, "op": "store.quota", "max_bytes": 0, "caps": ["store:manage"]}
        for request, extra, schema in (
            (original, dict(LD_PRELOAD=str(hook), JASH_TEST_MAINT_ADMISSION_FAIL="1"), "agent-store-v1.schema.json"),
            (dict(id="inspect-preparation", protocol_version=2, op="store.inspect", target_id=original["id"], caps=["store:read"]), {}, "agent-maintenance-inspection-v1.schema.json"),
            (dict(id="preview-close", protocol_version=2, op="store.resolve", target_id=original["id"], dry_run=True, caps=["store:manage"]), {}, "agent-maintenance-resolution-v1.schema.json"),
            (dict(id="close", protocol_version=2, op="store.resolve", target_id=original["id"], caps=["store:manage"]), {}, "agent-maintenance-resolution-v1.schema.json"),
            (original, {}, "agent-store-v1.schema.json")):
            output = subprocess.run([str(build / "jash-agentd")], input=(json.dumps(request) + "\n").encode(), cwd=temporary,
                                    env=dict(env, **extra), capture_output=True, check=True, timeout=20)
            document = json.loads(output.stdout.splitlines()[-1]); validators[schema].validate(document); count += 1
        if document["effect_admission"] != "closed" or document["applied"] or document["error"] != "effects_not_admitted": raise RuntimeError(document)
        proof = json.loads((journal / document["receipt_ref"]).read_bytes()); payload = json.loads(base64.b64decode(proof["payload"]))
        validators["agent-maintenance-receipt-v1.schema.json"].validate(payload); count += 1
        decision = json.loads((journal / payload["decision_ref"]).read_bytes())
        validators["agent-maintenance-receipt-v1.schema.json"].validate(json.loads(base64.b64decode(decision["payload"]))); count += 1
        def history_page(**fields):
            request = dict(id="schema-history", protocol_version=2, op="store.history", caps=["store:read"], **fields)
            output = subprocess.run([str(build / "jash-agentd")], input=(json.dumps(request) + "\n").encode(), cwd=temporary,
                                    env=env, capture_output=True, check=True, timeout=20)
            page = json.loads(output.stdout.splitlines()[-1])
            validators["agent-maintenance-history-v1.schema.json"].validate(page)
            return page
        first = history_page(limit=1); count += 1
        if not first["ok"] or not first["next_cursor"]: raise RuntimeError(first)
        page = history_page(limit=1, cursor=first["next_cursor"]); count += 1
        if page["snapshot"] != first["snapshot"]: raise RuntimeError(page)
        orphan = journal / "schema-unattributed"; orphan.write_bytes(b"unattributed"); orphan.chmod(0o600)
        page = history_page(limit=1, cursor=first["next_cursor"]); count += 1
        if page["error"] != "cursor_stale": raise RuntimeError(page)
        page = history_page(limit=64); count += 1
        if not any(item["state"] == "unattributed" for item in page["items"]): raise RuntimeError(page)
        for operation, fields, fault, expected in (
            ("store.journal.quota", {"max_bytes": 1}, {}, "journal_quota_exceeded"),
            ("store.quota", {"max_bytes": 0}, dict(LD_PRELOAD=str(hook), JASH_TEST_JOURNAL_SPACE="bytes"), "journal_space_insufficient")):
            request = dict(id="schema-pressure-" + operation, protocol_version=2, op=operation, caps=["store:manage"], **fields)
            output = subprocess.run([str(build / "jash-agentd")], input=(json.dumps(request) + "\n").encode(), cwd=temporary,
                                    env=dict(env, **fault), capture_output=True, check=True, timeout=20)
            document = json.loads(output.stdout.splitlines()[-1]); validators["agent-store-v1.schema.json"].validate(document)
            if document["error"] != expected or document["applied"] or document["transaction"] != "not_started": raise RuntimeError(document)
            count += 1
        for ident, preview, fault in (("preview", True, {}), ("hold", False, dict(LD_PRELOAD=str(hook), JASH_TEST_JOURNAL_GC_UNLINK_FAIL="1")), ("reconcile", False, {})):
            request = dict(id="schema-journal-gc-" + ident, protocol_version=2, op="store.journal.gc", older_than_seconds=0, dry_run=preview, caps=["store:manage"])
            output = subprocess.run([str(build / "jash-agentd")], input=(json.dumps(request) + "\n").encode(), cwd=temporary,
                                    env=dict(env, **fault), capture_output=True, check=True, timeout=20)
            document = json.loads(output.stdout.splitlines()[-1]); validators["agent-store-v1.schema.json"].validate(document); count += 1
            if ident == "preview" and document["journal_gc"]["planned_files"] < 1: raise RuntimeError(document)
            if ident == "hold" and document["error"] != "journal_gc_incomplete": raise RuntimeError(document)
            if ident == "reconcile" and document["journal_gc"]["reclaimed_files"] < 1: raise RuntimeError(document)
        count += data_documents(build, temporary, env, validators)
        for name in ("jash", "jash-agentd"):
            for selector in (None, "runtime:job.read", "invocation:serve", "invocation:exec-json", "invocation:listen"):
                command = [str(build / name), "--contract-json"]
                if selector: command.append(selector)
                data = json.loads(subprocess.check_output(command, cwd=temporary, env=env, timeout=15))
                schema = "agent-contract-invocation-v1.schema.json" if selector and selector.startswith("invocation:") else "agent-contract-inventory-v1.schema.json"
                validators[schema].validate(data)
                for operation in data.get("operations", []):
                    if operation.get("input_schema"):
                        Draft202012Validator.check_schema(operation["input_schema"])
                count += 1
        Path(temporary, "source").write_bytes(b"data")
        for argv, rc in ((["cp", "--json", "--verify", "source", "quote\"line\n"], 0),
                         (["cp", "--json", "source", "source"], 1),
                         (["mv", "--json", "--dry-run", "source", "target"], 0),
                         (["mv", "--json", "source", "target"], 0),
                         (["rm", "--json", "missing", "target"], 1),
                         (["rm", "--json", "-f", "missing"], 0),
                         (["touch", "--json", "metadata-file"], 0),
                         (["mkdir", "--json", "-p", "metadata-dir/sub"], 0),
                         (["rmdir", "--json", "metadata-dir/sub"], 0),
                         (["chmod", "--json", "0640", "metadata-file"], 0),
                         (["chown", "--json", str(os.getuid()), "metadata-file"], 0),
                         (["ln", "--json", "metadata-file", "metadata-link"], 0),
                         (["sed", "--json", "-i", "s/data/new/", "quote\"line\n"], 0)):
            result = subprocess.run([str(build / "jash"), "-c", shlex.join(argv)], cwd=temporary,
                                    env=env, capture_output=True, timeout=15)
            if result.returncode != rc: raise RuntimeError((argv, result.returncode, result.stderr))
            mutation = json.loads(result.stdout)
            validators["agent-mutation-v1.schema.json"].validate(mutation)
            count += 1
        reference = mutation["items"][0]["undo"]
        if reference is None: raise RuntimeError("sed did not export its qualified recovery reference")
        for dry, rc in ((True, 0), (False, 0), (False, 1)):
            argv = ["undo", "--json", "--ref", reference["ref"]]
            if dry: argv.append("--dry-run")
            result = subprocess.run([str(build / "jash"), "-c", shlex.join(argv)], cwd=temporary,
                                    env=env, capture_output=True, timeout=15)
            if result.returncode != rc: raise RuntimeError((argv, result.returncode, result.stderr))
            validators["agent-undo-result-v1.schema.json"].validate(json.loads(result.stdout))
            count += 1
        result = subprocess.run([str(build / "jash"), "-c", shlex.join(["undo", "--json", "--inspect", "--ref", reference["ref"]])],
                                cwd=temporary, env=env, capture_output=True, check=True, timeout=15)
        validators["agent-undo-result-v1.schema.json"].validate(json.loads(result.stdout))
        count += 1
        for command, schema in (
            ("df --json-report -i --total .", "agent-query-df-v1.schema.json"),
            ("df --json-report --baseline df-base .", "agent-query-df-v1.schema.json"),
            ("df --json-report --delta df-base .", "agent-query-df-v1.schema.json"),
            ("stat --json-report metadata-file", "agent-query-stat-v1.schema.json"),
            ("wc --json-report metadata-file", "agent-query-wc-v1.schema.json"),
            ("mv --json --atomic --dry-run metadata-file atomic-target", "agent-mutation-v1.schema.json"),
            ("mv --json --atomic metadata-file atomic-target", "agent-mutation-v1.schema.json"),
            ("checkpoint schema-fixture > /dev/null; checkpoint --json", "agent-checkpoints-v1.schema.json"),
            ("trash gc --json --dry-run", "agent-trash-gc-v1.schema.json"),
            ("mkdir timeline-fixture; timeline --json", "agent-timeline-v1.schema.json"),
            ("gossip --channel schema send payload", "agent-gossip-v1.schema.json"),
            ("gossip --channel schema receive", "agent-gossip-v1.schema.json"),
            ("undo --json --store", "agent-recovery-store-v1.schema.json"),
            ("undo --json --gc --dry-run", "agent-recovery-store-v1.schema.json"),
            ("undo --json --usage", "agent-recovery-quota-v1.schema.json"),
            ("undo --json --quota 0 --dry-run", "agent-recovery-quota-v1.schema.json"),
        ):
            result = subprocess.run([str(build / "jash"), "-c", command], cwd=temporary, env=env,
                                    capture_output=True, check=True, timeout=15)
            validators[schema].validate(json.loads(result.stdout))
            count += 1
        request = {"id": "schema-mirror", "argv": ["echo", "receipt"], "caps": ["effect:read"]}
        result = subprocess.run([str(build / "jash-agentd")], cwd=temporary, env=env,
                                input=(json.dumps(request) + "\n").encode(), capture_output=True, check=True, timeout=20)
        response = json.loads(result.stdout.splitlines()[-1])
        command = ["mirror", "--json", "--receipt", response["receipt_ref"], "--blake3", response["receipt_blake3"], "--pubkey", response["receipt_pubkey"]]
        result = subprocess.run([str(build / "jash"), "-c", shlex.join(command)], cwd=temporary, env=env,
                                capture_output=True, check=True, timeout=20)
        validators["agent-mirror-receipt-v1.schema.json"].validate(json.loads(result.stdout))
        count += 1
        request_path = Path(temporary, "mirror-request.json")
        response_path = Path(temporary, "mirror-response.json")
        request_path.write_text(json.dumps(request) + "\n")
        # Preserve the exact producer response rather than reserializing it.
        original = subprocess.run([str(build / "jash-agentd")], cwd=temporary, env=env,
                                  input=(json.dumps(request) + "\n").encode(), capture_output=True, check=True, timeout=20)
        response_path.write_bytes(original.stdout.splitlines(keepends=True)[-1])
        full = command + ["--request", str(request_path), "--response", str(response_path)]
        result = subprocess.run([str(build / "jash"), "-c", shlex.join(full)], cwd=temporary, env=env,
                                capture_output=True, check=True, timeout=20)
        validators["agent-mirror-receipt-v1.schema.json"].validate(json.loads(result.stdout))
        count += 1
        env["JASH_AGENT"] = "1"
        result = subprocess.run([str(build / "jash"), "-c", "true"], cwd=temporary, env=env,
                                capture_output=True, check=True, timeout=15)
        for line in result.stderr.splitlines():
            if line.startswith(b'{"jash_agent":'):
                validators["agent-envelope-v2.schema.json"].validate(json.loads(line))
                count += 1
    fixture = runpy.run_path(str(root / "tests/test_agent_json_roundtrip.py"), run_name="qualification")
    serializer = fixture["Serializer"]
    serializer.setUpClass()
    try:
        data = json.loads(serializer().result())
        validators["agent-envelope-v2.schema.json"].validate(data)
        count += 1
    finally:
        serializer.doClassCleanups()
    print(f"Draft 2020-12: {len(validators)} document schemas and {count} real output documents validated")


if __name__ == "__main__":
    main()
